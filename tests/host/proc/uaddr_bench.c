/* Production user-copy/fault replay. Host wall time * 32 is the repository's
 * T23 estimate, not a hardware measurement. SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#undef CLOCKS_PER_SEC
#define HOST_PAGES 17000u
#define main proc_fixture_main
#include "proc_test.c"
#undef main

#define ARCHIVE_BASE 0x30000000u
#define ARCHIVE_BYTES (48u * 1024 * 1024)
#define ARCHIVE_EXTENTS 24u
static volatile uintptr_t bench_sink;
static uint32_t shared_frame;
static unsigned shared_refs;
static void shared_retain(void *object) { CHECK(object == &shared_frame); shared_refs++; }
static void shared_release(void *object) { CHECK(object == &shared_frame && shared_refs); shared_refs--; }
static uint32_t shared_page(void *object, uint32_t index)
{ (void)index; CHECK(object == &shared_frame && shared_frame); return shared_frame; }
static double now_ms(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static void result(unsigned count, const char *operation, unsigned calls, double start)
{
    double elapsed = now_ms() - start;
    printf("extents=%u operation=%s calls=%u host_ms=%.3f t23_32x_ms=%.3f\n",
           count, operation, calls, elapsed, elapsed * 32);
}
static void replay(unsigned count)
{
    struct uaddr u;
    CHECK(!ua_init(&u, 0));
    u.win32 = true;
    struct process p = { .memory = &u, .state = PROC_LIVE };
    struct proc_thread thread = { .process = &p, .task = &controller };
    threads[0] = &thread; g_current = &controller;
    shared_frame = pmm_alloc(); CHECK(shared_frame && !shared_refs);
    struct ua_shared shared = { .object = &shared_frame, .retain = shared_retain,
                                .release = shared_release, .page = shared_page };
    const enum ua_kind kinds[] = { UA_IMAGE, UA_HEAP, UA_W32, UA_SHARED, UA_STACK, UA_GUARD };
    for (unsigned i = 0; i < count - ARCHIVE_EXTENTS; i++) {
        enum ua_kind kind = kinds[i % 6];
        /* Actual shared views (one backing frame aliased by their pages),
         * bounded below the native arena's existing 256-extent quota. */
        if (kind == UA_SHARED && shared_refs < 64) {
            CHECK((uint32_t)ua_map_shared(&u, (1 + i % 8) * PAGE_SIZE, 3, 3, &shared) < (uint32_t)-4095);
            continue;
        }
        if (kind == UA_SHARED) kind = UA_W32;
        uint32_t prot = kind == UA_GUARD || kind == UA_SHARED || kind == UA_W32 ? 0 : PROT_READ | PROT_WRITE;
        CHECK(!ua_map_at(&u, 0x01000000u + i * 65536, (1 + i % 8) * PAGE_SIZE,
                         prot, PROT_READ | PROT_WRITE | PROT_EXEC, kind, 0));
    }
    for (unsigned i = 0; i < ARCHIVE_EXTENTS; i++) {
        CHECK(!ua_map_at(&u, ARCHIVE_BASE + i * (ARCHIVE_BYTES / ARCHIVE_EXTENTS),
                         ARCHIVE_BYTES / ARCHIVE_EXTENTS, PROT_READ | PROT_WRITE,
                         PROT_READ | PROT_WRITE, UA_W32, 0));
        struct ua_extent *e = ua_find(&u, ARCHIVE_BASE + i * (ARCHIVE_BYTES / ARCHIVE_EXTENTS));
        e->w32_state = W32_MEM_COMMIT;
    }
    CHECK(u.extents == count);
    double start = now_ms();
    uint32_t random = 0x51ac9;
    for (unsigned i = 0; i < 200000; i++) {
        random = random * 1664525u + 1013904223u;
        unsigned slot = random % (count - ARCHIVE_EXTENTS);
        bench_sink = (uintptr_t)ua_find(&u, 0x01000000u + slot * 65536 + ((random >> 16) & 65535));
    }
    result(count, "find-random-hit-miss", 200000, start);
    start = now_ms();
    for (unsigned off = 0; off < ARCHIVE_BYTES; off += PAGE_SIZE)
        bench_sink = (uintptr_t)ua_find(&u, ARCHIVE_BASE + off);
    result(count, "find-sequential-48MiB", ARCHIVE_BYTES / PAGE_SIZE, start);
    start = now_ms();
    for (unsigned off = 0; off < ARCHIVE_BYTES; off += PAGE_SIZE)
        CHECK(!ua_fault(&u, ARCHIVE_BASE + off, 6));
    result(count, "fault-demand-write-48MiB", ARCHIVE_BYTES / PAGE_SIZE, start);
    start = now_ms();
    for (unsigned off = 0; off < ARCHIVE_BYTES; off += PAGE_SIZE)
        CHECK(!ua_fault(&u, ARCHIVE_BASE + off, 4));
    result(count, "fault-resident-read-48MiB", ARCHIVE_BYTES / PAGE_SIZE, start);
    uint8_t buffer[65536]; memset(buffer, 0x5a, sizeof(buffer));
    for (unsigned chunk = PAGE_SIZE; chunk <= sizeof(buffer); chunk *= 16) {
        char label[64];
        start = now_ms();
        for (unsigned off = 0; off < ARCHIVE_BYTES; off += chunk)
            CHECK(!ua_user_write(&u, ARCHIVE_BASE + off, buffer, chunk));
        snprintf(label, sizeof(label), "ua_user_write-48MiB-%u", chunk);
        result(count, label, ARCHIVE_BYTES / chunk, start);
        start = now_ms();
        for (unsigned off = 0; off < ARCHIVE_BYTES; off += chunk)
            CHECK(!ua_user_read(&u, buffer, ARCHIVE_BASE + off, chunk));
        snprintf(label, sizeof(label), "ua_user_read-48MiB-%u", chunk);
        result(count, label, ARCHIVE_BYTES / chunk, start);
        for (unsigned i = 0; i < chunk; i++) CHECK(buffer[i] == 0x5a);
        start = now_ms();
        for (unsigned off = 0; off < ARCHIVE_BYTES; off += chunk)
            CHECK(!copy_to_user(ARCHIVE_BASE + off, buffer, chunk));
        snprintf(label, sizeof(label), "copy_to_user-48MiB-%u", chunk);
        result(count, label, ARCHIVE_BYTES / chunk, start);
        start = now_ms();
        for (unsigned off = 0; off < ARCHIVE_BYTES; off += chunk)
            CHECK(!copy_from_user(buffer, ARCHIVE_BASE + off, chunk));
        snprintf(label, sizeof(label), "copy_from_user-48MiB-%u", chunk);
        result(count, label, ARCHIVE_BYTES / chunk, start);
    }
    ua_destroy(&u); threads[0] = 0;
    CHECK(!shared_refs); pmm_free(shared_frame); shared_frame = 0;
    CHECK(!pages_used && !heap_blocks && !u.head && !u.pins);
}
int main(void)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING;
    replay(500); replay(1000); replay(2000);
    free(ram);
    puts("uaddr benchmark: full cleanup PASS");
    return 0;
}
