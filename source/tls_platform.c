#include <mbedtls/entropy.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/threading.h>
#include <switch.h>

int mbedtls_hardware_poll(void *ctx, unsigned char *data, size_t length, size_t *written) {
    (void)ctx;
    *written = 0;
    if (R_FAILED(csrngGetRandomBytes(data, length))) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *written = length;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(armTicksToNs(armGetSystemTick()) / 1000000);
}

static void tls_mutex_free(Mutex *mutex) {
    (void)mutex;
}

static int tls_mutex_lock(Mutex *mutex) {
    mutexLock(mutex);
    return 0;
}

static int tls_mutex_unlock(Mutex *mutex) {
    mutexUnlock(mutex);
    return 0;
}

void nx_tls_threading_init(void) {
    mbedtls_threading_set_alt(mutexInit, tls_mutex_free, tls_mutex_lock, tls_mutex_unlock);
}

void nx_tls_threading_exit(void) {
    mbedtls_threading_free_alt();
}
