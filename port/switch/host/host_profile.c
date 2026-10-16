/*
HOST_PROFILE.C

A sampling profiler for the guest's main thread, for the heartbeat
(host_main.c): about a thousand times a second it pauses the thread, notes
its program counter and link register, and lets it go. Each heartbeat logs
the addresses met most often. Guest addresses (0x88000000 up) name the
game's code: llvm-symbolizer --obj=build/android/halo_guest.elf. Host
addresses are given as offsets in the NRO (the host, Mesa, libnx):
aarch64-none-elf-addr2line -f -e build/switch/opence.elf.

Also counts the guest's system calls by number (host_syscall.c), and keeps
the samples of each frame that took HITCH_NANOSECONDS or more apart, in
HITCHES_PATH (host_profile_frame), in profile.txt's format: what the game
did in the frames that stuttered.
*/

#include "host.h"

#include <stdio.h>
#include <string.h>

#define SAMPLE_INTERVAL 1000000LL
#define TABLE_SIZE 4096
#define REPORTED 10
#define SYSCALL_NUMBERS 512

struct sample_table
{
	uint64_t address[TABLE_SIZE];
	uint32_t count[TABLE_SIZE];
	uint32_t total;
};

static struct sample_table program_counters;
static struct sample_table link_registers;

/* every sample since the start, written whole to PROFILE_PATH every
twelfth heartbeat (a minute), for a profile by function (port/switch/README.md) */
#define PROFILE_PATH "sdmc:/switch/opence/profile.txt"
#define ALL_SIZE 131072
/* a table nearly full is not searched further: the sample is dropped */
#define ALL_PROBES 64

/* where the thread was, then its callers: the link register, and the
return addresses of CHAIN_DEPTH - 2 more frame records (the paused
thread's stack is read: one query of its mapping, then reads within it) */
#define CHAIN_DEPTH 12

struct chain_table
{
	uint64_t (*chains)[CHAIN_DEPTH];
	uint32_t *counts;
	uint32_t size;
	uint32_t total;
};

static uint64_t all_chains[ALL_SIZE][CHAIN_DEPTH];
static uint32_t all_counts[ALL_SIZE];
static struct chain_table all = { all_chains, all_counts, ALL_SIZE, 0 };

/* the samples of the last RECENT_SIZE (about four seconds), with when each
was taken, and those of the frames that took HITCH_NANOSECONDS or more */
#define HITCHES_PATH "sdmc:/switch/opence/hitches.txt"
#define HITCH_NANOSECONDS 100000000ULL
#define RECENT_SIZE 4096
#define HITCHES_SIZE 16384
/* the files' buffer */
#define WRITE_BUFFER_SIZE (256 * 1024)

static uint64_t recent_chains[RECENT_SIZE][CHAIN_DEPTH];
static uint64_t recent_times[RECENT_SIZE];
static uint32_t recent_next;
static uint64_t hitch_chains[HITCHES_SIZE][CHAIN_DEPTH];
static uint32_t hitch_counts[HITCHES_SIZE];
static struct chain_table hitches = { hitch_chains, hitch_counts, HITCHES_SIZE, 0 };
static uint32_t hitch_frames;
static uint64_t hitch_time;

static Mutex profile_lock;
static Handle profiled = INVALID_HANDLE;
static Thread sampler_thread;
static volatile uint32_t syscall_counts[SYSCALL_NUMBERS];

void host_profile_syscall(long long number)
{
	if (number >= 0 && number < SYSCALL_NUMBERS)
		__atomic_add_fetch(&syscall_counts[number], 1, __ATOMIC_RELAXED);
}

static void table_add(struct sample_table *table, uint64_t address)
{
	uint32_t slot = (uint32_t)((address >> 2) * 2654435761u) % TABLE_SIZE;
	uint32_t probe;

	for (probe = 0; probe < TABLE_SIZE; probe++)
	{
		uint32_t index = (slot + probe) % TABLE_SIZE;

		if (table->address[index] == address || !table->count[index])
		{
			table->address[index] = address;
			table->count[index]++;
			table->total++;
			return;
		}
	}
}

static void describe(char *text, size_t size, uint64_t address);

