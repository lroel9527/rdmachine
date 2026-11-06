// Additions to Mbed TLS's default configuration for rdmachine.
//
// The control server handles connections on several threads, and TLS 1.3 in
// Mbed TLS 3.6 goes through the global PSA crypto state, so the library must
// be built thread-safe.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
// Self-tests are not used, and their global multiplication counter in ecp.c
// is updated without locking (harmless, but it trips ThreadSanitizer).
#undef MBEDTLS_SELF_TEST
