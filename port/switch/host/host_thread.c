/*
HOST_THREAD.C

Threads that run guest code.

Guest (ILP32) code keeps stack addresses in 32-bit registers, so it must run
on a stack below 4 GB. libnx gives each thread a small stack of its own in
the stack region, above 4 GB; the thread moves at once to a stack in guest
memory (host_call_on_stack, host_stack.s) and runs the guest there, host
calls included. The guest's thread pointer (its musl struct pthread) is
kept per thread in host TLS.

Guest threads end by returning from __guest_thread_start. A finished
thread's libnx thread and stack are released when the next one is made.
*/

#include "host.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_STACK_SIZE (16 * 1024 * 1024)
/* the libnx stack each thread starts on, before it moves to its guest stack */
#define NATIVE_STACK_SIZE 0x10000
#define GUARD_SIZE 0x4000
/* Horizon does not share a core between threads of one priority: each
runs until it blocks. On cores 0 to 2 this priority alone is preemptive
(libnx's threadCreate), so the guest's threads take turns as on a desktop,
where the game's loops that poll another thread expect it. */
#define THREAD_PRIORITY 0x3b
/* the cores an application may use */
#define CORE_COUNT 3

struct guest_thread_start
{
	struct guest_thread_start *next;
	Thread thread;
	/* the guest's thread (pthread_create), or a host function that runs on
	the guest stack (host_native_thread_create) */
	uint32_t guest_thread;
	void (*host_function)(void *);
	void *host_argument;
	void *stack;
	uint64_t stack_size;
	volatile int finished;
};

static __thread uint32_t guest_tp;
static __thread uint64_t thread_id;
static uint64_t next_thread_id = 1;
static Mutex threads_lock;
/* the guest's exit on its main thread returns here, to the host's stack
(host_guest_exited) */
static jmp_buf guest_exit;
static Handle main_thread = INVALID_HANDLE;
static int guest_exit_code;
static struct guest_thread_start *threads;
static int next_core;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

/* the guest's thread ids (gettid), small positive numbers */
uint64_t host_thread_id(void)
{
	if (!thread_id)
		thread_id = __atomic_add_fetch(&next_thread_id, 1, __ATOMIC_RELAXED);
	return thread_id;
}

/* ---------- the main thread */

static void guest_main(uint64_t boot)
{
	((void (*)(uint32_t))(uintptr_t)host_image.header->start)((uint32_t)boot);
	host_fatal("the guest returned from __guest_start");
}

/* runs the game's main until it exits (on this thread); returns its code */
int host_run_guest_main(uint32_t boot)
{
	char *stack = host_low_map(MAIN_STACK_SIZE);

	if (!stack)
		host_fatal("cannot allocate the guest's main stack");
	main_thread = threadGetCurHandle();
	svcSetThreadPriority(CUR_THREAD_HANDLE, THREAD_PRIORITY);
	host_profile_start();
	host_logf(HOST_LOG_INFO, "starting the guest (stack %08llx)", (unsigned long long)(uintptr_t)stack);
	if (setjmp(guest_exit) == 0)
	{
		host_call_on_stack(boot, guest_main, stack + MAIN_STACK_SIZE);
		host_fatal("the guest returned from __guest_start");
	}
	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2c);
	return guest_exit_code;
}

/* the game exits: on its main thread, back to host_run_guest_main (the
guest's stack is left behind); 0 on any other thread, which cannot */
int host_guest_exited(int code)
{
	if (main_thread == INVALID_HANDLE || threadGetCurHandle() != main_thread)
		return 0;
	guest_exit_code = code;
	longjmp(guest_exit, 1);
}

/* The guest's other threads, once the game has exited: Horizon cannot end
another thread, so each is paused for good, and its handle and libnx stack
let go, before the memory they ran in is unmapped. */
void host_threads_stop(void)
{
	struct guest_thread_start *start, *next;

	mutexLock(&threads_lock);
	for (start = threads; start; start = next)
	{
		next = start->next;
		if (!__atomic_load_n(&start->finished, __ATOMIC_ACQUIRE))
			svcSetThreadActivity(start->thread.handle, ThreadActivity_Paused);
		else
			threadWaitForExit(&start->thread);
		threadClose(&start->thread);
		free(start);
	}
	threads = NULL;
	mutexUnlock(&threads_lock);
}