/* the thread's chain: pc, lr, then the return addresses of the frame
records from fp (each record: the caller's fp, then the return address),
all within the mapping that holds the stack pointer */
static void chain_of(const ThreadContext *context, uint64_t *chain)
{
	uint64_t fp = context->fp, low = context->sp, high = 0;
	MemoryInfo info;
	u32 page_info;
	int depth;

	memset(chain, 0, CHAIN_DEPTH * sizeof(*chain));
	chain[0] = context->pc.x;
	chain[1] = context->lr;
	if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, context->sp)) && (info.perm & Perm_R))
		high = info.addr + info.size;
	for (depth = 2; depth < CHAIN_DEPTH; depth++)
	{
		uint64_t next;

		if (fp < low || fp + 16 > high || (fp & 7))
			break;
		next = ((const uint64_t *)(uintptr_t)fp)[0];
		chain[depth] = ((const uint64_t *)(uintptr_t)fp)[1];
		/* (the first record may be lr's own frame's: skip the repeat) */
		if (depth == 2 && chain[depth] == chain[1])
			depth--;
		if (next <= fp)
			break;
		fp = next;
	}
}

static void chains_add(struct chain_table *table, const uint64_t *chain)
{
	uint32_t hash = 0, probe;
	int depth;

	for (depth = 0; depth < CHAIN_DEPTH; depth++)
		hash = (hash ^ (uint32_t)(chain[depth] >> 2)) * 2654435761u;
	for (probe = 0; probe < ALL_PROBES; probe++)
	{
		uint32_t index = (hash + probe) % table->size;

		if (!table->counts[index] || !memcmp(table->chains[index], chain, sizeof(table->chains[index])))
		{
			memcpy(table->chains[index], chain, sizeof(table->chains[index]));
			table->counts[index]++;
			table->total++;
			return;
		}
	}
}

/* (written whole to a new file, which then replaces the last: a file cut
off by the game's exit lost most of its samples) */
static void chains_write(const struct chain_table *table, const char *path, const char *heading)
{
	char new_path[64];
	FILE *file;
	char text[48];
	uint32_t index;
	int depth;

	snprintf(new_path, sizeof(new_path), "%s.new", path);
	file = fopen(new_path, "w");
	if (!file)
		return;
	/* (in large pieces: with newlib's own small buffer, a file of 10 MB was
	thousands of writes to the SD card, and took up to 40 s) */
	setvbuf(file, NULL, _IOFBF, WRITE_BUFFER_SIZE);
	fprintf(file, "# %s: count, where, then its callers\n# %u samples\n", heading, table->total);
	for (index = 0; index < table->size; index++)
	{
		if (!table->counts[index])
			continue;
		fprintf(file, "%u", table->counts[index]);
		for (depth = 0; depth < CHAIN_DEPTH; depth++)
		{
			describe(text, sizeof(text), table->chains[index][depth]);
			fprintf(file, "\t%s", text);
		}
		fprintf(file, "\n");
	}
	if (fclose(file) == 0)
	{
		remove(path);
		rename(new_path, path);
	}
}

/* a frame from start to end (host_now), which the game has presented: its
samples, if it took HITCH_NANOSECONDS or more, into the hitches' table. On
the game's main thread, which never waits for the lock: while the files are
written (host_profile_report) the frame is not kept, as the game stopped
until they were. */
void host_profile_frame(uint64_t start, uint64_t end)
{
	uint32_t index;

	if (end - start < HITCH_NANOSECONDS || sampler_thread.handle == INVALID_HANDLE || !sampler_thread.handle ||
		!mutexTryLock(&profile_lock))
	{
		return;
	}
	for (index = 0; index < RECENT_SIZE; index++)
	{
		if (recent_times[index] >= start && recent_times[index] <= end)
			chains_add(&hitches, recent_chains[index]);
	}
	hitch_frames++;
	hitch_time += end - start;
	mutexUnlock(&profile_lock);
}

/* pauses the sampler for good (the shutdown) */
void host_profile_stop(void)
{
	if (sampler_thread.handle == INVALID_HANDLE || !sampler_thread.handle)
		return;
	svcSetThreadActivity(sampler_thread.handle, ThreadActivity_Paused);
	threadClose(&sampler_thread);
}

