/* Kernel-backed randomness. No clock, address or PRNG fallback.
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/entropy.h>
#include <reent.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
int _getentropy_r(struct _reent *r, void *buffer, size_t bytes)
{
    if (bytes > 256) { r->_errno = EINVAL; return -1; }
    int e = ciuki_error(CU_CALL(GETENTROPY, CU_PTR(buffer), bytes, 0, 0, 0, 0));
    if (e) { r->_errno = e; return -1; }
    return 0;
}
int getentropy(void *buffer, size_t bytes)
{
    if (bytes > 256) { errno = EINVAL; return -1; }
    int e = ciuki_error(CU_CALL(GETENTROPY, CU_PTR(buffer), bytes, 0, 0, 0, 0));
    if (e) { errno = e; return -1; }
    return 0;
}
int ciuki_getentropy_wait(void *buffer, size_t bytes)
{
    while (getentropy(buffer, bytes)) {
        if (errno != EAGAIN) return -1;
        struct timespec delay = {0, 10000000, 0};
        if (nanosleep(&delay, NULL)) return -1;
    }
    return 0;
}
void arc4random_buf(void *buffer, size_t bytes)
{
    int saved = errno;
    unsigned char *p = buffer;
    while (bytes) {
        size_t n = bytes > 256 ? 256 : bytes;
        if (ciuki_getentropy_wait(p, n)) abort();
        p += n; bytes -= n;
    }
    errno = saved;
}
uint32_t arc4random(void)
{
    uint32_t value; arc4random_buf(&value, sizeof(value)); return value;
}
uint32_t arc4random_uniform(uint32_t upper)
{
    if (upper < 2) return 0;
    uint32_t minimum = (uint32_t)(-upper) % upper, value;
    do value = arc4random(); while (value < minimum);
    return value % upper;
}
