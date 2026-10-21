/*
HOST_CLOCK.C

The CPU's and the GPU's clocks, which the game may raise, docked and in the
hand (switch.cpu_clock_docked, switch.cpu_clock_handheld,
switch.gpu_clock_docked and switch.gpu_clock_handheld, through the
host_cpu_clock and host_gpu_clock imports): the game's main thread, on one
core, is what slows its heaviest scenes, and the GPU what slows its higher
resolutions docked. The system runs games' CPU at 1020 MHz, and their GPU at
768 MHz docked and 307 to 460 in the hand. Set through the clock service
(clkrst), which a homebrew program may use as sys-clk does, to the clock of
the mode the console is in, the nearest the module's own table has at or
below it.

The system sets its own clocks again when the console is docked, undocked
or wakes, so they are set again at once when the display changes and for a
few seconds after (host_gl.c), and by each heartbeat (host_clock_keep). Each
mode's own clock, as the system had it before a clock was set there, is
remembered and put back for Stock and by the shutdown (host_clock_restore).
*/

#include "host.h"

#define MAXIMUM_RATES 32

struct clock
{
	PcvModuleId module;
	const char *name;
	ClkrstSession session;
	int opened;
	/* the module's own clocks (Hz), from the system */
	uint32_t rates[MAXIMUM_RATES];
	int rate_count;
	/* those asked for docked and in the hand (0: the system's), and the
	system's own in each, as found before one was set there (0: not yet) */
	uint32_t docked_hz, handheld_hz;
	uint32_t docked_stock, handheld_stock;
};

static struct clock cpu = { .module = PcvModuleId_CpuBus, .name = "CPU" };
static struct clock gpu = { .module = PcvModuleId_GPU, .name = "GPU" };

static Mutex clock_lock;
static int service_opened;
static int apm_opened;

static int clock_open(struct clock *clock)
{
	PcvClockRatesListType type;
	s32 count = 0;
	Result rc;

	if (clock->opened)
		return clock->opened > 0;
	clock->opened = -1;
	if (!service_opened)
	{
		rc = clkrstInitialize();
		if (R_FAILED(rc))
		{
			host_logf(HOST_LOG_WARN, "clock: the clock service cannot be used (0x%x)", rc);
			return 0;
		}
		service_opened = 1;
		/* (the performance service says whether the console is docked:
		libnx's own operation mode is only kept by applet messages, which
		the host reads only for these: host_main.c) */
		apm_opened = R_SUCCEEDED(apmInitialize());
	}
	rc = clkrstOpenSession(&clock->session, clock->module, 3);
	if (R_FAILED(rc))
	{
		host_logf(HOST_LOG_WARN, "clock: the %s's clock cannot be set (0x%x)", clock->name, rc);
		return 0;
	}
	if (R_SUCCEEDED(clkrstGetPossibleClockRates(&clock->session, clock->rates, MAXIMUM_RATES, &type, &count)) &&
		type == PcvClockRatesListType_Discrete && count > 0)
	{
		clock->rate_count = count;
	}
	clock->opened = 1;
	return 1;
}

/* whether the console is docked (its performance mode is the boost one) */
static int clock_docked(void)
{
	ApmPerformanceMode mode = ApmPerformanceMode_Normal;

	if (apm_opened && R_SUCCEEDED(apmGetPerformanceMode(&mode)))
		return mode == ApmPerformanceMode_Boost;
	return appletGetOperationMode() == AppletOperationMode_Console;
}

/* the module's clock nearest to megahertz at or below it (Hz), or 0: within
the megahertz it names (the GPU's are 844.8, 921.6) */
static uint32_t clock_rate(const struct clock *clock, unsigned int megahertz)
{
	uint32_t wanted = megahertz * 1000000u, best = 0;
	int index;

	if (!megahertz)
		return 0;
	/* (a module whose table is not known: as asked) */
	if (!clock->rate_count)
		return wanted;
	for (index = 0; index < clock->rate_count; index++)
	{
		if (clock->rates[index] < wanted + 1000000u && clock->rates[index] > best)
			best = clock->rates[index];
	}
	return best;
}