/* ---------- guest threads */

static void guest_thread_body(uint64_t context)
{
	struct guest_thread_start *start = (struct guest_thread_start *)(uintptr_t)context;

	if (start->host_function)
		start->host_function(start->host_argument);
	else
		((void (*)(uint32_t))(uintptr_t)host_image.header->thread_start)(start->guest_thread);
	/* (a host thread that ran guest code keeps a struct pthread, which the
	guest lets go here) */
	if (guest_tp)
		((void (*)(void))(uintptr_t)host_image.header->thread_detach)();
}

static void native_thread_main(void *context)
{
	struct guest_thread_start *start = context;

	host_call_on_stack((uint64_t)(uintptr_t)start, guest_thread_body, (char *)start->stack + start->stack_size);
	guest_tp = 0;
	__atomic_store_n(&start->finished, 1, __ATOMIC_RELEASE);
}

/* releases the threads that have finished; threads_lock is held */
static void reap_threads(void)
{
	struct guest_thread_start **link = &threads;

	while (*link)
	{
		struct guest_thread_start *start = *link;

		if (__atomic_load_n(&start->finished, __ATOMIC_ACQUIRE))
		{
			*link = start->next;
			threadWaitForExit(&start->thread);
			threadClose(&start->thread);
			host_low_unmap((char *)start->stack - GUARD_SIZE, start->stack_size + GUARD_SIZE);
			free(start);
		}
		else
		{
			link = &start->next;
		}
	}
}

/* a thread on a guest stack, running the guest's thread or host_function;
0 or an errno */
static int thread_create(uint32_t guest_thread, void (*host_function)(void *), void *host_argument,
	uint32_t stack_size, int priority)
{
	struct guest_thread_start *start;
	uint64_t size = ((uint64_t)stack_size + 0xffff) & ~0xffffULL;
	char *memory;
	Result rc;
	int core;

	if (!size)
		size = 0x10000;
	start = calloc(1, sizeof(*start));
	if (!start)
		return 12; /* ENOMEM */
	/* (the guard is ordinary memory: Horizon cannot make a page of these
	mappings inaccessible after it was read/write, so it only keeps the
	stack apart from the memory below it) */
	memory = host_low_map(size + GUARD_SIZE);
	if (!memory)
	{
		free(start);
		return 11; /* EAGAIN */
	}
	start->guest_thread = guest_thread;
	start->host_function = host_function;
	start->host_argument = host_argument;
	start->stack = memory + GUARD_SIZE;
	start->stack_size = size;

	mutexLock(&threads_lock);
	reap_threads();
	core = next_core;
	next_core = (next_core + 1) % CORE_COUNT;
	rc = threadCreate(&start->thread, native_thread_main, start, NULL, NATIVE_STACK_SIZE, priority, core);
	if (R_SUCCEEDED(rc))
	{
		rc = threadStart(&start->thread);
		if (R_FAILED(rc))
			threadClose(&start->thread);
	}
	if (R_FAILED(rc))
	{
		mutexUnlock(&threads_lock);
		host_logf(HOST_LOG_ERROR, "cannot start a guest thread: 0x%x", rc);
		host_low_unmap(memory, size + GUARD_SIZE);
		free(start);
		return 11; /* EAGAIN */
	}
	start->next = threads;
	threads = start;
	mutexUnlock(&threads_lock);
	return 0;
}

/* host_thread_create: a thread for the guest's pthread_create; 0 or an errno */
int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return thread_create(guest_thread, NULL, NULL, stack_size, THREAD_PRIORITY);
}

/* a host thread that can call the guest (host_call_guest): it runs function
on a guest stack, at priority (above the guest's for the audio thread, so
that it is never starved) */
int host_native_thread_create(void (*function)(void *), void *argument, uint32_t stack_size, int priority)
{
	return thread_create(0, function, argument, stack_size, priority);
}

/* calls a guest function from a thread on a guest stack, giving the thread
a struct pthread first if it has none */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);

	if (sp >= HOST_LOW_LIMIT)
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		((uint32_t (*)(void))(uintptr_t)host_image.header->thread_attach)();
	return ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)function)(a, b, c, d);
}
