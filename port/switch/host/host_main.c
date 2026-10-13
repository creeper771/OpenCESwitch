/*
HOST_MAIN.C

Start-up of the Switch host: checks the launch mode, loads the guest image
from the NRO's RomFS (romfs:/halo_guest.elf, the Android port's image),
gives the guest its memory, and runs the game's main on a guest stack.

The guest is the Android build of the game, so it asks the host for its
data and save folders (host_android_path) and reads its settings from the
environment as the Android host gives them: here the data root is
sdmc:/switch/opence. Until the host draws with OpenGL ES, it starts the
game without a renderer (HALO_NULL_RENDERER) and shows its log on the
screen; the log is also written to sdmc:/switch/opence/host.txt.

The process ends with the game (svcExitProcess): the guest's threads may
still run, and its mappings use the homebrew loader's heap, so returning to
the loader is not safe.
*/

#include "host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ENVIRONMENT_MAXIMUM 32
#define BOOT_SIZE 0x10000

#define HEARTBEAT_SECONDS 5

struct host_activity host_activity;

static FILE *log_file;
static Mutex log_lock;
/* the shutdown has paused threads, which may hold log_lock: the log only
tries it from then on */
static volatile int shutting_down;
static volatile int heartbeat_stop;
/* the text console, on the screen until the guest's OpenGL takes it */
static int console_open;
static PadState pad;

/* ---------- the log */

static void log_line(int level, const char *text)
{
	static const char *const levels[] = {"", "warning: ", "error: "};

	if (shutting_down)
	{
		if (!mutexTryLock(&log_lock))
			return;
	}
	else
	{
		mutexLock(&log_lock);
	}
	if (console_open)
	{
		printf("%s%s\n", levels[level], text);
		consoleUpdate(NULL);
	}
	if (log_file)
	{
		fprintf(log_file, "%s%s\n", levels[level], text);
		fflush(log_file);
	}
	mutexUnlock(&log_lock);
}

void host_logf(int level, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	log_line(level, text);
}

/* host_log: the guest's log lines (platform_log), with Android's
priorities (4 information, 5 warning, 6 error) */
void host_log(int priority, const char *text)
{
	log_line(priority >= 6 ? HOST_LOG_ERROR : priority == 5 ? HOST_LOG_WARN : HOST_LOG_INFO, text);
}

/* the guest's OpenGL takes the window (host_gl.c): the log goes on in
host.txt only */
void host_console_close(void)
{
	mutexLock(&log_lock);
	if (console_open)
	{
		consoleExit(NULL);
		console_open = 0;
	}
	mutexUnlock(&log_lock);
}

/* ends the process: once + is pressed while the log is on the screen, at
once after the game has taken the screen (the log is in host.txt) */
static void __attribute__((noreturn)) end_process(void)
{
	if (!console_open)
	{
		mutexLock(&log_lock);
		if (log_file)
			fclose(log_file);
		log_file = NULL;
		svcExitProcess();
	}
	log_line(HOST_LOG_INFO, "Press + to exit.");
	while (appletMainLoop())
	{
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		svcSleepThread(16000000LL);
	}
	mutexLock(&log_lock);
	if (log_file)
		fclose(log_file);
	log_file = NULL;
	svcExitProcess();
}

void host_fatal(const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	log_line(HOST_LOG_ERROR, text);
	end_process();
}

void host_abort(const char *reason)
{
	host_fatal("the game stopped: %s", reason ? reason : "(no reason given)");
}

/* the game exits: on its main thread, the host shuts down and returns to
the homebrew loader (main); from another thread, the process ends */
void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited with code %d", code);
	host_guest_exited(code);
	end_process();
}

/* ---------- the heartbeat */

/* Logs what the guest did in the last few seconds, so a log that stops
shows whether the game is idle (its counts still rise) or stuck (they do
not; "waiting" then counts the threads blocked in a futex). */
static void heartbeat(void *unused)
{
	struct host_activity last;

	memset(&last, 0, sizeof(last));

	(void)unused;
	while (!heartbeat_stop)
	{
		int tick;

		for (tick = 0; tick < HEARTBEAT_SECONDS * 10 && !heartbeat_stop; tick++)
			svcSleepThread(100000000LL);
		if (heartbeat_stop)
			break;
		struct host_activity now = host_activity;

		host_logf(HOST_LOG_INFO, "heartbeat: %llu system calls, %llu input polls, %llu frames; "
			"%d threads waiting (last on %08x)",
			(unsigned long long)(now.system_calls - last.system_calls),
			(unsigned long long)(now.input_polls - last.input_polls),
			(unsigned long long)(now.frames - last.frames),
			(int)now.waiting, (unsigned)now.last_wait_address);
		host_logf(HOST_LOG_INFO, "  time: reading %llu ms (%llu KB), sleeping %llu ms, waiting %llu ms, "
			"mapping %llu ms",
			(unsigned long long)((now.read_time - last.read_time) / 1000000),
			(unsigned long long)((now.read_bytes - last.read_bytes) >> 10),
			(unsigned long long)((now.sleep_time - last.sleep_time) / 1000000),
			(unsigned long long)((now.wait_time - last.wait_time) / 1000000),
			(unsigned long long)((now.mapping_time - last.mapping_time) / 1000000));
		if (now.frames != last.frames)
			host_logf(HOST_LOG_INFO, "  renderer: hashing %llu ms, fences %llu ms, readbacks %llu ms, swaps %llu ms",
				(unsigned long long)((now.hash_time - last.hash_time) / 1000000),
				(unsigned long long)((now.fence_time - last.fence_time) / 1000000),
				(unsigned long long)((now.readback_time - last.readback_time) / 1000000),
				(unsigned long long)((now.swap_time - last.swap_time) / 1000000));
		host_profile_report();
		last = now;
	}
}

