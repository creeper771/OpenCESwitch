/*
HOST_NETWORK.C

The guest's network functions (port/linux/src/posix.h's sockets, UPnP,
DTLS, Discord and process functions, which posix_net.c and its neighbours
are on Android), as on a machine without a network, until the host has
BSD sockets: a socket cannot be made, a select waits out its timeout and
finds nothing, and there is no Discord client.

They must fail rather than seem to work: the stubs for missing imports
return 0, which made a socket (descriptor 0) whose select returned at once,
so internet play's thread (p2p.c) spun holding p2p_lock, which the game's
main thread needs each frame, and the game drew about a frame a second.

posix_random_bytes, posix_process_id and posix_command_line_argument are
complete.
*/

#include "host.h"
#include "../../linux/src/posix.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* (each failure sets newlib's errno, which host_errno gives the guest as
Linux's) */
static int no_network(void)
{
	errno = ENETDOWN;
	return -1;
}

static int no_socket(void)
{
	errno = EBADF;
	return -1;
}

static int network_socket_last_error(void)
{
	return host_linux_errno(ENETDOWN);
}

static int network_socket(int family, int type, int protocol)
{
	(void)family;
	(void)type;
	(void)protocol;
	return no_network();
}

static int network_socket_close(int socket)
{
	(void)socket;
	return no_socket();
}

static int network_socket_address(int socket, const void *address, int address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return no_socket();
}

static int network_socket_listen(int socket, int backlog)
{
	(void)socket;
	(void)backlog;
	return no_socket();
}

static int network_socket_accept(int socket, void *address, int *address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return no_socket();
}

static int network_socket_send(int socket, const void *buffer, int length, int flags)
{
	(void)socket;
	(void)buffer;
	(void)length;
	(void)flags;
	return no_socket();
}

static int network_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	(void)address;
	(void)address_length;
	return network_socket_send(socket, buffer, length, flags);
}

static int network_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	(void)socket;
	(void)buffer;
	(void)length;
	(void)flags;
	(void)address;
	(void)address_length;
	return no_socket();
}

static int network_socket_option(int socket, int value)
{
	(void)socket;
	(void)value;
	return no_socket();
}

static int network_socket_bytes_available(int socket, posix_ulong *count)
{
	(void)socket;
	*count = 0;
	return no_socket();
}

static int network_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	(void)socket;
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	return no_socket();
}

static int network_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	(void)socket;
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	return no_socket();
}

/* nothing is ever ready: waits out the timeout (a caller loops on it) */
static int network_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	(void)read;
	(void)write;
	(void)error;
	if (read_count)
		*read_count = 0;
	if (write_count)
		*write_count = 0;
	if (error_count)
		*error_count = 0;
	if (infinite)
		timeout_seconds = 1;
	svcSleepThread((int64_t)timeout_seconds * 1000000000LL + (int64_t)timeout_microseconds * 1000LL);
	return 0;
}

static posix_ulong network_local_ipv4_address(void)
{
	return 0;
}

static void network_random_bytes(void *buffer, posix_ulong size)
{
	randomGet(buffer, size);
}

static posix_ulong network_resolve_ipv4(const char *host)
{
	(void)host;
	return 0;
}

static int network_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "no network on the Switch yet");
	return 0;
}

static void network_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}

static int network_dtls_fingerprint(unsigned char *fingerprint)
{
	(void)fingerprint;
	return 0;
}

static int network_dtls_open(void)
{
	return -1;
}

static void network_dtls_close(int handle)
{
	(void)handle;
}

static void network_dtls_input(int handle, const void *data, int size)
{
	(void)handle;
	(void)data;
	(void)size;
}

static int network_dtls_receive(int handle, posix_ulong now, void *buffer, int size)
{
	(void)handle;
	(void)now;
	(void)buffer;
	(void)size;
	return -1;
}

static int network_dtls_buffer(int handle, void *buffer, int size)
{
	(void)handle;
	(void)buffer;
	(void)size;
	return 0;
}

