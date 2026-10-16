/*
HOST_CLOCK.C

The CPU's clock, which the game may raise, docked and in the hand
(switch.cpu_clock_docked and switch.cpu_clock_handheld, through the
host_cpu_clock import): the game's main thread, on one core, is what slows
its heaviest scenes, and the system runs games at 1020 MHz. Set through
the clock service (clkrst), which a homebrew program may use as sys-clk
does, to the clock of the mode the console is in. The system may set its
own clock again when the console is docked, undocked or wakes, so it is
set again at once when the display changes and for a few seconds after
(host_gl.c), and by each heartbeat (host_clock_keep); the shutdown puts
back the clock the program found (host_clock_restore).
*/

#include "host.h"

/* the clocks the setting takes, in MHz: the system's, and those the CPU's
own table has above it (1785 is the system's own while a game loads) */
static const uint32_t clocks[] = { 1224, 1428, 1581, 1785 };

static Mutex clock_lock;
static int opened;
static ClkrstSession session;
static int apm_opened;
/* the clock the program found (Hz), and those asked for docked and in the
hand (0: the system's) */
static uint32_t found_hz;
static uint32_t docked_hz, handheld_hz;

static int clock_open(void)
{
	Result rc;

	if (opened)
		return opened > 0;
	opened = -1;
	rc = clkrstInitialize();
	if (R_SUCCEEDED(rc))
	{
		rc = clkrstOpenSession(&session, PcvModuleId_CpuBus, 3);
		if (R_FAILED(rc))
			clkrstExit();
	}
	if (R_FAILED(rc))
	{
		host_logf(HOST_LOG_WARN, "clock: the clock service cannot be used (0x%x)", rc);
		return 0;
	}
	if (R_FAILED(clkrstGetClockRate(&session, &found_hz)))
		found_hz = 1020000000u;
	/* (the performance service says whether the console is docked: libnx's
	own operation mode is only kept by applet messages, which this program
	does not take while the game runs) */
	apm_opened = R_SUCCEEDED(apmInitialize());
	opened = 1;
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

/* sets the clock if it is not the one wanted, docked or in the hand; the
clock lock held */
static void clock_set(int restoring)
{
	int docked = clock_docked();
	uint32_t wanted = restoring ? 0 : docked ? docked_hz : handheld_hz;
	uint32_t hz = wanted ? wanted : found_hz, now = 0;
	Result rc;

	if (R_SUCCEEDED(clkrstGetClockRate(&session, &now)) && now == hz)
		return;
	rc = clkrstSetClockRate(&session, hz);
	if (R_FAILED(rc))
		host_logf(HOST_LOG_WARN, "clock: cannot set the CPU to %u MHz (0x%x)", hz / 1000000u, rc);
	else
		host_logf(HOST_LOG_INFO, "clock: CPU at %u MHz %s (it was %u)", hz / 1000000u,
			docked ? "docked" : "in the hand", now / 1000000u);
}

/* a setting's MHz as a clock the CPU takes (Hz): 0 for the system's own; a
value between those the setting takes is the next one down */
static uint32_t clock_of(unsigned int megahertz)
{
	uint32_t hz = 0;
	unsigned int index;

	for (index = 0; index < sizeof(clocks) / sizeof(clocks[0]); index++)
	{
		if (megahertz >= clocks[index])
			hz = clocks[index] * 1000000u;
	}
	return hz;
}

/* the import: the CPU's clocks in MHz docked and in the hand, 0 for the
system's own. Returns the clock set now (MHz), or 0 where it cannot be
set. */
unsigned int host_cpu_clock(unsigned int docked_megahertz, unsigned int handheld_megahertz)
{
	uint32_t docked = clock_of(docked_megahertz), handheld = clock_of(handheld_megahertz), now = 0;

	mutexLock(&clock_lock);
	if (!docked && !handheld && !opened)
	{
		/* (the system's own, and never changed: nothing to open) */
		mutexUnlock(&clock_lock);
		return 0;
	}
	if (!clock_open())
	{
		mutexUnlock(&clock_lock);
		return 0;
	}
	docked_hz = docked;
	handheld_hz = handheld;
	clock_set(0);
	clkrstGetClockRate(&session, &now);
	mutexUnlock(&clock_lock);
	return now / 1000000u;
}

/* the heartbeat's, and after the display changes (host_gl.c): the clock for
the mode the console is in now, or again if the system has set its own */
void host_clock_keep(void)
{
	mutexLock(&clock_lock);
	if (opened > 0)
		clock_set(0);
	mutexUnlock(&clock_lock);
}

/* the shutdown's: the clock the program found */
void host_clock_restore(void)
{
	mutexLock(&clock_lock);
	if (opened > 0)
	{
		clock_set(1);
		clkrstCloseSession(&session);
		clkrstExit();
		if (apm_opened)
			apmExit();
		opened = 0;
	}
	mutexUnlock(&clock_lock);
}