/* (at the usual priority, above the guest's preemptive one, so that it runs
while the guest's threads are busy; on core 2, away from the game's main
thread on core 0, which its writes to the SD card would otherwise stall) */
static Thread heartbeat_thread;

static void start_heartbeat(void)
{
	Thread *thread = &heartbeat_thread;

	Result rc = threadCreate(thread, heartbeat, NULL, NULL, 0x4000, 0x2c, 2);

	if (R_SUCCEEDED(rc))
		rc = threadStart(thread);
	if (R_FAILED(rc))
		host_logf(HOST_LOG_WARN, "cannot start the heartbeat: 0x%x", rc);
}

/* ---------- the guest's folders and environment */

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? HOST_SAVE_ROOT : HOST_DATA_ROOT);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const char *const *environment, int count)
{
	char *memory = host_low_map(BOOT_SIZE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < count && index < ENVIRONMENT_MAXIMUM; index++)
	{
		size_t length = strlen(environment[index]) + 1;

		if (strings + length > memory + BOOT_SIZE)
			break;
		memcpy(strings, environment[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = (uint32_t)HOST_PAGE;
	return (uint32_t)(uintptr_t)boot;
}

/* the whole of a file, or NULL */
static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

/* ---------- main */

/* libnx's own exit (its destructors and services, then the exit function) */
void NX_NORETURN __libnx_exit(int code);

static void NX_NORETURN exit_process(int code)
{
	(void)code;
	svcExitProcess();
}

int main(int argc, char **argv)
{
	static const char *const environment[] =
	{
		"HOME=" HOST_SAVE_ROOT,
		"HALO_DATA_ROOT=" HOST_DATA_ROOT,
		"HALO_SAVE_ROOT=" HOST_SAVE_ROOT,
		"TZ=UTC0",
		/* 480 lines at the screen's 16:9 (d3d8_gl.c), as the Android host
		gives the display's shape */
		"HALO_DISPLAY_WIDTH=852",
	};
	AppletType type;
	size_t image_size = 0;
	void *image;
	Result rc;
	int code;

	(void)argc;
	(void)argv;
	mutexInit(&log_lock);
	consoleInit(NULL);
	console_open = 1;
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	mkdir("sdmc:" HOST_DATA_ROOT, 0777);
	mkdir("sdmc:" HOST_SAVE_ROOT, 0777);
	log_file = fopen(HOST_LOG_PATH, "w");
	/* what libraries report there (Mesa's warnings, its shader cache's) */
	freopen(HOST_STDERR_PATH, "w", stderr);
	host_logf(HOST_LOG_INFO, "OpenCE for the Switch (bring-up host)");

	type = appletGetAppletType();
	if (type != AppletType_Application && type != AppletType_SystemApplication)
		host_fatal("Start OpenCE by holding R while starting a game (title takeover). "
			"From the Album (applet mode) there is too little memory.");

	rc = romfsInit();
	if (R_FAILED(rc))
		host_fatal("cannot open the NRO's RomFS: 0x%x", rc);
	image = read_file("romfs:/halo_guest.elf", &image_size);
	if (!image)
		host_fatal("cannot read the game image (romfs:/halo_guest.elf)");
	if (host_memory_initialize() != 0)
		host_fatal("cannot map the game's memory below 4 GB");
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image");
	free(image);
	romfsExit();

	host_logf(HOST_LOG_INFO, "data %s, saves %s", HOST_DATA_ROOT, HOST_SAVE_ROOT);
	start_heartbeat();
	code = host_run_guest_main(make_boot(environment, (int)(sizeof(environment) / sizeof(environment[0]))));

	/* the game has exited: everything it used is let go, in an order that
	frees nothing still in use, and the homebrew loader takes over */
	if (heartbeat_thread.handle)
	{
		heartbeat_stop = 1;
		threadWaitForExit(&heartbeat_thread);
		threadClose(&heartbeat_thread);
	}
	shutting_down = 1;
	/* (each step logged: a shutdown that stops shows where) */
	host_logf(HOST_LOG_INFO, "shutdown: the profiler and the game's threads");
	host_profile_stop();
	host_threads_stop();
	host_logf(HOST_LOG_INFO, "shutdown: the sound");
	host_audio_stop();
	host_logf(HOST_LOG_INFO, "shutdown: the game's files");
	host_files_close();
	/* (OpenGL ES and the game's memory are left to the process's end: with
	Mesa's GL thread, EGL's own shutdown waited for that thread, which was
	stuck, and the game froze as it quit) */
	host_logf(HOST_LOG_INFO, "closing");
	mutexLock(&log_lock);
	if (log_file)
		fclose(log_file);
	log_file = NULL;
	mutexUnlock(&log_lock);
	/* libnx's exit closes the applet and the other services, then calls
	exit_process instead of returning to the homebrew loader: the Homebrew
	Menu it would start could not set up the GPU after Mesa, and ending the
	process without libnx's exit shows a software error. It is called
	directly, not by returning from main, which would first run atexit's
	functions: Mesa's waits for its threads, the GL thread among them. */
	envSetExitFuncPtr(exit_process);
	__libnx_exit(code);
}
