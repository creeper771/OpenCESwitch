/*
SYS/UN.H

A Unix socket's address, for port/third_party/miniupnpc (its minissdpc.c
asks a local minissdpd over one): libnx's sockets have no such sockets, so
a connect to one fails and miniupnpc looks for routers itself, as it does
where there is no minissdpd.
*/

#ifndef SWITCH_COMPAT_SYS_UN_H
#define SWITCH_COMPAT_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un
{
	unsigned char sun_len;
	sa_family_t sun_family;
	char sun_path[104];
};

#endif
