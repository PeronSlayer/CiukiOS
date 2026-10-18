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

#ifdef DESK_PROFILE
static const char *const painter_names[]={"background_rows","windows","captions","shadows",
    "icons","text","cursor","bars_menus","present_front_copy"};
static uint64_t painter_cycles[9], painter_calls[9], profile_last;
static int profile_stack[32], profile_depth, profiling;
void compositor_profile_enter(const char *name)
{
    if (!profiling) return;
    uint64_t now=ticks();
    if (profile_depth) painter_cycles[profile_stack[profile_depth-1]]+=now-profile_last;
    unsigned k=0;
    while (k<9 && strcmp(name,painter_names[k])) k++;
    assert(k<9 && profile_depth<32);
    profile_stack[profile_depth++]=(int)k; painter_calls[k]++; profile_last=ticks();
}
void compositor_profile_leave(void)
{
    if (!profiling) return;
    uint64_t now=ticks(); assert(profile_depth);
    painter_cycles[profile_stack[--profile_depth]]+=now-profile_last;
    profile_last=ticks();
}
void __real_du_icon_blit(struct du_canvas *, int, int, const uint32_t *, int, enum du_icon_mode);
void __wrap_du_icon_blit(struct du_canvas *c, int x, int y, const uint32_t *p, int size, enum du_icon_mode mode)
{
    DESK_PROFILE("icons", __real_du_icon_blit(c,x,y,p,size,mode));
}
int __real_desk_paint_text(struct desk_paint *, int, int, int, const char *, uint32_t);
int __wrap_desk_paint_text(struct desk_paint *p, int font, int x, int y, const char *s, uint32_t colour)
{
    int result; DESK_PROFILE("text", result=__real_desk_paint_text(p,font,x,y,s,colour)); return result;
}
int __real_desk_paint_text_n(struct desk_paint *, int, int, int, const char *, size_t, uint32_t);
int __wrap_desk_paint_text_n(struct desk_paint *p, int font, int x, int y, const char *s, size_t n, uint32_t colour)
{
    int result; DESK_PROFILE("text", result=__real_desk_paint_text_n(p,font,x,y,s,n,colour)); return result;
}
#endif

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

#ifdef DESK_PROFILE
static int discard_present(int display, int output, const struct ciuki_rect *r)
{ (void)display; (void)output; (void)r; return 0; }
static int front_accel(int display, const struct ciuki_accel_request *q)
{ (void)display; return q->operation==CIUKI_ACCEL_QUERY ? 1 : 0; }
static void profile_front(struct desktop *d, unsigned frames, const char *theme)
{
    d->ops.present=discard_present;
    for (int active=0;active<2;active++) {
        memset(painter_cycles,0,sizeof(painter_cycles)); memset(painter_calls,0,sizeof(painter_calls));
        d->accel=active ? front_accel : NULL; d->accel_caps=active ? 1 : 0;
        profiling=1;
        for (unsigned n=0;n<frames;n++) {
            d->scanout_valid=0; d->damage.count=0;
            desk_damage_add(&d->damage,screen,W,H); assert(!desk_redraw(d));
        }
        profiling=0;
        assert(!profile_depth);
        if (!getenv("CIUKI_BENCH_REFERENCE")) assert(painter_calls[8]==0);
        printf("FRONT_COPY theme=%s accelerator=%d cycles_per_frame=%llu calls_per_frame=%llu\n",theme,active,
            (unsigned long long)(painter_cycles[8]/frames),(unsigned long long)(painter_calls[8]/frames));
    }
    d->accel=NULL; d->accel_caps=0;
    d->scanout_valid=0;
}
#endif

