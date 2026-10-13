/* Production DOS window presenter benchmark; paging/CPU/VFS seams are the
 * same as dosvm_test. Synthetic frames only, no game payloads or guest CPU.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main dosvm_tests_main
#include "dosvm_test.c"
#undef main
#include <time.h>

static uint64_t bench_ns(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static unsigned bench_rows;
static int bench_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{
    bench_rows++;
    return capture_row(arg, y, pixels, bytes);
}
static void bench_case(const char *name, bool planar, bool sparse)
{
    struct dosvm *v = create();
    int10(v, 0x13, 0, 0, 0);
    if (planar) {
        port_out(v, 0x3C4, 2, 0x0604);
        port_out(v, 0x3D4, 2, 0x0014);
        port_out(v, 0x3D4, 2, 0xE317);
        port_out(v, 0x3C4, 2, 0x0102);
    }
    struct ciuki_rect damage;
    CHECK(dosvm_vga_present(v, bench_row, frame_buffer, 0, &damage) == 1);
    bench_rows = 0;
    uint64_t elapsed = 0, total = bench_ns();
    const unsigned frames = 300;
    for (unsigned f = 1; f <= frames; f++) {
        dosvm_vga_direct(v);
        unsigned first = sparse ? 320 * 17 : 0, end = sparse ? first + 320 : 64000;
        if (planar) {
            for (unsigned p = 0; p < 4; p++) {
                port_out(v, 0x3C4, 2, (p + 1 == 4 ? 8u : 1u << p) * 256u + 2);
                for (unsigned at = first / 4; at < end / 4; at++)
                    v->vga.plane[p][at] = (uint8_t)(at + f + p);
                for (unsigned pg = first / 4 / PAGE_SIZE; pg <= (end / 4 - 1) / PAGE_SIZE; pg++)
                    *window_pte(v, 0xA0000 + pg * PAGE_SIZE) |= DOSVM_PTE_DIRTY;
            }
            port_out(v, 0x3D4, 2, 0x000C); /* frame completion, including unchanged start */
        } else {
            for (unsigned at = first; at < end; at++) {
                ((uint8_t *)P2V(v->frames[0xA0 + at / PAGE_SIZE]))[at % PAGE_SIZE] = (uint8_t)(at + f);
            }
            for (unsigned pg = first / PAGE_SIZE; pg <= (end - 1) / PAGE_SIZE; pg++)
                *window_pte(v, 0xA0000 + pg * PAGE_SIZE) |= DOSVM_PTE_DIRTY;
        }
        uint64_t start = bench_ns();
        CHECK(dosvm_vga_present(v, bench_row, frame_buffer, f * 40, &damage) == 1);
        elapsed += bench_ns() - start;
    }
    total = bench_ns() - total;
    printf("%s frames=%u present_ns_per_frame=%llu total_ns_per_frame=%llu rows_per_frame=%u\n",
           name, frames, (unsigned long long)(elapsed / frames),
           (unsigned long long)(total / frames), bench_rows / frames);
    destroy(v, true); empty();
}
int main(void)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    bench_case("chain-full", false, false);
    bench_case("planar-full", true, false);
    bench_case("chain-one-row", false, true);
    free(ram);
    return 0;
}
