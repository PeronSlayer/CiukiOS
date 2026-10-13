/* SPDX-License-Identifier: MIT
 * Production compositor, pinned assets, four overlapping opaque clients.
 * Only painting is timed: wallpaper/icon construction and presentation are not.
 * usage: benchmark portrait.xrgb tango.cis theme out.xrgb [frames-per-sample]
 */
#define _GNU_SOURCE
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <cpuid.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { W = 1024, H = 768, SAMPLES = 9 };
static const struct desk_box screen = {0, 0, W, H};

static const uint32_t *icon(const char *name, unsigned size)
{ return du_icon_argb(du_icon_name(name), size); }

static uint64_t ticks(void)
{
    unsigned a, b, c, d, lo, hi;
    /* CPUID serializes on P6 as well as modern x86; the second CPUID keeps
     * following work behind the read. Compiler barriers cover memory too. */
    __asm__ volatile("" ::: "memory");
    __cpuid(0, a, b, c, d);
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    __cpuid(0, a, b, c, d);
    __asm__ volatile("" ::: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t ns(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000ull + (unsigned)t.tv_nsec;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void setup(struct desktop *d, const char *portrait, const char *store, const char *theme)
{
    fake_reset(); desk_init(d, &fake_ops, W, H);
    uint32_t *p = malloc(DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE * 4);
    assert(p);
    FILE *f = fopen(portrait, "rb");
    assert(f && fread(p, 4, DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE, f) ==
           DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE && !fclose(f));
    d->portrait = p; d->icon = icon;
    assert(!du_icon_paths(store, portrait));
    d->pixels = malloc((W * H + 2) * 4); assert(d->pixels);
    d->pixels[0] = d->pixels[W * H + 1] = 0xfeedface; d->pixels++;
    desk_shell_enable(d);
    snprintf(d->sh.cfg.theme, sizeof(d->sh.cfg.theme), "%s", theme);
    strcpy(d->sh.cfg.wallpaper, "ciuki");
    desk_shell_apply(d); assert(!desk_shell_wallpaper_update(d));
    d->now_ns = 12345000000000ull; d->realtime_second = 1791894896;
    d->sh.cfg.clock_seconds = 1; d->sh.mixer_state = 2; d->sh.volume = 70;
    static const char *const titles[] = {"Files", "CiukNote", "Task Manager", "Ciuki Demo"};
    static const int positions[][4] = {{180, 72, 460, 320}, {440, 168, 420, 280},
                                      {240, 388, 400, 250}, {704, 412, 360, 290}};
    for (int i = 0; i < 4; i++) {
        struct desk_client *c = &d->clients[i];
        unsigned w = positions[i][2], h = positions[i][3];
        uint32_t *client = malloc((size_t)w * h * 4); assert(client);
        for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++)
            /* Nonzero X byte must still be discarded (XRGB, not alpha). */
            client[y * w + x] = 0xa5000000u | ((x * 3 + i * 40) % 256 << 16) |
                               ((y * 2 + i * 30) % 256 << 8) | ((x + y) % 256);
        c->pixels = client; c->info.width = w; c->info.height = h;
        c->window = 1; c->window_id = i + 1; c->order = i + 1; c->resizable = 1;
        c->frame = desk_frame(w, h, positions[i][0], positions[i][1]);
        c->program = i == 0 ? DESK_APP_FILES : i == 1 ? DESK_APP_NOTE : 0;
        c->taskman = i == 2;
        strcpy(c->title, titles[i]);
    }
    d->focus = 3;
    desk_menu_open(d, SH_MENU_START, DESK_START_X, DESK_BAR);
    d->sh.menus[0].hot = 2;
    d->mouse_x = 790; d->mouse_y = 532;
    desk_composite(d, screen); /* warm the stable wallpaper/icon cache */
}

static void exact_clips(struct desktop *d)
{
    uint32_t *pixels = d->pixels, *tiled = calloc(W * H, 4); assert(tiled);
    d->pixels = tiled;
    for (int y = 0; y < H; y += 73) for (int x = 0; x < W; x += 91)
        desk_composite(d, (struct desk_box){x, y, 91, 73});
    assert(!memcmp(pixels, tiled, W * H * 4));
    /* One-pixel clips through captions, icons, shadows, client edges,
     * cursor and the screen edge; surrounding output must stay intact. */
    static const int points[][2] = {{4, 4}, {12, 100}, {187, 72}, {438, 175},
                                   {182, 400}, {790, 532}, {800, 548}, {1023, 767}};
    for (unsigned k = 0; k < sizeof(points) / sizeof(points[0]); k++) {
        memcpy(tiled, pixels, W * H * 4);
        int x = points[k][0], y = points[k][1]; tiled[y * W + x] ^= 0xffffffu;
        desk_composite(d, (struct desk_box){x, y, 1, 1});
        assert(!memcmp(pixels, tiled, W * H * 4));
    }
    d->pixels = pixels; free(tiled);
    assert(pixels[-1] == 0xfeedface && pixels[W * H] == 0xfeedface);
}

static void bounded_damage(struct desktop *d)
{
    desk_menu_close(d); d->damage.count = 0;
    uint64_t previous = d->realtime_second++;
    desk_shell_clock_damage(d, previous);
    assert(d->damage.count == 1);
    struct desk_box b = d->damage.boxes[0];
    assert(b.x > W - SH_TRAY_W && b.y >= H - DESK_BAR && b.w * b.h < 4096);
    d->sh.cfg.clock_seconds = 0; d->damage.count = 0;
    desk_shell_clock_damage(d, previous); assert(!d->damage.count);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_MOTION,
        .value = DESK_START_X + 10 - d->mouse_x, .value2 = DESK_START_Y + 10 - d->mouse_y};
    desk_input(d, &e); assert(d->damage.count == 1);
    b = d->damage.boxes[0];
    assert(b.x == DESK_START_X && b.y == DESK_START_Y && b.w == DESK_START_W && b.h == DESK_START_H);
    d->damage.count = 0;
    e.value = 120 - d->mouse_x; e.value2 = 16 - d->mouse_y;
    desk_input(d, &e); assert(d->damage.count == 1);
    assert(d->damage.boxes[0].w * d->damage.boxes[0].h == DESK_START_W * DESK_START_H);
    d->damage.count = 0;
    e.value = W - 20 - d->mouse_x; e.value2 = H - 16 - d->mouse_y;
    desk_input(d, &e); assert(!d->damage.count);
}

int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    unsigned frames = argc == 6 ? (unsigned)strtoul(argv[5], NULL, 10) : 0;
    assert(frames <= 10000);
    struct desktop *d = calloc(1, sizeof(*d)); assert(d);
    setup(d, argv[1], argv[2], argv[3]);
    FILE *f = fopen(argv[4], "wb");
    assert(f && fwrite(d->pixels, 4, W * H, f) == W * H && !fclose(f));
    if (frames) {
        cpu_set_t cpus, selected; assert(!sched_getaffinity(0, sizeof(cpus), &cpus));
        int cpu = 0; while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &cpus)) cpu++;
        assert(cpu < CPU_SETSIZE); CPU_ZERO(&selected); CPU_SET(cpu, &selected);
        assert(!sched_setaffinity(0, sizeof(selected), &selected));
        uint64_t cycles[SAMPLES], times[SAMPLES];
        for (int i = 0; i < SAMPLES; i++) {
            uint64_t start_ns = ns(), start = ticks();
            for (unsigned n = 0; n < frames; n++) desk_composite(d, screen);
            cycles[i] = ticks() - start; times[i] = ns() - start_ns;
        }
        qsort(cycles, SAMPLES, sizeof(*cycles), compare); qsort(times, SAMPLES, sizeof(*times), compare);
        uint64_t pixels = (uint64_t)frames * W * H, cpp1000 = cycles[SAMPLES / 2] * 1000 / pixels;
        printf("COMPOSITOR theme=%s size=%dx%d windows=4 start=1 cursor=1 cpu=%d samples=%d frames=%u "
               "cycles_per_pixel=%llu.%03llu min_cpp=%llu.%03llu ns_per_frame=%llu\n",
               argv[3], W, H, cpu, SAMPLES, frames,
               (unsigned long long)(cpp1000 / 1000), (unsigned long long)(cpp1000 % 1000),
               (unsigned long long)(cycles[0] * 1000 / pixels / 1000),
               (unsigned long long)(cycles[0] * 1000 / pixels % 1000),
               (unsigned long long)(times[SAMPLES / 2] / frames));
    }
    exact_clips(d); bounded_damage(d);
    desk_render_release(d);
    for (int i = 0; i < 4; i++) free((void *)d->clients[i].pixels);
    free((void *)d->portrait); free(d->pixels - 1);
    free(d->sh.wall); free(d->sh.background); free(d->sh.icons); free(d);
    puts("PASS compositor scene: tiled/one-pixel clips, XRGB clients, guards, clock and hover damage");
    return 0;
}
