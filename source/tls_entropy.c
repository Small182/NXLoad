#include <mbedtls/entropy.h>
#include <switch.h>

int mbedtls_hardware_poll(void *ctx, unsigned char *data, size_t length, size_t *written) {
    (void)ctx;
    *written = 0;
    if (R_FAILED(csrngGetRandomBytes(data, length))) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *written = length;
    return 0;
}
