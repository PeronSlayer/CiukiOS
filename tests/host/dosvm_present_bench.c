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
/* Native stores use the actual production PTE, not knowledge of its target.
 * The host substitutes the CPU's D-bit update, as in dosvm_workloads_test. */
static void bench_store(struct dosvm *v, unsigned offset, const uint8_t *bytes, unsigned count)
{
    while (count) {
        unsigned n = PAGE_SIZE - (offset & (PAGE_SIZE - 1));
        if (n > count) n = count;
        uint32_t address = 0xA0000 + offset, pte = *window_pte(v, address);
        CHECK((pte & (PTE_U | PTE_W)) == (PTE_U | PTE_W));
        memcpy(window_memory(v, offset), bytes, n);
        *window_pte(v, address) |= DOSVM_PTE_DIRTY;
        bytes += n; offset += n; count -= n;
    }
}
static void bench_planar(struct dosvm *v)
{
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604);
    port_out(v, 0x3D4, 2, 0x0014);
    port_out(v, 0x3D4, 2, 0xE317);
    port_out(v, 0x3C4, 2, 0x0102);
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
                uint8_t bytes[16000];
                for (unsigned at = first / 4; at < end / 4; at++) bytes[at - first / 4] = (uint8_t)(at + f + p);
                bench_store(v, first / 4, bytes, (end - first) / 4);
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
static void bench_replay(const char *name, unsigned wolf_width)
{
    struct dosvm *v = create();
    CHECK(!dosvm_boot(v)); v->info.dos_ready = true;
    bench_planar(v);
    struct ciuki_rect damage;
    CHECK(dosvm_vga_present(v, bench_row, frame_buffer, 0, &damage) == 1);
    struct v86_frame f = {0};
    f.tf.user_ss = 0x9000; f.tf.user_esp = 0xFFF0; f.tf.eflags = V86_VM | V86_IF | 2;
    v->monitor.vif = false;
    uint8_t bytes[16000];
    static uint8_t ref[4][16384];
    const unsigned frames = 120;
    uint64_t present = 0, replay = 0, harvest = v->perf.harvest_cycles;
    uint64_t remap = v->perf.remap_cycles, ports = v->perf.port_cycles[DOSVM_VGA_SR_MASK];
    unsigned port_writes = v->perf.port_count[DOSVM_VGA_SR_MASK], swaps = v->perf.port_remaps;
    unsigned maps = v->video_maps, faults = v->video_faults, harvests = v->perf.harvests,
             pages = v->perf.direct_pages;
    bench_rows = 0;
    for (unsigned frame = 0; frame < frames; frame++) {
        uint64_t start = bench_ns();
        unsigned base = wolf_width ? 0 : (frame % 3) * 0x4000;
        if (wolf_width) {
            uint8_t texture[64];
            for (unsigned i = 0; i < sizeof(texture); i++) texture[i] = (uint8_t)(i * 3 + 7 + frame);
            CHECK(!dosvm_write(v, WOLF_TEXTURE, texture, sizeof(texture)));
            memset(ref, 0, sizeof(ref));
            wolf_frame(v, &f, wolf_width == 4 ? 300 : wolf_width == 2 ? 100 : 50,
                       wolf_width, 0, ref);
        } else {
            /* planar.asm: one map-mask OUT per column, then one per plane
             * per floor span. Replay the measured 1279 OUTs/frame rather
             * than the old four plane-copy passes. */
            for (unsigned x = 0; x < 320; x++) {
                unsigned plane = x & 3;
                port_out(v, 0x3C5, 1, 1u << plane);
                for (unsigned y = 0; y < 200; y++) {
                    unsigned at = y * 80 + x / 4;
                    uint8_t byte = (uint8_t)(at + frame + plane);
                    bench_store(v, base + at, &byte, 1);
                }
            }
            for (unsigned span = 0; span < 959; span++) {
                unsigned plane = span & 3, at = (span / 4 % 200) * 80;
                port_out(v, 0x3C5, 1, 1u << plane);
                for (unsigned x = 0; x < 80; x++) bytes[x] = (uint8_t)(at + x + frame + plane);
                bench_store(v, base + at, bytes, 80);
            }
            port_out(v, 0x3D4, 2, ((base & 255u) << 8) | 0x0D);
            port_out(v, 0x3D4, 2, ((base >> 8) << 8) | 0x0C);
        }
        replay += bench_ns() - start;
        start = bench_ns();
        CHECK(dosvm_vga_present(v, bench_row, frame_buffer, (frame + 1) * 40, &damage) == 1);
        present += bench_ns() - start;
        /* Independent expected bytes, outside all timed regions. */
        for (unsigned plane = 0; plane < 4; plane++) {
            if (wolf_width) {
                for (unsigned at = 0; at < sizeof(ref[plane]); at++) {
                    uint8_t want = ref[plane][at] ? (uint8_t)(ref[plane][at] + frame) : 0;
                    CHECK(v->vga.plane[plane][at] == want);
                }
            } else for (unsigned at = 0; at < sizeof(bytes); at++)
                CHECK(v->vga.plane[plane][base + at] == (uint8_t)(at + frame + plane));
        }
    }
    printf("replay=%s frames=%u planes_per_frame=%.2f faults_per_frame=%.2f harvests_per_frame=%.2f "
           "dirty_pages_per_frame=%.2f harvest_us_per_frame=%.3f present_us_per_frame=%.3f replay_us_per_frame=%.3f\n",
           name, frames, (double)(v->video_maps - maps) / frames, (double)(v->video_faults - faults) / frames,
           (double)(v->perf.harvests - harvests) / frames, (double)(v->perf.direct_pages - pages) / frames,
           (double)(v->perf.harvest_cycles - harvest) / frames / 1000,
           (double)present / frames / 1000, (double)replay / frames / 1000);
    unsigned writes = v->perf.port_count[DOSVM_VGA_SR_MASK] - port_writes;
    unsigned remaps = v->perf.port_remaps - swaps;
    printf("ports=%s map_mask_writes_per_frame=%.2f handler_ns_per_write=%.3f remap_ns_per_swap=%.3f projected_ports_percent_at_35fps=%.3f\n",
           name, (double)writes / frames, writes ? (double)(v->perf.port_cycles[DOSVM_VGA_SR_MASK] - ports) / writes : 0,
           remaps ? (double)(v->perf.remap_cycles - remap) / remaps : 0,
           (double)(v->perf.port_cycles[DOSVM_VGA_SR_MASK] - ports) / frames * 35 / 10000000);
    destroy(v, true); empty();
}
static void bench_latches(void)
{
    struct dosvm *v = create();
    bench_planar(v);
    struct ciuki_rect damage;
    CHECK(dosvm_vga_present(v, bench_row, frame_buffer, 0, &damage) == 1);
    const uint8_t rep[] = {0xF3, 0xA4};
    CHECK(!dosvm_write(v, 0x30000, rep, sizeof(rep)));
    const unsigned frames = 120;
    uint64_t replay = 0, present = 0, harvest = v->perf.harvest_cycles;
    unsigned faults = v->video_faults, maps = v->video_maps, harvests = v->perf.harvests,
             pages = v->perf.direct_pages;
    for (unsigned frame = 0; frame < frames; frame++) {
        /* Latch images are resident in VRAM before the blit, as in Wolf3D.
         * Change their data outside the timed transfer to avoid idle frames. */
        for (unsigned p = 0; p < 4; p++) for (unsigned at = 0; at < 256; at++)
            cvga_store_plane(&v->vga, p, 0x8000 + at, (uint8_t)(at + p * 41 + frame));
        uint64_t start = bench_ns();
        port_out(v, 0x3CE, 2, 0x4105);
        port_out(v, 0x3C4, 2, 0x0F02);
        CHECK(!v->video_direct);
        for (unsigned y = 0; y < 32; y++) {
            struct v86_frame f = vf(0, 0, 8, 0);
            f.tf.cs = 0x3000; f.ds = f.es = 0xA000;
            f.tf.esi = 0x8000 + y * 8; f.tf.edi = y * 80;
            CHECK(!dosvm_vga_fault(v, &f) && f.tf.eip == 2 && !f.tf.ecx);
        }
        port_out(v, 0x3CE, 2, 0x4005);
        port_out(v, 0x3D4, 2, 0x000C);
        replay += bench_ns() - start;
        start = bench_ns();
        CHECK(dosvm_vga_present(v, bench_row, frame_buffer, (frame + 1) * 40, &damage) == 1);
        present += bench_ns() - start;
        for (unsigned p = 0; p < 4; p++) {
            for (unsigned y = 0; y < 32; y++)
                CHECK(!memcmp(&v->vga.plane[p][y * 80], &v->vga.plane[p][0x8000 + y * 8], 8));
            CHECK(v->vga.latch[p] == (uint8_t)(255 + p * 41 + frame));
        }
    }
    printf("replay=wolf-latch-32x32 frames=%u planes_per_frame=%.2f faults_per_frame=%.2f "
           "harvests_per_frame=%.2f dirty_pages_per_frame=%.2f harvest_us_per_frame=%.3f "
           "present_us_per_frame=%.3f replay_us_per_frame=%.3f\n", frames,
           (double)(v->video_maps - maps) / frames, (double)(v->video_faults - faults) / frames,
           (double)(v->perf.harvests - harvests) / frames, (double)(v->perf.direct_pages - pages) / frames,
           (double)(v->perf.harvest_cycles - harvest) / frames / 1000,
           (double)present / frames / 1000, (double)replay / frames / 1000);
    destroy(v, true); empty();
}
static void bench_planar_rows(void)
{
    struct dosvm *v=create(); bench_planar(v);
    cvga_geometry g; CHECK(cvga_get_geometry(&v->vga,&g));
    unsigned base[480],pan[480]; uint32_t lut[256],checksum=0;
    for (unsigned i=0;i<256;i++) lut[i]=i*0x010101u;
    for (unsigned p=0;p<4;p++) for (unsigned i=0;i<CVGA_PLANE_SIZE;i++)
        v->vga.plane[p][i]=(uint8_t)(i*7+p*31);
    for (unsigned y=0;y<g.height;y++) CHECK(cvga_modex_row(&v->vga,y,&base[y],&pan[y]));
    const unsigned frames=1000;
    uint64_t start=bench_ns();
    for (unsigned f=0;f<frames;f++) for (unsigned y=0;y<g.height;y++) {
        cvp_modex_row32(&v->vga,base[y],pan[y],g.width,2,lut,v->video_pixels);
        checksum+=v->video_pixels[(f+y)%(g.width*2)];
    }
    uint64_t convert=bench_ns()-start;
    memset(v->video_taken,0,sizeof(v->video_taken));
    v->video_taken[2][1023]=0x80; /* nonvisible dirt: query the full row span */
    start=bench_ns();
    for (unsigned f=0;f<frames;f++) for (unsigned y=0;y<g.height;y++)
        checksum+=cvga_modex_dirty(base[y],pan[y],g.width,v->video_taken);
    printf("planar_rows frames=%u source_rows=%u conversion_ns_per_frame=%llu dirty_query_ns_per_frame=%llu checksum=%u\n",
           frames,g.height,(unsigned long long)(convert/frames),
           (unsigned long long)((bench_ns()-start)/frames),checksum);
    destroy(v,true); empty();
}
int main(void)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    if (getenv("CVGA_ROW_BENCH")) { bench_planar_rows(); free(ram); return 0; }
    bench_planar_rows();
    bench_case("chain-full", false, false);
    bench_case("planar-full", true, false);
    bench_case("chain-one-row", false, true);
    bench_case("planar-one-row", true, true);
    /* Host nanoseconds are the verbose clock seam, never target TSC evidence. */
    tsc_clock_hook = bench_ns; g_boot.flags = CBI_F_VERBOSE; g_tsc_per_ms = 1000000;
    bench_replay("doom-mode-y", 0);
    bench_replay("wolf-near-width4", 4);
    bench_replay("wolf-mid-width2", 2);
    bench_replay("wolf-far-width1", 1);
    bench_latches();
    tsc_clock_hook = 0; g_boot.flags = 0; g_tsc_per_ms = 0;
    free(ram);
    return 0;
}