/* sets the clock wanted in the mode the console is in, or the system's own
there; the clock lock held */
static void clock_set(struct clock *clock, int restoring)
{
	int docked = clock_docked();
	uint32_t wanted = restoring ? 0 : docked ? clock->docked_hz : clock->handheld_hz;
	uint32_t other = docked ? clock->handheld_hz : clock->docked_hz;
	uint32_t *stock = docked ? &clock->docked_stock : &clock->handheld_stock;
	uint32_t now = 0, hz;
	Result rc;

	if (clock->opened <= 0 || R_FAILED(clkrstGetClockRate(&clock->session, &now)))
		return;
	/* (the system's own here, before one is set: not one the host set, in
	this mode or the other, which the system may not have put back yet) */
	if (wanted && !*stock && now != wanted && now != other)
		*stock = now;
	hz = wanted ? wanted : *stock;
	if (!hz || now == hz)
		return;
	rc = clkrstSetClockRate(&clock->session, hz);
	if (R_FAILED(rc))
		host_logf(HOST_LOG_WARN, "clock: cannot set the %s to %u MHz (0x%x)", clock->name, hz / 1000000u, rc);
	else
		host_logf(HOST_LOG_INFO, "clock: %s at %u MHz %s (it was %u)", clock->name, hz / 1000000u,
			docked ? "docked" : "in the hand", now / 1000000u);
}

/* an import's: the clocks in MHz docked and in the hand, 0 for the
system's own. Returns the clock now (MHz), or 0 where it cannot be set. */
static unsigned int clock_choose(struct clock *clock, unsigned int docked_megahertz, unsigned int handheld_megahertz)
{
	uint32_t now = 0;

	mutexLock(&clock_lock);
	if (!docked_megahertz && !handheld_megahertz && !clock->opened)
	{
		/* (the system's own, and never changed: nothing to open) */
		mutexUnlock(&clock_lock);
		return 0;
	}
	if (!clock_open(clock))
	{
		mutexUnlock(&clock_lock);
		return 0;
	}
	clock->docked_hz = clock_rate(clock, docked_megahertz);
	clock->handheld_hz = clock_rate(clock, handheld_megahertz);
	clock_set(clock, 0);
	clkrstGetClockRate(&clock->session, &now);
	mutexUnlock(&clock_lock);
	return now / 1000000u;
}

unsigned int host_cpu_clock(unsigned int docked_megahertz, unsigned int handheld_megahertz)
{
	return clock_choose(&cpu, docked_megahertz, handheld_megahertz);
}

unsigned int host_gpu_clock(unsigned int docked_megahertz, unsigned int handheld_megahertz)
{
	return clock_choose(&gpu, docked_megahertz, handheld_megahertz);
}

/* the heartbeat's, and after the display changes (host_gl.c): the clocks
for the mode the console is in now, or again if the system has set its own */
void host_clock_keep(void)
{
	mutexLock(&clock_lock);
	clock_set(&cpu, 0);
	clock_set(&gpu, 0);
	mutexUnlock(&clock_lock);
}

/* the shutdown's: each clock as the system had it in this mode */
void host_clock_restore(void)
{
	mutexLock(&clock_lock);
	clock_set(&cpu, 1);
	clock_set(&gpu, 1);
	if (cpu.opened > 0)
		clkrstCloseSession(&cpu.session);
	if (gpu.opened > 0)
		clkrstCloseSession(&gpu.session);
	cpu.opened = gpu.opened = 0;
	if (service_opened)
	{
		clkrstExit();
		if (apm_opened)
			apmExit();
	}
	service_opened = apm_opened = 0;
	mutexUnlock(&clock_lock);
}
