/* mbed TLS on kaiHEN: randomness from libnx (mbedtls_hardware_poll in kaihen_platform.c), no
 * timing module (it needs Unix signals). Sockets and time use the Unix code (unix=1 on the
 * mbed TLS targets only), which libnx's BSD sockets and newlib serve. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#undef MBEDTLS_TIMING_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT /* mbedtls_ms_time in kaihen_platform.c */
