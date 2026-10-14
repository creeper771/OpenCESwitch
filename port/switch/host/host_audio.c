/*
HOST_AUDIO.C

The guest's sound (port/linux/src/dsound_sdl.c) through libnx's audout.

The game opens an SDL3 audio stream with a callback (48 kHz, stereo, 32-bit
float) and mixes in it, putting the samples into the stream. Without SDL,
a host thread on a guest stack (so that it can run the callback) keeps
audout's buffers filled: for each buffer it calls the game's callback for
the buffer's worth, takes what the game put into the stream
(host_sdl_put_audio_stream_data), converts it to audout's 16-bit samples
and queues it. It runs above the guest's priority, so that the game's busy
threads never starve the sound.

A stream without a callback (the microphone's, for voice chat) is not
supported: opening one fails, as on a machine without a microphone.
*/

#include "host.h"

#include <stdlib.h>
#include <string.h>

/* SDL3's SDL_AUDIO_F32 (little-endian 32-bit float) */
#define SDL_AUDIO_F32 0x8120
#define RATE 48000
#define CHANNELS 2
/* frames per audout buffer (about 11 ms), and buffers queued (about 64 ms
in all). The game's mixer holds its lock over the whole buffer, and the
game's main thread waited on that lock (dsound_begin_scene) 2.7% of its
time with 1024-frame buffers: smaller ones keep it shorter. */
#define FRAMES 512
#define BUFFER_COUNT 6
#define BUFFER_BYTES ((FRAMES * CHANNELS * sizeof(int16_t) + 0xfff) & ~(size_t)0xfff)
#define STREAM_COUNT 4
#define AUDIO_STACK_SIZE (256 * 1024)
#define AUDIO_PRIORITY 0x2c

/* the guest's SDL_AudioSpec */
struct guest_audio_spec
{
	int32_t format;
	int32_t channels;
	int32_t frequency;
};

struct audio_stream
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	volatile int running;
	volatile int quit;
	Mutex lock;
	/* what the guest put into the stream, not yet queued */
	float *pending;
	int pending_bytes;
	int pending_size;
	AudioOutBuffer buffers[BUFFER_COUNT];
};

static struct audio_stream *streams[STREAM_COUNT];
static Mutex streams_lock;
static int audout_ready;

static struct audio_stream *stream_of(uint32_t handle)
{
	return handle >= 1 && handle <= STREAM_COUNT ? streams[handle - 1] : NULL;
}

/* one buffer of the game's sound, as 16-bit samples */
static void fill(struct audio_stream *stream, int16_t *output)
{
	const int wanted = FRAMES * CHANNELS * (int)sizeof(float);
	int samples, index;

	while (stream->pending_bytes < wanted)
	{
		int before = stream->pending_bytes;

		host_call_guest(stream->callback, stream->userdata, stream->handle,
			(uint32_t)(wanted - stream->pending_bytes), (uint32_t)(wanted - stream->pending_bytes));
		if (stream->pending_bytes == before)
			break;
	}
	mutexLock(&stream->lock);
	samples = stream->pending_bytes / (int)sizeof(float);
	if (samples > FRAMES * CHANNELS)
		samples = FRAMES * CHANNELS;
	for (index = 0; index < samples; index++)
	{
		float value = stream->pending[index];

		if (value > 1.0f)
			value = 1.0f;
		else if (value < -1.0f)
			value = -1.0f;
		output[index] = (int16_t)(value * 32767.0f);
	}
	memset(output + samples, 0, (size_t)(FRAMES * CHANNELS - samples) * sizeof(int16_t));
	stream->pending_bytes -= samples * (int)sizeof(float);
	memmove(stream->pending, stream->pending + samples, (size_t)stream->pending_bytes);
	mutexUnlock(&stream->lock);
}

static void queue(struct audio_stream *stream, AudioOutBuffer *buffer)
{
	if (stream->running)
		fill(stream, buffer->buffer);
	else
		memset(buffer->buffer, 0, BUFFER_BYTES);
	buffer->data_size = FRAMES * CHANNELS * sizeof(int16_t);
	buffer->data_offset = 0;
	audoutAppendAudioOutBuffer(buffer);
}