static void network_hmac_sha1(const void *key, int key_size, const void *data, int size, unsigned char *digest)
{
	(void)key;
	(void)key_size;
	(void)data;
	(void)size;
	memset(digest, 0, 20);
}

static int network_command_line_argument(int index, char *buffer, posix_ulong size)
{
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

static posix_ulong network_process_id(void)
{
	return 1;
}

static int network_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

static int network_user_secret(unsigned char *secret, int size)
{
	(void)secret;
	(void)size;
	return 0;
}

static int network_discord_connect(void)
{
	return -1;
}

static int network_discord_transfer(int handle, void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

static void network_discord_close(int handle)
{
	(void)handle;
}

/* ---------- the imports */

const struct host_import host_network_imports[] =
{
	{"hostposix_socket_last_error", (void *)network_socket_last_error},
	{"hostposix_socket", (void *)network_socket},
	{"hostposix_socket_close", (void *)network_socket_close},
	{"hostposix_socket_bind", (void *)network_socket_address},
	{"hostposix_socket_connect", (void *)network_socket_address},
	{"hostposix_socket_listen", (void *)network_socket_listen},
	{"hostposix_socket_accept", (void *)network_socket_accept},
	{"hostposix_socket_send", (void *)network_socket_send},
	{"hostposix_socket_sendto", (void *)network_socket_sendto},
	{"hostposix_socket_recv", (void *)network_socket_send},
	{"hostposix_socket_recvfrom", (void *)network_socket_recvfrom},
	{"hostposix_socket_shutdown", (void *)network_socket_option},
	{"hostposix_socket_set_nonblocking", (void *)network_socket_option},
	{"hostposix_socket_bytes_available", (void *)network_socket_bytes_available},
	{"hostposix_socket_set_nodelay", (void *)network_socket_option},
	{"hostposix_socket_setsockopt", (void *)network_socket_setsockopt},
	{"hostposix_socket_getsockopt", (void *)network_socket_getsockopt},
	{"hostposix_socket_getsockname", (void *)network_socket_accept},
	{"hostposix_socket_getpeername", (void *)network_socket_accept},
	{"hostposix_socket_select", (void *)network_socket_select},
	{"hostposix_local_ipv4_address", (void *)network_local_ipv4_address},
	{"hostposix_random_bytes", (void *)network_random_bytes},
	{"hostposix_resolve_ipv4", (void *)network_resolve_ipv4},
	{"hostposix_upnp_forward_udp", (void *)network_upnp_forward_udp},
	{"hostposix_upnp_stop_forwarding_udp", (void *)network_upnp_stop_forwarding_udp},
	{"hostposix_dtls_fingerprint", (void *)network_dtls_fingerprint},
	{"hostposix_dtls_open", (void *)network_dtls_open},
	{"hostposix_dtls_close", (void *)network_dtls_close},
	{"hostposix_dtls_input", (void *)network_dtls_input},
	{"hostposix_dtls_receive", (void *)network_dtls_receive},
	{"hostposix_dtls_send", (void *)network_dtls_buffer},
	{"hostposix_dtls_output", (void *)network_dtls_buffer},
	{"hostposix_dtls_peer_fingerprint", (void *)network_dtls_fingerprint},
	{"hostposix_hmac_sha1", (void *)network_hmac_sha1},
	{"hostposix_command_line_argument", (void *)network_command_line_argument},
	{"hostposix_process_id", (void *)network_process_id},
	{"hostposix_register_url_scheme", (void *)network_register_url_scheme},
	{"hostposix_user_secret", (void *)network_user_secret},
	{"hostposix_discord_connect", (void *)network_discord_connect},
	{"hostposix_discord_write", (void *)network_discord_transfer},
	{"hostposix_discord_read", (void *)network_discord_transfer},
	{"hostposix_discord_close", (void *)network_discord_close},
	{NULL, NULL}
};
