/*
HOST_NETWORK.C

The guest's network functions (port/linux/src/posix.h's sockets, UPnP,
DTLS, Discord and process functions), as on Android: posix_net.c,
posix_upnp.c (with port/third_party/miniupnpc) and posix_dtls.c (with
port/third_party/mbedtls), built on libnx's sockets, the system's BSD ones
(posix_net.c turns the guest's addresses to theirs). This file starts and
stops them (host_network_start, host_network_stop) and gives the process
functions that are the host's: random numbers from the system's, one
process, no command line.

The sockets' buffers are libnx's defaults made larger for internet play's
thread, which sends to and receives from every player of a game at once.
*/

#include "host.h"
#include "../../linux/src/posix.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* (libnx's, which newlib does not declare) */
long gethostid(void);

/* each datagram socket's buffers (the game's and internet play's are all
datagrams but the brokers' one): larger than libnx's defaults, for
internet play's tunnel, which carries all of a game's traffic */
#define BUFFER_SEND 0x8000
#define BUFFER_RECEIVE 0x20000

static int network_started;
/* the network service, open while the game runs, and the game's request for
the network in it, held as long: as a game's, without which the system
says nothing of the network (libnx's gethostid, posix_net.c's local
address, gave 127.0.0.1) and need not keep the console on it */
static int nifm_opened;
static NifmRequest nifm_request;
static int nifm_requested;

/* the network's own broadcast address (its address with the host part all
ones), from the network service, as a HALO_NET_BROADCAST beside
255.255.255.255 (port/linux/src/xnet.c: network.broadcast), into setting;
"" if there is no network. Horizon's sockets may deliver only the
network's own, which the homebrew of the Switch's local play sends to. */
static void network_broadcast(char *setting, size_t size)
{
	u32 address = 0, mask = 0, gateway = 0, dns[2];

	setting[0] = 0;
	if (!nifm_opened)
		return;
	if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&address, &mask, &gateway, &dns[0], &dns[1])) && address && mask &&
		mask != 0xffffffffu)
	{
		/* (in network order, as the sockets give them) */
		u32 broadcast = address | ~mask;
		const unsigned char *bytes = (const unsigned char *)&broadcast;

		snprintf(setting, size, "HALO_NET_BROADCAST=255.255.255.255,%u.%u.%u.%u", bytes[0], bytes[1], bytes[2],
			bytes[3]);
		host_logf(HOST_LOG_INFO, "network: broadcasts to 255.255.255.255 and %u.%u.%u.%u", bytes[0], bytes[1],
			bytes[2], bytes[3]);
	}
	nifmExit();
}

void host_network_start(char *broadcast, size_t broadcast_size)
{
	SocketInitConfig config = *socketGetDefaultInitConfig();
	Result rc;

	broadcast[0] = 0;
	config.udp_tx_buf_size = BUFFER_SEND;
	config.udp_rx_buf_size = BUFFER_RECEIVE;
	config.sb_efficiency = 8;
	rc = socketInitialize(&config);
	if (R_FAILED(rc))
	{
		host_logf(HOST_LOG_WARN, "network: the sockets cannot start (0x%x): no network play", rc);
		return;
	}
	network_started = 1;
	nifm_opened = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
	if (!nifm_opened)
		host_logf(HOST_LOG_WARN, "network: the network service cannot be used: no local address");
	else if (R_SUCCEEDED(rc = nifmCreateRequest(&nifm_request, true)))
	{
		nifm_requested = 1;
		rc = nifmRequestSubmitAndWait(&nifm_request);
		if (R_SUCCEEDED(rc))
			rc = nifmGetResult(&nifm_request);
		if (R_FAILED(rc))
			host_logf(HOST_LOG_WARN, "network: the system gave no network (0x%x): is the console connected?", rc);
	}
	{
		u32 address = 0;
		const unsigned char *bytes = (const unsigned char *)&address;

		if (nifm_opened)
			nifmGetCurrentIpAddress(&address);
		host_logf(HOST_LOG_INFO, "network: sockets started; this console's address is %u.%u.%u.%u", bytes[0],
			bytes[1], bytes[2], bytes[3]);
	}
	network_broadcast(broadcast, broadcast_size);
}

