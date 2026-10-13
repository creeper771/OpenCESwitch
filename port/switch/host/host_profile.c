/*
HOST_PROFILE.C

A sampling profiler for the guest's main thread, for the heartbeat
(host_main.c): about a thousand times a second it pauses the thread, notes
its program counter and link register, and lets it go. Each heartbeat logs
the addresses met most often. Guest addresses (0x88000000 up) name the
game's code: llvm-symbolizer --obj=build/android/halo_guest.elf. Host
addresses are given as offsets in the NRO (the host, Mesa, libnx):
aarch64-none-elf-addr2line -f -e build/switch/opence.elf.

Also counts the guest's system calls by number (host_syscall.c).
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
sixth heartbeat (30 seconds), for a profile by function (port/switch/README.md) */
#define PROFILE_PATH "sdmc:/switch/opence/profile.txt"
#define ALL_SIZE 65536

static uint64_t all_addresses[ALL_SIZE];
static uint64_t all_callers[ALL_SIZE];
static uint32_t all_counts[ALL_SIZE];
static uint32_t all_total;
static Mutex profile_lock;
static Handle profiled = INVALID_HANDLE;
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

/* counted by where the thread was and what called it */
static void all_add(uint64_t address, uint64_t caller)
{
	uint32_t slot = (uint32_t)(((address >> 2) ^ (caller >> 2) * 31) * 2654435761u) % ALL_SIZE;
	uint32_t probe;

	for (probe = 0; probe < ALL_SIZE; probe++)
	{
		uint32_t index = (slot + probe) % ALL_SIZE;

		if ((all_addresses[index] == address && all_callers[index] == caller) || !all_counts[index])
		{
			all_addresses[index] = address;
			all_callers[index] = caller;
			all_counts[index]++;
			all_total++;
			return;
		}
	}
}

static void all_write(void)
{
	FILE *file = fopen(PROFILE_PATH, "w");
	char where[48], caller[48];
	uint32_t index;

	if (!file)
		return;
	fprintf(file, "# samples of the main thread since the start: count, where, called from\n# %u samples\n",
		all_total);
	for (index = 0; index < ALL_SIZE; index++)
	{
		if (!all_counts[index])
			continue;
		describe(where, sizeof(where), all_addresses[index]);
		describe(caller, sizeof(caller), all_callers[index]);
		fprintf(file, "%u\t%s\t%s\n", all_counts[index], where, caller);
	}
	fclose(file);
}

static void sampler(void *unused)
{
	(void)unused;
	for (;;)
	{
		ThreadContext context;

		svcSleepThread(SAMPLE_INTERVAL);
		if (R_FAILED(svcSetThreadActivity(profiled, ThreadActivity_Paused)))
			continue;
		if (R_SUCCEEDED(svcGetThreadContext3(&context, profiled)))
		{
			mutexLock(&profile_lock);
			table_add(&program_counters, context.pc.x);
			table_add(&link_registers, context.lr);
			all_add(context.pc.x, context.lr);
			mutexUnlock(&profile_lock);
		}
		svcSetThreadActivity(profiled, ThreadActivity_Runnable);
	}
}

/* samples the calling thread from now on */
void host_profile_start(void)
{
	static Thread thread;
	Result rc;

	if (!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33))
	{
		host_logf(HOST_LOG_WARN, "profile: this process may not pause its threads");
		return;
	}
	profiled = threadGetCurHandle();
	/* (above the guest's priority, and the heartbeat's, on a core of its own) */
	rc = threadCreate(&thread, sampler, NULL, NULL, 0x4000, 0x2b, 2);
	if (R_SUCCEEDED(rc))
		rc = threadStart(&thread);
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
	/* (every sixth: the file is large) */
	if (++reports % 6 == 0)
		all_write();
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