static void setup(struct desktop *d, const char *portrait, const char *store, const char *theme)
{
    fake_reset(); desk_init(d, &fake_ops, W, H);
    uint32_t *p = malloc(DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE * 4);
    assert(p);
    FILE *f = fopen(portrait, "rb");
    assert(f && fread(p, 4, DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE, f) ==
           DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE && !fclose(f));
    d->portrait = p; d->icon = icon;
    static int assets_configured;
    if (!assets_configured) { assert(!du_icon_paths(store,portrait)); assets_configured=1; }
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
    d->order=4;
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
    assert(b.x > desk_shell_tray_box(d).x && b.y >= 0 && b.y+b.h <= DESK_BAR && b.w * b.h < 4096);
    d->sh.cfg.clock_seconds = 0; d->damage.count = 0;
    desk_shell_clock_damage(d, previous); assert(!d->damage.count);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_MOTION,
        .value = DESK_START_X + 10 - d->mouse_x, .value2 = DESK_START_Y + 10 - d->mouse_y};
    desk_input(d, &e); assert(d->damage.count == 1);
    b = d->damage.boxes[0];
    assert(b.x == DESK_START_X && b.y == DESK_START_Y && b.w == DESK_START_W && b.h == DESK_START_H+DESK_START_SHADOW);
    d->damage.count = 0;
    e.value = 120 - d->mouse_x; e.value2 = 16 - d->mouse_y;
    desk_input(d, &e); assert(d->damage.count == 1);
    assert(d->damage.boxes[0].w * d->damage.boxes[0].h == DESK_START_W * (DESK_START_H+DESK_START_SHADOW));
    d->damage.count = 0;
    e.value = W - 20 - d->mouse_x; e.value2 = H - 16 - d->mouse_y;
    desk_input(d, &e); assert(!d->damage.count);
}

static void redraw_exact(struct desktop *d)
{
    assert(!desk_redraw(d));
    uint32_t *output=d->pixels, *expected=calloc(W*H,4); assert(expected);
    d->pixels=expected; desk_composite(d,screen); d->pixels=output;
    if (memcmp(output,expected,W*H*4)) {
        for (unsigned k=0;k<W*H;k++) if (output[k]!=expected[k]) {
            fprintf(stderr,"redraw mismatch theme=%s pixel=%u,%u got=%08x want=%08x menu=%d focus=%d\n",
                desk_theme->name,k%W,k/W,output[k],expected[k],d->menu_open,d->focus); break;
        }
        assert(!"incremental redraw mismatch");
    }
    free(expected);
}

static uint64_t damage_union(struct desktop *d, const struct desk_box *allowed, unsigned n)
{
    uint64_t count=0;
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        unsigned dirty=0, wanted=0;
        for (unsigned k=0;k<d->damage.count;k++) dirty+=(unsigned)desk_contains(d->damage.boxes[k],x,y);
        for (unsigned k=0;k<d->move_damage.count;k++) dirty+=(unsigned)desk_contains(d->move_damage.boxes[k],x,y);
        for (unsigned k=0;k<n;k++) wanted|=(unsigned)desk_contains(allowed[k],x,y);
        assert(dirty<=1 && (!dirty || wanted)); count+=dirty;
    }
    return count;
}

