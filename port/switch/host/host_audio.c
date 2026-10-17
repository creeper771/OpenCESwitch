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

A stream without a callback is the microphone's, for voice chat
(port/linux/src/voice_audio.c: 48 kHz, mono, 32-bit float, read with
SDL_GetAudioStreamData): libnx's audin, the system's default input (a
headset's microphone in the headphone jack, or a USB one), captured on a
host thread of its own into the stream, as the game's format, which the
game takes from it. Opening it fails, as on a machine without a
microphone, if the system has no input.
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
/* the microphone's audin buffers (about 21 ms each, a voice frame's worth,
stereo 16-bit as the system gives them), and how much of it the stream
keeps for the game at most (a second of mono float: older is dropped) */
#define IN_FRAMES 1024
#define IN_BUFFER_COUNT 4
#define IN_BUFFER_BYTES ((IN_FRAMES * 2 * sizeof(int16_t) + 0xfff) & ~(size_t)0xfff)
#define IN_KEPT_BYTES (RATE * 2 * (int)sizeof(float))
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
	/* what the guest put into the stream, not yet queued (the microphone's:
	what it captured, not yet taken) */
	float *pending;
	int pending_bytes;
	int pending_size;
	AudioOutBuffer buffers[BUFFER_COUNT];
	/* the microphone's: its channels, as the guest asked, and its buffers */
	int recording;
	int channels;
	AudioInBuffer in_buffers[IN_BUFFER_COUNT];
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

/* ---------- the microphone */

static int audin_ready;

/* a captured buffer, as the guest's samples, onto the stream's end */
static void capture(struct audio_stream *stream, const AudioInBuffer *buffer)
{
	const int16_t *input = buffer->buffer;
	int channels = (int)audinGetChannelCount();
	int frames, frame, kept;
	float *output;

	if (channels < 1 || audinGetPcmFormat() != PcmFormat_Int16)
		return;
	frames = (int)(buffer->data_size / ((size_t)channels * sizeof(int16_t)));
	mutexLock(&stream->lock);
	if (stream->pending_bytes + frames * stream->channels * (int)sizeof(float) > stream->pending_size)
	{
		int size = IN_KEPT_BYTES + IN_FRAMES * 2 * (int)sizeof(float);
		float *pending = realloc(stream->pending, (size_t)size);

		if (!pending)
		{
			mutexUnlock(&stream->lock);
			return;
		}
		stream->pending = pending;
		stream->pending_size = size;
	}
	/* (the oldest dropped, if the game has not taken it) */
	kept = IN_KEPT_BYTES - frames * stream->channels * (int)sizeof(float);
	if (stream->pending_bytes > kept)
	{
		int dropped = (stream->pending_bytes - kept) / (int)sizeof(float) * (int)sizeof(float);

		memmove(stream->pending, (char *)stream->pending + dropped, (size_t)(stream->pending_bytes - dropped));
		stream->pending_bytes -= dropped;
	}
	output = (float *)((char *)stream->pending + stream->pending_bytes);
	for (frame = 0; frame < frames; frame++)
	{
		const int16_t *sample = input + frame * channels;
		float left = sample[0] / 32768.0f, right = channels > 1 ? sample[1] / 32768.0f : left;

		if (stream->channels == 1)
			*output++ = (left + right) * 0.5f;
		else
		{
			*output++ = left;
			*output++ = right;
		}
	}
	stream->pending_bytes += frames * stream->channels * (int)sizeof(float);
	mutexUnlock(&stream->lock);
}

static void record_thread(void *context)
{
	struct audio_stream *stream = context;
	int index;

	for (index = 0; index < IN_BUFFER_COUNT; index++)
		audinAppendAudioInBuffer(&stream->in_buffers[index]);
	audinStartAudioIn();
	while (!stream->quit)
	{
		AudioInBuffer *released = NULL;
		u32 count = 0;

		if (R_FAILED(audinWaitCaptureFinish(&released, &count, 100000000ULL)))
			continue;
		while (count && released && !stream->quit)
		{
			if (stream->running)
				capture(stream, released);
			/* (its size is how much to capture next) */
			released->data_size = IN_FRAMES * audinGetChannelCount() * sizeof(int16_t);
			audinAppendAudioInBuffer(released);
			released = NULL;
			count = 0;
			if (R_FAILED(audinGetReleasedAudioInBuffer(&released, &count)))
				break;
		}
	}
	/* (stopping releases the buffers, which may then be freed) */
	audinStopAudioIn();
	audinExit();
	mutexLock(&streams_lock);
	audin_ready = 0;
	streams[stream->handle - 1] = NULL;
	mutexUnlock(&streams_lock);
	for (index = 0; index < IN_BUFFER_COUNT; index++)
		free(stream->in_buffers[index].buffer);
	free(stream->pending);
	free(stream);
}

/* the microphone's stream (one at a time), 48 kHz float, mono or stereo */
static uint32_t open_recording(const struct guest_audio_spec *format)
{
	struct audio_stream *stream;
	uint32_t handle = 0;
	Result rc;
	int index;

	if (!format || format->format != SDL_AUDIO_F32 || format->channels < 1 || format->channels > 2 ||
		format->frequency != RATE)
	{
		host_logf(HOST_LOG_WARN, "audio: only 48 kHz float input is supported");
		return 0;
	}
	mutexLock(&streams_lock);
	for (index = 0; index < STREAM_COUNT && !handle; index++)
	{
		if (!streams[index])
			handle = (uint32_t)index + 1;
	}
	if (!handle || audin_ready)
	{
		mutexUnlock(&streams_lock);
		return 0;
	}
	rc = audinInitialize();
	if (R_FAILED(rc))
	{
		mutexUnlock(&streams_lock);
		host_logf(HOST_LOG_WARN, "audio: no microphone (audin 0x%x): a headset in the headphone jack, or a USB one, "
			"is the Switch's", rc);
		return 0;
	}
	if (audinGetSampleRate() != RATE || audinGetPcmFormat() != PcmFormat_Int16)
	{
		host_logf(HOST_LOG_WARN, "audio: the microphone gives %u Hz, format %d: only 48 kHz 16-bit is taken",
			audinGetSampleRate(), (int)audinGetPcmFormat());
		audinExit();
		mutexUnlock(&streams_lock);
		return 0;
	}
	stream = calloc(1, sizeof(*stream));
	if (!stream)
	{
		audinExit();
		mutexUnlock(&streams_lock);
		return 0;
	}
	stream->handle = handle;
	stream->recording = 1;
	stream->channels = format->channels;
	mutexInit(&stream->lock);
	for (index = 0; index < IN_BUFFER_COUNT; index++)
	{
		stream->in_buffers[index].buffer = aligned_alloc(0x1000, IN_BUFFER_BYTES);
		stream->in_buffers[index].buffer_size = IN_BUFFER_BYTES;
		stream->in_buffers[index].data_size = IN_FRAMES * audinGetChannelCount() * sizeof(int16_t);
		if (!stream->in_buffers[index].buffer)
		{
			while (index >= 0)
				free(stream->in_buffers[index--].buffer);
			free(stream);
			audinExit();
			mutexUnlock(&streams_lock);
			return 0;
		}
	}
	streams[handle - 1] = stream;
	audin_ready = 1;
	mutexUnlock(&streams_lock);
	if (host_native_thread_create(record_thread, stream, AUDIO_STACK_SIZE, AUDIO_PRIORITY) != 0)
	{
		mutexLock(&streams_lock);
		streams[handle - 1] = NULL;
		audin_ready = 0;
		mutexUnlock(&streams_lock);
		for (index = 0; index < IN_BUFFER_COUNT; index++)
			free(stream->in_buffers[index].buffer);
		free(stream);
		audinExit();
		return 0;
	}
	host_logf(HOST_LOG_INFO, "audio: microphone through audin (%u Hz, %u channels)", audinGetSampleRate(),
		audinGetChannelCount());
	return handle;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	const struct guest_audio_spec *format = spec;
	struct audio_stream *stream;
	uint32_t handle = 0;
	Result rc;
	int index;

	(void)device;
	if (!callback)
		return open_recording(format);
	if (!format || format->format != SDL_AUDIO_F32 || format->channels != CHANNELS ||
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

/* (the microphone's: what it captured, the oldest first) */
int host_sdl_get_audio_stream_data(uint32_t handle, void *data, int length)
{
	struct audio_stream *stream = stream_of(handle);
	int taken;

	if (!stream || !stream->recording || length <= 0)
		return 0;
	mutexLock(&stream->lock);
	taken = length < stream->pending_bytes ? length : stream->pending_bytes;
	taken -= taken % (int)sizeof(float);
	memcpy(data, stream->pending, (size_t)taken);
	stream->pending_bytes -= taken;
	memmove(stream->pending, (char *)stream->pending + taken, (size_t)stream->pending_bytes);
	mutexUnlock(&stream->lock);
	return taken;
}

int host_sdl_get_audio_stream_available(uint32_t handle)
{
	struct audio_stream *stream = stream_of(handle);
	int available;

	if (!stream || !stream->recording)
		return 0;
	mutexLock(&stream->lock);
	available = stream->pending_bytes;
	mutexUnlock(&stream->lock);
	return available;
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
	if (audin_ready)
	{
		audinStopAudioIn();
		audinExit();
		audin_ready = 0;
	}
	if (!audout_ready)
		return;
	audoutStopAudioOut();
	audoutExit();
	audout_ready = 0;
}
