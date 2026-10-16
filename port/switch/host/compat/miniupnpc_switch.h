/*
MINIUPNPC_SWITCH.H

Forced into port/third_party/miniupnpc's sources when the Switch host
compiles them: libnx's sockets have no interface names, which miniupnpc
reads only for IPv6 addresses' scopes and a multicast interface given by
name, neither of which internet play gives (posix_upnp.c).
*/

#ifndef SWITCH_MINIUPNPC_SWITCH_H
#define SWITCH_MINIUPNPC_SWITCH_H

#include <net/if.h>
#include <stddef.h>

static inline unsigned int if_nametoindex(const char *name)
{
	(void)name;
	return 0;
}

static inline char *if_indextoname(unsigned int index, char *name)
{
	(void)index;
	(void)name;
	return NULL;
}

#endif