/* The thread is paused only to read its registers and walk its stack;
recording waits for the lock (the heartbeat's report and file) with it
running again. */
static void sampler(void *unused)
{
	(void)unused;
	for (;;)
	{
		ThreadContext context;
		uint64_t chain[CHAIN_DEPTH];
		int sampled;

		svcSleepThread(SAMPLE_INTERVAL);
		if (R_FAILED(svcSetThreadActivity(profiled, ThreadActivity_Paused)))
			continue;
		sampled = R_SUCCEEDED(svcGetThreadContext3(&context, profiled));
		if (sampled)
			chain_of(&context, chain);
		svcSetThreadActivity(profiled, ThreadActivity_Runnable);
		if (!sampled)
			continue;
		mutexLock(&profile_lock);
		table_add(&program_counters, chain[0]);
		table_add(&link_registers, chain[1]);
		chains_add(&all, chain);
		memcpy(recent_chains[recent_next], chain, sizeof(recent_chains[recent_next]));
		recent_times[recent_next] = host_now();
		recent_next = (recent_next + 1) % RECENT_SIZE;
		mutexUnlock(&profile_lock);
	}
}

/* samples the calling thread from now on */
void host_profile_start(void)
{
	Result rc;

	if (!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33))
	{
		host_logf(HOST_LOG_WARN, "profile: this process may not pause its threads");
		return;
	}
	profiled = threadGetCurHandle();
	/* (above the guest's priority, and the heartbeat's, on a core of its own) */
	rc = threadCreate(&sampler_thread, sampler, NULL, NULL, 0x4000, 0x2b, 2);
	if (R_SUCCEEDED(rc))
		rc = threadStart(&sampler_thread);
	if (R_FAILED(rc))
		host_logf(HOST_LOG_WARN, "profile: cannot start the sampler: 0x%x", rc);
}

/* the start of the NRO's code: the mapping that holds this function */
static uint64_t host_base(void)
{
	static uint64_t base;
	MemoryInfo info;
	u32 page_info;

	if (!base && R_SUCCEEDED(svcQueryMemory(&info, &page_info, (uint64_t)(uintptr_t)host_base)))
		base = info.addr;
	return base;
}

static void describe(char *text, size_t size, uint64_t address)
{
	uint64_t start = host_base();

	if (address >= HALO_GUEST_IMAGE_BASE && address < (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE)
		snprintf(text, size, "guest %08llx", (unsigned long long)address);
	else if (address >= start && address < start + 0x10000000ULL)
		snprintf(text, size, "host +%llx", (unsigned long long)(address - start));
	else
		snprintf(text, size, "other %llx", (unsigned long long)address);
}

static void table_report(struct sample_table *table, const char *what)
{
	uint32_t reported = 0;

	if (!table->total)
		return;
	host_logf(HOST_LOG_INFO, "  profile, %s (%u samples):", what, table->total);
	while (reported < REPORTED)
	{
		uint32_t best = TABLE_SIZE, index;
		char text[48];

		for (index = 0; index < TABLE_SIZE; index++)
		{
			if (table->count[index] && (best == TABLE_SIZE || table->count[index] > table->count[best]))
				best = index;
		}
		if (best == TABLE_SIZE)
			break;
		describe(text, sizeof(text), table->address[best]);
		host_logf(HOST_LOG_INFO, "    %5.1f%% %s", 100.0 * table->count[best] / table->total, text);
		table->count[best] = 0;
		reported++;
	}
	memset(table, 0, sizeof(*table));
}

/* the heartbeat's report: the top addresses and system calls since the last */
void host_profile_report(void)
{
	static uint32_t reports;
	uint32_t counts[SYSCALL_NUMBERS];
	int reported, number;

	mutexLock(&profile_lock);
	table_report(&program_counters, "where the main thread was");
	table_report(&link_registers, "what it was called from");
	/* (every twelfth, a minute: the file is large, and its writes share the
	SD card with the game's reads) */
	if (++reports % 12 == 0)
	{
		char heading[96];

		chains_write(&all, PROFILE_PATH, "samples of the main thread since the start");
		snprintf(heading, sizeof(heading), "samples of the main thread in its %u frames of 100 ms or more (%llu ms)",
			hitch_frames, (unsigned long long)(hitch_time / 1000000));
		if (hitch_frames)
			chains_write(&hitches, HITCHES_PATH, heading);
	}
	mutexUnlock(&profile_lock);

	for (number = 0; number < SYSCALL_NUMBERS; number++)
		counts[number] = __atomic_exchange_n(&syscall_counts[number], 0, __ATOMIC_RELAXED);
	for (reported = 0; reported < 5; reported++)
	{
		int best = -1;

		for (number = 0; number < SYSCALL_NUMBERS; number++)
		{
			if (counts[number] && (best < 0 || counts[number] > counts[best]))
				best = number;
		}
		if (best < 0)
			break;
		host_logf(HOST_LOG_INFO, "  system call %d: %u", best, counts[best]);
		counts[best] = 0;
	}
}
