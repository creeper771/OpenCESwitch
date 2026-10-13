/*
HOST.H

The Switch host: a homebrew program (NRO, libnx) that runs the guest image
of the Android port (port/android: the game, compiled for ILP32 AArch64,
linked at 0x88000000) on Horizon. It does what port/android/host does on
Android, with Horizon's calls:

- host_memory.c: the guest's memory below 4 GB. Horizon places a process's
  heap, alias and stack regions above 4 GB, so every guest mapping is heap
  memory mapped again with svcMapProcessCodeMemory, as the homebrew loader
  maps an NRO (port/switch/feasibility/memory_probe).
- host_loader.c: loads the image and fills its import table.
- host_thread.c: guest threads, each on a stack below 4 GB.
- host_syscall.c: the guest musl's Linux system calls, on newlib and libnx.
- host_imports.c: the host functions the guest imports, by name.
*/

#ifndef SWITCH_HOST_H
#define SWITCH_HOST_H

#include <switch.h>

#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

#define HOST_PAGE 0x1000ULL
#define HOST_LOW_LIMIT 0x100000000ULL

/* the data root on the SD card (the guest's maps/, config.toml, debug.txt) */
#define HOST_DATA_ROOT "/switch/opence"
#define HOST_SAVE_ROOT "/switch/opence/save"
/* the host's own log, beside the guest's debug.txt */
#define HOST_LOG_PATH "sdmc:/switch/opence/host.txt"

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

enum
{
	HOST_LOG_INFO,
	HOST_LOG_WARN,
	HOST_LOG_ERROR
};

/* what the guest has done, for host_main.c's heartbeat */
struct host_activity
{
	volatile uint64_t system_calls;
	volatile uint64_t input_polls;
	volatile uint64_t frames;
	/* threads in a futex wait, and the last address waited on */
	volatile int32_t waiting;
	volatile uint32_t last_wait_address;
	/* where the guest's time went (nanoseconds, all threads together) */
	volatile uint64_t read_bytes;
	volatile uint64_t read_time;
	volatile uint64_t sleep_time;
	volatile uint64_t wait_time;
	volatile uint64_t mapping_time;
	/* the renderer's: texture write tracking (hashing), the end-of-frame
	fence waits, buffer readbacks, and eglSwapBuffers */
	volatile uint64_t hash_time;
	volatile uint64_t fence_time;
	volatile uint64_t readback_time;
	volatile uint64_t swap_time;
};

static inline uint64_t host_now(void)
{
	return armTicksToNs(armGetSystemTick());
}

#define HOST_ADD(field, amount) __atomic_add_fetch(&host_activity.field, (amount), __ATOMIC_RELAXED)

extern struct host_activity host_activity;

/* host_main.c */
void host_logf(int level, const char *format, ...) __attribute__((format(printf, 2, 3)));
void host_fatal(const char *format, ...) __attribute__((noreturn, format(printf, 1, 2)));
void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));
void host_exit(int code) __attribute__((noreturn));
void host_android_path(int which, char *buffer, uint32_t size);
void host_console_close(void);

/* host_gl.c */
void *host_gl_resolve(const char *name);
uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags);
void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height);
int host_sdl_set_relative_mouse(uint32_t window, int enabled);
int host_sdl_gl_set_attribute(int attribute, int value);
uint32_t host_sdl_gl_create_context(uint32_t window);
int host_sdl_gl_make_current(uint32_t window, uint32_t context);
int host_sdl_gl_set_swap_interval(int interval);
int host_sdl_gl_swap_window(uint32_t window);
int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message);
void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size);
int host_gl_has_extension(const char *name);
void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data);
void host_gl_fence_frame(uint32_t slot);
void host_gl_wait_frame(uint32_t slot);
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data);

/* host_memory.c */
int host_memory_initialize(void);
/* maps a fixed range below 4 GB from heap memory: source, if not NULL, is
the heap memory to map (the loader's image), else new memory is allocated.
With read_write the range becomes read/write; without it, it keeps the
mapping's first permission so that code pages can still become read/execute
(host_set_permission), as a page once writable cannot. */
int host_map_fixed(uint64_t address, uint64_t size, void *source, int read_write, const char *what);
int host_set_permission(uint64_t address, uint64_t size, uint32_t permission);
void *host_low_map(uint64_t size);
void host_low_unmap(void *address, uint64_t size);
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
void host_memory_watch_initialize(void);
void host_memory_watch_protect(uint32_t address, uint32_t size);
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size);
uint32_t host_memory_watch_serial(void);
void host_memory_watch_prepare_write(uint32_t address, uint32_t size);
void host_memory_watch_forget(uint32_t address, uint32_t size);
void host_memory_watch_begin_frame(void);

/* host_loader.c */
struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base;
	uint32_t end;
};

extern struct host_guest_image host_image;

int host_load_image(const void *file, size_t size);

/* host_thread.c */
uint32_t host_get_tp(void);
void host_set_tp(uint32_t thread);
int host_thread_create(uint32_t guest_thread, uint32_t stack_size);
int host_run_guest_main(uint32_t boot);
int host_guest_exited(int code);
void host_threads_stop(void);
uint64_t host_thread_id(void);
int host_native_thread_create(void (*function)(void *), void *argument, uint32_t stack_size, int priority);
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* host_audio.c */
uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata);
int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length);
int host_sdl_resume_audio_stream_device(uint32_t stream);
int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length);
int host_sdl_get_audio_stream_available(uint32_t stream);
void host_sdl_destroy_audio_stream(uint32_t stream);

/* host_stack.s: calls function(argument) with the stack pointer at
stack_top, and returns to the caller's stack afterwards */
void host_call_on_stack(uint64_t argument, void (*function)(uint64_t), void *stack_top);

/* host_syscall.c */
long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f);
int host_errno(void);
int host_linux_errno(int error);
struct posix_file_information;
int host_posix_fstat(int descriptor, struct posix_file_information *information);
int host_posix_seek(int descriptor, int offset_low, int offset_high, int whence,
	unsigned int *position_low, unsigned int *position_high);
int host_posix_truncate(int descriptor, unsigned int size_low, unsigned int size_high);

/* an import the host implements, by name */
struct host_import
{
	const char *name;
	void *function;
};

/* host_input.c: controllers as SDL gamepads, and no touch, keyboard or
clipboard */
extern const struct host_import host_input_imports[];
int host_input_poll_event(void *event);

/* host_network.c: as without a network, until the host has sockets */
extern const struct host_import host_network_imports[];

/* the shutdown, once the game has exited (host_main.c) */
void host_audio_stop(void);
void host_gl_stop(void);
void host_memory_release(void);
void host_files_close(void);
void host_profile_stop(void);

/* host_profile.c */
void host_profile_start(void);
void host_profile_report(void);
void host_profile_syscall(long long number);

/* host_imports.c */
void *host_resolve_import(const char *name, uint32_t index);
uint32_t host_missing_import_count(void);

#endif