void host_network_stop(void)
{
	if (nifm_requested)
		nifmRequestClose(&nifm_request);
	nifm_requested = 0;
	if (nifm_opened)
		nifmExit();
	nifm_opened = 0;
	if (network_started)
		socketExit();
	network_started = 0;
}

/* ---------- failures, logged

Each socket call that fails is logged (stderr.txt), but a non-blocking one
that would block, which the game waits on as it should: the first
LOGGED_FAILURES of them, which say where network play stops. */

#define WSAEWOULDBLOCK 10035
#define LOGGED_FAILURES 200

static int logged_failures;

static int network_failed(const char *call, int socket, int result)
{
	int error;

	if (result >= 0)
		return result;
	error = posix_socket_last_error();
	if (error != WSAEWOULDBLOCK && __atomic_add_fetch(&logged_failures, 1, __ATOMIC_RELAXED) <= LOGGED_FAILURES)
		fprintf(stderr, "network: %s on socket %d failed: Winsock error %d (errno %d)\n", call, socket, error, errno);
	return result;
}

static int logged_socket(int family, int type, int protocol)
{
	return network_failed("socket", -1, posix_socket(family, type, protocol));
}

static int logged_close(int socket)
{
	return network_failed("close", socket, posix_socket_close(socket));
}

static int logged_bind(int socket, const void *address, int address_length)
{
	return network_failed("bind", socket, posix_socket_bind(socket, address, address_length));
}

static int logged_connect(int socket, const void *address, int address_length)
{
	return network_failed("connect", socket, posix_socket_connect(socket, address, address_length));
}

static int logged_listen(int socket, int backlog)
{
	return network_failed("listen", socket, posix_socket_listen(socket, backlog));
}

static int logged_accept(int socket, void *address, int *address_length)
{
	return network_failed("accept", socket, posix_socket_accept(socket, address, address_length));
}

static int logged_send(int socket, const void *buffer, int length, int flags)
{
	return network_failed("send", socket, posix_socket_send(socket, buffer, length, flags));
}

static int logged_sendto(int socket, const void *buffer, int length, int flags, const void *address,
	int address_length)
{
	return network_failed("sendto", socket, posix_socket_sendto(socket, buffer, length, flags, address,
		address_length));
}

static int logged_recv(int socket, void *buffer, int length, int flags)
{
	return network_failed("recv", socket, posix_socket_recv(socket, buffer, length, flags));
}

static int logged_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length)
{
	return network_failed("recvfrom", socket, posix_socket_recvfrom(socket, buffer, length, flags, address,
		address_length));
}

static int logged_shutdown(int socket, int how)
{
	return network_failed("shutdown", socket, posix_socket_shutdown(socket, how));
}

static int logged_set_nonblocking(int socket, int nonblocking)
{
	return network_failed("set_nonblocking", socket, posix_socket_set_nonblocking(socket, nonblocking));
}

static int logged_bytes_available(int socket, posix_ulong *count)
{
	return network_failed("bytes_available", socket, posix_socket_bytes_available(socket, count));
}

static int logged_set_nodelay(int socket)
{
	return network_failed("set_nodelay", socket, posix_socket_set_nodelay(socket));
}

/* A buffer as large as the game and internet play ask for, 1 MB each way
for each datagram socket, is not taken: the system's sockets share one
pool, eight sockets' worth of BUFFER_SEND and BUFFER_RECEIVE
(host_network_start), and one that took a buffer that large left the
others none, so that a game could not be made, and after a while none
joined. A size up to the pool's is set; a larger one is the pool's. */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002

static int logged_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int size;

	if (level == WINSOCK_SOL_SOCKET && (name == WINSOCK_SO_SNDBUF || name == WINSOCK_SO_RCVBUF) &&
		length == (int)sizeof(int) && *(const int *)value > (name == WINSOCK_SO_SNDBUF ? BUFFER_SEND : BUFFER_RECEIVE))
	{
		size = name == WINSOCK_SO_SNDBUF ? BUFFER_SEND : BUFFER_RECEIVE;
		return network_failed("setsockopt", socket, posix_socket_setsockopt(socket, level, name, &size, sizeof(size)));
	}
	return network_failed("setsockopt", socket, posix_socket_setsockopt(socket, level, name, value, length));
}

