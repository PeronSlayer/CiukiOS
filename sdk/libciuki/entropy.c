/* Kernel-backed randomness. No clock, address or PRNG fallback.
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/entropy.h>
#include <reent.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
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
int ciuki_entropy_wait(void *buffer, size_t bytes)
{
    while (getentropy(buffer, bytes)) {
        if (errno != EAGAIN) return -1;
        struct timespec delay = {0, 10000000, 0};
        if (nanosleep(&delay, NULL)) return -1;
    }
    return 0;
}
int ciuki_getentropy_wait(void *buffer, size_t bytes)
{
    return ciuki_entropy_wait(buffer, bytes);
}
struct entropy_deadline { struct timespec start; int active; };
static _Noreturn void unavailable(void)
{
    static const char message[] = "arc4random: kernel entropy unavailable (5 s EAGAIN deadline or permanent error); aborting\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    abort();
}
static void random_bytes(void *buffer, size_t bytes, struct entropy_deadline *deadline)
{
    unsigned char *p = buffer;
    while (bytes) {
        size_t n = bytes > 256 ? 256 : bytes;
        for (;;) {
            struct timespec now;
            if (deadline->active) {
                if (clock_gettime(CLOCK_MONOTONIC, &now)) unavailable();
                time_t seconds = now.tv_sec - deadline->start.tv_sec;
                if (now.tv_nsec < deadline->start.tv_nsec) seconds--;
                if (seconds >= 5 || seconds < 0) unavailable();
            }
            if (!getentropy(p, n)) break;
            if (errno != EAGAIN) unavailable();
            if (!deadline->active) {
                if (clock_gettime(CLOCK_MONOTONIC, &deadline->start)) unavailable();
                deadline->active = 1;
            }
            struct timespec delay = {0, 10000000, 0};
            if (nanosleep(&delay, NULL) && errno != EINTR) unavailable();
        }
        p += n; bytes -= n;
    }
}
void arc4random_buf(void *buffer, size_t bytes)
{
    int saved = errno;
    struct entropy_deadline deadline = {0};
    random_bytes(buffer, bytes, &deadline);
    errno = saved;
}
uint32_t arc4random(void)
{
    uint32_t value; arc4random_buf(&value, sizeof(value)); return value;
}
uint32_t arc4random_uniform(uint32_t upper)
{
    if (upper < 2) return 0;
    int saved = errno;
    struct entropy_deadline deadline = {0};
    uint32_t minimum = (uint32_t)(-upper) % upper, value;
    do random_bytes(&value, sizeof(value), &deadline); while (value < minimum);
    errno = saved;
    return value % upper;
}