static void drag_and_menu_damage(struct desktop *d)
{
    d->damage.count=0; d->sh.icon_selected=-1; desk_focus(d,3);
    struct desk_client *c=&d->clients[3];
    free((void *)c->pixels); uint32_t *pixels=malloc(400*300*4); assert(pixels);
    for (unsigned k=0;k<400*300;k++) pixels[k]=0xa5123456u+k;
    c->pixels=pixels; c->info.width=400; c->info.height=300;
    c->frame=desk_frame(400,300,510,340);
    desk_damage_add(&d->damage,screen,W,H); redraw_exact(d);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,
        .value=c->frame.x+50-d->mouse_x,.value2=c->frame.y+10-d->mouse_y};
    desk_input(d,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1};
    desk_input(d,&e); assert(d->drag==3); redraw_exact(d);
    struct desk_box regions[7]; regions[0]=desk_outer(c);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=64,.value2=32};
    desk_input(d,&e); regions[1]=desk_outer(c);
    uint64_t area=damage_union(d,regions,2);
    /* Every old/new pixel is covered; clean bounding-box corners stay clean. */
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        unsigned wanted=desk_contains(regions[0],x,y)||desk_contains(regions[1],x,y), got=0;
        for (unsigned k=0;k<d->damage.count;k++) got|=(unsigned)desk_contains(d->damage.boxes[k],x,y);
        for (unsigned k=0;k<d->move_damage.count;k++) got|=(unsigned)desk_contains(d->move_damage.boxes[k],x,y);
        assert(got==wanted);
    }
    redraw_exact(d); assert(d->frame_stats.scene_px<area && area<=W*H/4);
    printf("DAMAGE theme=%s action=drag client=400x300 dx=64 dy=32 scene_px=%llu affected_px=%llu full_px=%d\n",
           desk_theme->name,(unsigned long long)d->frame_stats.scene_px,(unsigned long long)area,W*H);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT}; desk_input(d,&e);
    redraw_exact(d);
    desk_menu_open(d,SH_MENU_START,DESK_START_X,DESK_BAR);
    struct desk_box menu=d->sh.menus[0].box;
    if (desk_theme->shadow_size) menu=(struct desk_box){menu.x-4,menu.y-4,menu.w+10,menu.h+10};
    regions[0]=menu; regions[1]=c->frame; regions[1].h=DESK_TITLE;
    regions[2]=(struct desk_box){c->frame.x,c->frame.y+DESK_TITLE,DESK_BORDER,c->frame.h-DESK_TITLE};
    regions[3]=regions[2]; regions[3].x=c->frame.x+c->frame.w-DESK_BORDER;
    regions[4]=(struct desk_box){c->frame.x,c->frame.y+c->frame.h-DESK_BORDER,c->frame.w,DESK_BORDER};
    regions[5]=desk_task_button(d,3); regions[6]=(struct desk_box){0,0,W,DESK_BAR};
    area=damage_union(d,regions,7); redraw_exact(d);
    assert(d->frame_stats.scene_px==area && area<=W*H/4);
    printf("DAMAGE theme=%s action=start_open scene_px=%llu menu_and_focus_cues=1\n",desk_theme->name,(unsigned long long)area);
    desk_menu_close(d); area=damage_union(d,regions,7); redraw_exact(d);
    assert(d->frame_stats.scene_px==area && area<=W*H/4);
    printf("DAMAGE theme=%s action=start_close scene_px=%llu menu_and_focus_cues=1\n",desk_theme->name,(unsigned long long)area);
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
        /* Settle the host's frequency governor after pinning. This warm-up
         * is outside every timing sample and identical for the reference. */
        uint64_t warm_until=ns()+250000000ull;
        do { desk_composite(d,screen); } while (ns()<warm_until);
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
#ifdef DESK_PROFILE
        uint64_t breakdown[9][SAMPLES];
        for (int sample=0;sample<SAMPLES;sample++) {
            memset(painter_cycles,0,sizeof(painter_cycles)); memset(painter_calls,0,sizeof(painter_calls));
            profiling=1;
            for (unsigned n=0; n<frames; n++) desk_composite(d,screen);
            profiling=0; assert(!profile_depth);
            for (unsigned k=0;k<9;k++) breakdown[k][sample]=painter_cycles[k]/frames;
        }
        for (unsigned k=0;k<9;k++) {
            qsort(breakdown[k],SAMPLES,sizeof(uint64_t),compare);
            printf("PAINTER theme=%s name=%s cycles_per_frame=%llu calls_per_frame=%llu\n",argv[3],painter_names[k],
                   (unsigned long long)breakdown[k][SAMPLES/2],(unsigned long long)(painter_calls[k]/frames));
        }
        profile_front(d,frames,argv[3]);
#endif
    }
    exact_clips(d); bounded_damage(d);
    if (!getenv("CIUKI_BENCH_REFERENCE")) drag_and_menu_damage(d);
    desk_render_release(d);
    for (int i = 0; i < 4; i++) free((void *)d->clients[i].pixels);
    free((void *)d->portrait); free(d->pixels - 1);
    free(d->sh.wall); free(d->sh.background); free(d->sh.icons); free(d);
    puts("PASS compositor scene: tiled/one-pixel clips, XRGB clients, guards, clock and hover damage");
    return 0;
}
