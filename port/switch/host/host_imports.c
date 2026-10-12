/*
HOST_IMPORTS.C

The host functions the guest image imports (port/android/host_imports.list,
and the generated OpenGL ES and posix_*.c lists), by name. An import not
implemented here yet gets a stub (host_stubs.s) that logs its name the first
time the guest calls it and returns 0, so a run shows what the game needs
next.
*/

#include "host.h"
#include "../../linux/src/posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_IMPORTS 512
#define STUB_SIZE 8

extern const char host_import_stubs[];

static const char *missing_names[MAXIMUM_IMPORTS];
static uint8_t missing_reported[MAXIMUM_IMPORTS];
static uint32_t missing_count;

uint64_t host_missing_import_called(uint32_t index);

uint64_t host_missing_import_called(uint32_t index)
{
	if (index < MAXIMUM_IMPORTS && !__atomic_exchange_n(&missing_reported[index], 1, __ATOMIC_RELAXED))
		host_logf(HOST_LOG_WARN, "the guest called %s, which the Switch host does not implement yet",
			missing_names[index] ? missing_names[index] : "(unknown)");
	return 0;
}

uint32_t host_missing_import_count(void)
{
	return missing_count;
}

/* ---------- the few SDL functions that need no SDL */

static long long sdl_ticks(void)
{
	return (long long)(armTicksToNs(armGetSystemTick()) / 1000000ULL);
}

static long long sdl_thread_id(void)
{
	return (long long)host_thread_id();
}

static void sdl_get_error(char *buffer, uint32_t size)
{
	if (size)
		snprintf(buffer, size, "not available on the Switch yet");
}

/* SDL "starts": the host itself is what the guest's SDL calls reach */
static int sdl_init(uint32_t flags)
{
	(void)flags;
	return 1;
}

static int sdl_set_hint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return 1;
}

/* no events yet (no input): counted for the heartbeat, as the game polls
once a frame */
static int sdl_poll_event(void *event)
{
	(void)event;
	__atomic_add_fetch(&host_activity.input_polls, 1, __ATOMIC_RELAXED);
	return 0;
}

/* ---------- the table */

static const struct
{
	const char *name;
	void *function;
} implemented[] =
{
	{"host_syscall", (void *)host_syscall},
	{"host_log", (void *)host_log},
	{"host_abort", (void *)host_abort},
	{"host_exit", (void *)host_exit},
	{"host_errno", (void *)host_errno},
	{"host_get_tp", (void *)host_get_tp},
	{"host_set_tp", (void *)host_set_tp},
	{"host_thread_create", (void *)host_thread_create},
	{"host_memory_watch_initialize", (void *)host_memory_watch_initialize},
	{"host_memory_watch_protect", (void *)host_memory_watch_protect},
	{"host_memory_watch_generation", (void *)host_memory_watch_generation},
	{"host_memory_watch_serial", (void *)host_memory_watch_serial},
	{"host_memory_watch_prepare_write", (void *)host_memory_watch_prepare_write},
	{"host_memory_watch_forget", (void *)host_memory_watch_forget},
	{"host_memory_watch_begin_frame", (void *)host_memory_watch_begin_frame},
	{"host_android_path", (void *)host_android_path},
	{"host_sdl_ticks", (void *)sdl_ticks},
	{"host_sdl_thread_id", (void *)sdl_thread_id},
	{"host_sdl_get_error", (void *)sdl_get_error},
	{"host_sdl_poll_event", (void *)sdl_poll_event},
	{"host_sdl_init", (void *)sdl_init},
	{"host_sdl_set_hint", (void *)sdl_set_hint},
	/* host_audio.c */
	{"host_sdl_open_audio_stream", (void *)host_sdl_open_audio_stream},
	{"host_sdl_put_audio_stream_data", (void *)host_sdl_put_audio_stream_data},
	{"host_sdl_resume_audio_stream_device", (void *)host_sdl_resume_audio_stream_device},
	{"host_sdl_get_audio_stream_data", (void *)host_sdl_get_audio_stream_data},
	{"host_sdl_get_audio_stream_available", (void *)host_sdl_get_audio_stream_available},
	{"host_sdl_destroy_audio_stream", (void *)host_sdl_destroy_audio_stream},
	/* port/linux/src/posix_files.c, as on Android; the functions that take
	a descriptor translate the guest's first (host_syscall.c) */
	{"hostposix_stat", (void *)posix_stat},
	{"hostposix_fstat", (void *)host_posix_fstat},
	{"hostposix_set_file_times", (void *)posix_set_file_times},
	{"hostposix_seek", (void *)host_posix_seek},
	{"hostposix_truncate", (void *)host_posix_truncate},
	{"hostposix_disk_space", (void *)posix_disk_space},
	{"hostposix_set_read_only", (void *)posix_set_read_only},
	{"hostposix_make_directory", (void *)posix_make_directory},
	{"hostposix_directory_open", (void *)posix_directory_open},
	{"hostposix_directory_next", (void *)posix_directory_next},
	{"hostposix_directory_close", (void *)posix_directory_close},
	{"hostposix_find_entry_case_insensitive", (void *)posix_find_entry_case_insensitive},
};

void *host_resolve_import(const char *name, uint32_t index)
{
	size_t entry;

	for (entry = 0; entry < sizeof(implemented) / sizeof(implemented[0]); entry++)
	{
		if (!strcmp(implemented[entry].name, name))
			return implemented[entry].function;
	}
	missing_count++;
	if (index >= MAXIMUM_IMPORTS)
		host_fatal("the guest image has more imports than the Switch host's stubs (%u)", MAXIMUM_IMPORTS);
	/* (a copy: name is in the image's heap memory, which the loader then
	maps at the image's address, out of reach here) */
	missing_names[index] = strdup(name);
	return (void *)(host_import_stubs + index * STUB_SIZE);
}