static int logged_getsockopt(int socket, int level, int name, void *value, int *length)
{
	return network_failed("getsockopt", socket, posix_socket_getsockopt(socket, level, name, value, length));
}

static int logged_getsockname(int socket, void *address, int *address_length)
{
	return network_failed("getsockname", socket, posix_socket_getsockname(socket, address, address_length));
}

static int logged_getpeername(int socket, void *address, int *address_length)
{
	/* (not connected yet, which the game asks to find out: not logged) */
	return posix_socket_getpeername(socket, address, address_length);
}

static int logged_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count,
	posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	return network_failed("select", -1, posix_socket_select(read, read_count, write, write_count, error,
		error_count, timeout_seconds, timeout_microseconds, infinite));
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	randomGet(buffer, size);
}

posix_ulong posix_process_id(void)
{
	return 1;
}

/* mbedtls's entropy and clock (compat/mbedtls_switch_config.h) */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *written)
{
	(void)data;
	randomGet(output, length);
	*written = length;
	return 0;
}

int64_t mbedtls_ms_time(void)
{
	return (int64_t)(host_now() / 1000000ULL);
}

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

/* ---------- the imports */

const struct host_import host_network_imports[] =
{
	{"hostposix_socket_last_error", (void *)posix_socket_last_error},
	{"hostposix_socket", (void *)logged_socket},
	{"hostposix_socket_close", (void *)logged_close},
	{"hostposix_socket_bind", (void *)logged_bind},
	{"hostposix_socket_connect", (void *)logged_connect},
	{"hostposix_socket_listen", (void *)logged_listen},
	{"hostposix_socket_accept", (void *)logged_accept},
	{"hostposix_socket_send", (void *)logged_send},
	{"hostposix_socket_sendto", (void *)logged_sendto},
	{"hostposix_socket_recv", (void *)logged_recv},
	{"hostposix_socket_recvfrom", (void *)logged_recvfrom},
	{"hostposix_socket_shutdown", (void *)logged_shutdown},
	{"hostposix_socket_set_nonblocking", (void *)logged_set_nonblocking},
	{"hostposix_socket_bytes_available", (void *)logged_bytes_available},
	{"hostposix_socket_set_nodelay", (void *)logged_set_nodelay},
	{"hostposix_socket_setsockopt", (void *)logged_setsockopt},
	{"hostposix_socket_getsockopt", (void *)logged_getsockopt},
	{"hostposix_socket_getsockname", (void *)logged_getsockname},
	{"hostposix_socket_getpeername", (void *)logged_getpeername},
	{"hostposix_socket_select", (void *)logged_select},
	{"hostposix_local_ipv4_address", (void *)posix_local_ipv4_address},
	{"hostposix_random_bytes", (void *)posix_random_bytes},
	{"hostposix_resolve_ipv4", (void *)posix_resolve_ipv4},
	{"hostposix_upnp_forward_udp", (void *)posix_upnp_forward_udp},
	{"hostposix_upnp_stop_forwarding_udp", (void *)posix_upnp_stop_forwarding_udp},
	{"hostposix_dtls_fingerprint", (void *)posix_dtls_fingerprint},
	{"hostposix_dtls_open", (void *)posix_dtls_open},
	{"hostposix_dtls_close", (void *)posix_dtls_close},
	{"hostposix_dtls_input", (void *)posix_dtls_input},
	{"hostposix_dtls_receive", (void *)posix_dtls_receive},
	{"hostposix_dtls_send", (void *)posix_dtls_send},
	{"hostposix_dtls_output", (void *)posix_dtls_output},
	{"hostposix_dtls_peer_fingerprint", (void *)posix_dtls_peer_fingerprint},
	{"hostposix_hmac_sha1", (void *)posix_hmac_sha1},
	{"hostposix_command_line_argument", (void *)posix_command_line_argument},
	{"hostposix_process_id", (void *)posix_process_id},
	{"hostposix_register_url_scheme", (void *)posix_register_url_scheme},
	{"hostposix_user_secret", (void *)posix_user_secret},
	{"hostposix_discord_connect", (void *)posix_discord_connect},
	{"hostposix_discord_write", (void *)posix_discord_write},
	{"hostposix_discord_read", (void *)posix_discord_read},
	{"hostposix_discord_close", (void *)posix_discord_close},
	{NULL, NULL}
};
