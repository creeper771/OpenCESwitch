/*
HOST_CLOCK.C

The CPU's clock, which the game may raise (switch.cpu_clock, through the
host_cpu_clock import): the game's main thread, on one core, is what slows
its heaviest scenes, and the system runs games at 1020 MHz. Set through
the clock service (clkrst), which a homebrew program may use as sys-clk
does; the system sets its own clock again when the console is docked or
undocked, or wakes, so it is set again at once when the display changes
and for a few seconds after (host_gl.c), and by each heartbeat
(host_clock_keep); the shutdown puts back the clock the program found
(host_clock_restore).
*/

#include "host.h"

/* the clocks the setting takes, in MHz: the system's, and those the CPU's
own table has above it (1785 is the system's own while a game loads) */
static const uint32_t clocks[] = { 1224, 1428, 1581, 1785 };

static Mutex clock_lock;
static int opened;
static ClkrstSession session;
/* the clock the program found (Hz), and the one asked for (0: the
system's) */
static uint32_t found_hz;
static uint32_t wanted_hz;

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
	opened = 1;
	return 1;
}

/* sets the clock if it is not the one wanted; the clock lock held */
static void clock_set(void)
{
	uint32_t hz = wanted_hz ? wanted_hz : found_hz, now = 0;
	Result rc;

	if (R_SUCCEEDED(clkrstGetClockRate(&session, &now)) && now == hz)
		return;
	rc = clkrstSetClockRate(&session, hz);
	if (R_FAILED(rc))
		host_logf(HOST_LOG_WARN, "clock: cannot set the CPU to %u MHz (0x%x)", hz / 1000000u, rc);
	else
		host_logf(HOST_LOG_INFO, "clock: CPU at %u MHz (it was %u)", hz / 1000000u, now / 1000000u);
}

/* the import: the CPU's clock in MHz, 0 for the system's own; a value
between those the setting takes is the next one down. Returns the clock
asked for (MHz), or 0 where it cannot be set. */
unsigned int host_cpu_clock(unsigned int megahertz)
{
	uint32_t hz = 0;
	unsigned int index;

	for (index = 0; index < sizeof(clocks) / sizeof(clocks[0]); index++)
	{
		if (megahertz >= clocks[index])
			hz = clocks[index] * 1000000u;
	}
	mutexLock(&clock_lock);
	if (!hz && !opened)
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
	wanted_hz = hz;
	clock_set();
	mutexUnlock(&clock_lock);
	return (hz ? hz : found_hz) / 1000000u;
}

/* the heartbeat's: the clock again, if the system has set its own */
void host_clock_keep(void)
{
	mutexLock(&clock_lock);
	if (opened > 0 && wanted_hz)
		clock_set();
	mutexUnlock(&clock_lock);
}

/* the shutdown's: the clock the program found */
void host_clock_restore(void)
{
	mutexLock(&clock_lock);
	if (opened > 0)
	{
		wanted_hz = 0;
		clock_set();
		clkrstCloseSession(&session);
		clkrstExit();
		opened = 0;
	}
	mutexUnlock(&clock_lock);
}
