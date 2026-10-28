/* SPDX-License-Identifier: MIT
 * sec-user-05: one bounded exchange with the SYSTEM account service
 * (ACCOUNTS, service 2) for the native identity adapters (pwd.c). Each call
 * connects through call 101, sends the copied body in CSC1 fragments of at
 * most 192 bytes, checks every reply's sender, endpoint generation, request id
 * and offsets, and closes the link before returning: no descriptor or
 * partial reply outlives the call, and no caller value is an authority (the
 * service authorizes with the kernel-retained subject of the link). */
#include <ciuki/runtime.h>
#include <ciuki/security.h>
#include <ciuki/channel.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int __ciuki_account_rpc(uint32_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t now_ms(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (uint64_t)t.tv_sec * 1000 + (uint32_t)t.tv_nsec / 1000000;
}
static void pause_1ms(void)
{
    struct timespec t = {.tv_nsec = 1000000};
    nanosleep(&t, NULL);
}
static uint32_t serial;
/* Returns 0, the service's application error (errno value), or EPIPE for a
 * broken/malformed exchange and ETIMEDOUT after the frozen 5 s bound. */
int __ciuki_account_rpc(uint32_t op, const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used)
{
    if (!used || n > CIUKI_SECURITY_SERVICE_BODY_MAX || (!in && n) || (!out && cap)) return EINVAL;
    *used = 0;
    struct ciuki_security_link q = {.version = 1, .size = sizeof(q), .service = CIUKI_SECURITY_SERVICE_ACCOUNTS}, r;
    memset(&r, 0, sizeof(r));
    struct ciuki_security_args a = {.version = CIUKI_SECURITY_VERSION, .size = sizeof(a),
        .operation = CIUKI_SECURITY_SERVICE_CONNECT, .spawn_address = CU_PTR(&q),
        .output_address = CU_PTR(&r), .output_bytes = sizeof(r)};
    if (ciuki_security(&a)) return errno ? errno : EPIPE;
    if (r.endpoint_fd < 0) return EPIPE;
    int fd = r.endpoint_fd, status = EPIPE;
    uint32_t id = __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
    uint64_t deadline = now_ms() + CIUKI_SECURITY_SERVICE_TIMEOUT_MS;
    if (r.server_role != CIUKI_SECURITY_ROLE_ACCOUNT || !r.endpoint_generation) goto done;
    uint32_t at = 0;
    do {
        struct ciuki_message m;
        memset(&m, 0, sizeof(m));
        uint8_t *h = m.data;
        uint32_t take = n - at;
        if (take > CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) take = CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
        wr32(h, 0x31435343u); wr32(h + 4, 1); wr32(h + 8, 64); wr32(h + 12, CIUKI_SECURITY_SERVICE_ACCOUNTS);
        wr32(h + 16, op); wr32(h + 20, 1); wr32(h + 24, id); wr32(h + 32, r.endpoint_generation);
        wr32(h + 36, take); wr32(h + 52, n); wr32(h + 56, at);
        if (n == 312) {
            wr32(h + 40, rd32((const uint8_t *)in + 40));
            wr32(h + 44, rd32((const uint8_t *)in + 44));
        }
        if (take) memcpy(h + 64, (const uint8_t *)in + at, take);
        m.length = 64 + take;
        while (ciuki_channel_send(fd, &m, 1) < 0) {
            if (errno != EAGAIN) goto done;
            if (now_ms() >= deadline) { status = ETIMEDOUT; goto done; }
            pause_1ms();
        }
        at += take;
    } while (at < n);
    uint32_t total = 0;
    at = 0;
    do {
        struct ciuki_message m;
        memset(&m, 0, sizeof(m));
        int got;
        while ((got = ciuki_channel_recv(fd, &m, 1)) < 0 && errno == EAGAIN) {
            if (now_ms() >= deadline) { status = ETIMEDOUT; goto done; }
            pause_1ms();
        }
        if (got != 1) goto done;
        if (m.fd_count) {
            for (unsigned i = 0; i < m.fd_count && i < 4; i++) close(m.fds[i]);
            goto done;
        }
        const uint8_t *h = m.data;
        uint32_t take = rd32(h + 36), length = rd32(h + 52);
        if (m.sender_pid != r.peer_pid || m.length < 64 || rd32(h) != 0x31435343u || rd32(h + 4) != 1 ||
            rd32(h + 8) != 64 || rd32(h + 12) != CIUKI_SECURITY_SERVICE_ACCOUNTS || rd32(h + 16) != op ||
            rd32(h + 20) != 2 || rd32(h + 24) != id || rd32(h + 28) || rd32(h + 32) != r.endpoint_generation ||
            take > CIUKI_SECURITY_SERVICE_FRAGMENT_MAX || m.length != 64 + take ||
            length > CIUKI_SECURITY_SERVICE_BODY_MAX || length > cap || rd32(h + 56) != at || rd32(h + 60))
            goto done;
        int32_t s = (int32_t)rd32(h + 48);
        if (s) {
            if (s < 0 && !at && !length && !take) status = -s;
            goto done;
        }
        if (!at) total = length;
        else if (total != length) goto done;
        if (take > total - at || (!take && total)) goto done;
        if (take) memcpy((uint8_t *)out + at, h + 64, take);
        at += take;
    } while (at < total);
    *used = at;
    status = 0;
done:
    close(fd);
    return status;
}
