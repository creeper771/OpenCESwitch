/*
MBEDTLS_SWITCH_CONFIG.H

port/third_party/mbedtls's configuration on the Switch (its
MBEDTLS_USER_CONFIG_FILE, which leaves the vendored copy as it is): its
platform code knows Unix and Windows only. The entropy is the system's
random generator (mbedtls_hardware_poll, host_network.c), the clock its
tick (mbedtls_ms_time); its own sockets and timers, which posix_dtls.c does
not use (its datagrams go through internet play's tunnel, its timers are
its own), are left out.
*/

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