static void audio_thread(void *context)
{
	struct audio_stream *stream = context;
	int index;

	for (index = 0; index < BUFFER_COUNT; index++)
		queue(stream, &stream->buffers[index]);
	while (!stream->quit)
	{
		AudioOutBuffer *released = NULL;
		u32 count = 0;

		if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ULL)))
			continue;
		while (count && released && !stream->quit)
		{
			queue(stream, released);
			released = NULL;
			count = 0;
			if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)))
				break;
		}
	}
	/* (stopping releases the buffers, which may then be freed) */
	audoutStopAudioOut();
	mutexLock(&streams_lock);
	streams[stream->handle - 1] = NULL;
	mutexUnlock(&streams_lock);
	for (index = 0; index < BUFFER_COUNT; index++)
		free(stream->buffers[index].buffer);
	free(stream->pending);
	free(stream);
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	const struct guest_audio_spec *format = spec;
	struct audio_stream *stream;
	uint32_t handle = 0;
	Result rc;
	int index;

	(void)device;
	if (!callback || !format || format->format != SDL_AUDIO_F32 || format->channels != CHANNELS ||
		format->frequency != RATE)
	{
		host_logf(HOST_LOG_WARN, "audio: only 48 kHz stereo float output is supported");
		return 0;
	}
	mutexLock(&streams_lock);
	for (index = 0; index < STREAM_COUNT && !handle; index++)
	{
		if (!streams[index])
			handle = (uint32_t)index + 1;
	}
	if (!handle)
	{
		mutexUnlock(&streams_lock);
		return 0;
	}
	if (!audout_ready)
	{
		rc = audoutInitialize();
		if (R_FAILED(rc))
		{
			mutexUnlock(&streams_lock);
			host_logf(HOST_LOG_ERROR, "audio: audout cannot start: 0x%x", rc);
			return 0;
		}
		audout_ready = 1;
	}
	stream = calloc(1, sizeof(*stream));
	if (!stream)
	{
		mutexUnlock(&streams_lock);
		return 0;
	}
	stream->handle = handle;
	stream->callback = callback;
	stream->userdata = userdata;
	mutexInit(&stream->lock);
	for (index = 0; index < BUFFER_COUNT; index++)
	{
		stream->buffers[index].buffer = aligned_alloc(0x1000, BUFFER_BYTES);
		stream->buffers[index].buffer_size = BUFFER_BYTES;
		if (!stream->buffers[index].buffer)
		{
			while (index >= 0)
				free(stream->buffers[index--].buffer);
			free(stream);
			mutexUnlock(&streams_lock);
			return 0;
		}
	}
	streams[handle - 1] = stream;
	mutexUnlock(&streams_lock);

	rc = audoutStartAudioOut();
	if (R_FAILED(rc) || host_native_thread_create(audio_thread, stream, AUDIO_STACK_SIZE, AUDIO_PRIORITY) != 0)
	{
		host_logf(HOST_LOG_ERROR, "audio: cannot start the sound: 0x%x", rc);
		mutexLock(&streams_lock);
		streams[handle - 1] = NULL;
		mutexUnlock(&streams_lock);
		for (index = 0; index < BUFFER_COUNT; index++)
			free(stream->buffers[index].buffer);
		free(stream);
		return 0;
	}
	host_logf(HOST_LOG_INFO, "audio: playing through audout (48 kHz, stereo)");
	return handle;
}

int host_sdl_put_audio_stream_data(uint32_t handle, const void *data, int length)
{
	struct audio_stream *stream = stream_of(handle);

	if (!stream || length < 0)
		return 0;
	mutexLock(&stream->lock);
	if (stream->pending_bytes + length > stream->pending_size)
	{
		int size = (stream->pending_bytes + length) * 2;
		float *pending = realloc(stream->pending, (size_t)size);

		if (!pending)
		{
			mutexUnlock(&stream->lock);
			return 0;
		}
		stream->pending = pending;
		stream->pending_size = size;
	}
	memcpy((char *)stream->pending + stream->pending_bytes, data, (size_t)length);
	stream->pending_bytes += length;
	mutexUnlock(&stream->lock);
	return 1;
}

int host_sdl_resume_audio_stream_device(uint32_t handle)
{
	struct audio_stream *stream = stream_of(handle);

	if (!stream)
		return 0;
	stream->running = 1;
	return 1;
}

/* (only for streams the guest reads, the microphone's, which are not
supported) */
int host_sdl_get_audio_stream_data(uint32_t handle, void *data, int length)
{
	(void)handle;
	(void)data;
	(void)length;
	return 0;
}

int host_sdl_get_audio_stream_available(uint32_t handle)
{
	(void)handle;
	return 0;
}

/* the stream's thread ends and frees it; this does not wait for it, as the
thread may be in the game's callback, waiting for a lock the caller holds */
void host_sdl_destroy_audio_stream(uint32_t handle)
{
	struct audio_stream *stream = stream_of(handle);

	if (stream)
		stream->quit = 1;
}

/* the shutdown: the streams' threads are paused (host_threads_stop), so the
output stops here, which releases their buffers */
void host_audio_stop(void)
{
	if (!audout_ready)
		return;
	audoutStopAudioOut();
	audoutExit();
	audout_ready = 0;
}
