/* Screen console: VGA text mode or the boot LFB with the bundled
 * Inconsolata bitmap (assets/fonts/native/INCONSOL.CFN, OFL 1.1).
 * Keeps a history of lines so earlier records can be shown again
 * (docs/design/f0-acceptance.md, screen evidence and paging).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/fbdev.h>
#include <ciuki/task.h>
#include <ciuki/cpustat.h>
#include <ciuki/sync.h>
#include <ciuki/splash.h>

extern const uint8_t font_cfn_regular[];   /* 95 widths + 95 * 16 rows (u16 LE) */

#define CELL_W 8
#define CELL_H 16
#define MAX_COLS 160
/* Worst-case core records (including BEGIN/END): boot 7; bootinfo
 * 2 + 2*128 E820/normalized + 16 PMM reservations + 32 PCI + 1 summary
 * + 7 negatives + 2 totals = 316; allocator 4; protection 6; isolation 5;
 * preempt 6; localfault 7; syslife 2 + 12 buffers + 1 summary + 100 cycles
 * + 1 ledger = 116; fpu 5. Total 472 * ceil(240/80) = 1416 lines.
 * 1536 leaves 120 lines for boot/selector logs and margin. The history
 * uses 1536*161 = 247296 bytes; all console static storage is < 256 KiB. */
#define HIST_LINES 1536

enum { CON_NONE, CON_TEXT, CON_LFB };
static int con_kind;
static unsigned cols, rows, cur_col, cur_row;
static uint8_t *fb;
static uint32_t pitch, bpp, width, height;
static uint32_t fg = 0xE8E8E8, bg = 0x10243C;

/* line history for paging */
static char hist[HIST_LINES][MAX_COLS + 1];
static unsigned hist_head, hist_count;
static char linebuf[MAX_COLS + 1];
static unsigned linelen;
static bool draw_enabled;
/* f3-02: while a process holds the display grant (the desktop), every LFB
 * pixel is its own. The console keeps history and the serial copy but draws
 * nothing until the grant is released (proc/grants.c); the next line then
 * repaints the screen from history. Deliberate overlays (the operator
 * prompt, evidence pages) and panic output (console_reclaim) still draw. */
static volatile bool display_owned, repaint_pending;
/* Quiet boot (docs/boot.md): a normal boot leaves the loader's screen alone
 * until the desktop takes the display. Lines still reach the history, the
 * serial port and the boot log file; console_wake (panic, double fault,
 * desktop failure, watchdog) switches drawing on and repaints the history. */
static volatile bool quiet;
static bool panic_drawing, paint_valid;
static uint32_t committed, painted_lines;
static unsigned painted_rows;
static uint64_t next_paint;
static const char *pinned_page;
static struct task *console_task;          /* the console worker (sched_init) */
/* A service turn paints at most eight cells or one copied scanline. The
 * presenter claim is always released before a scheduling point. */
#define PAINT_CELLS 8u
static struct {
    bool active, pinned, row_ready;
    uint8_t *mapping;
    unsigned rows, cols, head, count, row, col, move_y, move_end, dy;
    uint32_t target;
    const char *page, *cursor;
    char tail[MAX_COLS + 1], line[MAX_COLS + 1];
} paint;


static uint32_t line_begin_as(bool overlay)
{
    uint32_t flags = irq_save();
    draw_enabled = con_kind != CON_LFB ||
                   (__atomic_load_n(&fb, __ATOMIC_ACQUIRE) && (overlay || (!display_owned && !quiet)) && fbdev_console_begin());
    irq_restore(flags); /* presenter claim, not IF, protects the pixels */
    return flags;
}
static uint32_t line_begin(void) { return line_begin_as(false); }

static void line_end(uint32_t flags)
{
    if (con_kind == CON_LFB && draw_enabled)
        fbdev_console_end();
    irq_restore(flags);
}

static uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    const struct fb_device *d = fbdev_get();
    const struct ciuki_boot_info *bi = &g_boot;
    unsigned rp = d->present ? d->red_pos : bi->fb_red_pos, gp = d->present ? d->green_pos : bi->fb_green_pos;
    unsigned bp = d->present ? d->blue_pos : bi->fb_blue_pos;
    unsigned rs = d->present ? d->red_size : bi->fb_red_size, gs = d->present ? d->green_size : bi->fb_green_size;
    unsigned bs = d->present ? d->blue_size : bi->fb_blue_size;
    if (bpp == 16 || bpp == 15)
        return ((r >> (8 - rs)) << rp) |
               ((g >> (8 - gs)) << gp) |
               ((b >> (8 - bs)) << bp);
    return ((r >> (8 - rs)) << rp) | ((g >> (8 - gs)) << gp) | ((b >> (8 - bs)) << bp);
}

static void put_px(uint32_t x, uint32_t y, uint32_t c)
{
    uint8_t *p = fb + y * pitch + x * (bpp / 8);
    switch (bpp) {
    case 32: *(volatile uint32_t *)p = c; break;
    case 24: p[0] = c; p[1] = c >> 8; p[2] = c >> 16; break;
    case 16: case 15: *(volatile uint16_t *)p = (uint16_t)c; break;
    }
}

static void draw_cell(unsigned col, unsigned row, char ch)
{
    if (con_kind == CON_TEXT) {
        volatile uint16_t *vga = (volatile uint16_t *)P2V(0xB8000);
        vga[row * cols + col] = (uint16_t)(uint8_t)ch | 0x1F00;
        return;
    }
    if (!draw_enabled)
        return;
    uint32_t cf = pack(fg), cb = pack(bg);
    const uint8_t *rowsp = font_cfn_regular + 95;
    unsigned idx = (ch >= 32 && ch <= 126) ? (unsigned)(ch - 32) : ('?' - 32);
    for (unsigned y = 0; y < CELL_H; y++) {
        const uint8_t *r = rowsp + (idx * CELL_H + y) * 2;
        uint16_t bits = (uint16_t)(r[0] | (r[1] << 8));
        for (unsigned x = 0; x < CELL_W; x++)
            put_px(col * CELL_W + x, row * CELL_H + y, (bits & (0x8000u >> x)) ? cf : cb);
    }
}

#ifdef CIUKI_SPLASH_BIN
/* Quiet-boot splash: the original Ciuki photograph, 512x384 RGB565 from
 * scripts/convert_splash.py, scaled (nearest neighbour) to the largest 4:3
 * area that fits and centred on black. Drawn once, before any line.
 * f0-55: under the photograph's wordmark, the tagline in CiukiOS Sans and a
 * thin rounded progress bar in colours taken from the photograph (cream
 * track, Ciuki's fur for the fill). The bar is indeterminate until the
 * first kernel phase (ciuki/splash.h), then determinate; it is drawn only
 * while the boot is quiet and nobody holds the display grant, and the
 * grant (console_display_owned) completes it to 100 % before the desktop
 * owns the pixels. The splash is never cleared: the desktop's first frame
 * replaces it. Each update claims the console region for at most
 * SPLASH_CLAIM_PIXELS pixels computed from the embedded photograph: no LFB
 * reads, no allocation, no disk read.
 * f0-55b: the first frame already shows state. kmain paints it before the
 * PIT runs (tick 0) with the sweep segment centred, or with the level of
 * any phase a quiet boot reported before the paint. The console worker
 * (task "console", sched_init) animates the sweep: task_sleep_ms wakes it
 * every SPLASH_TICK_MS from its kernel timer (PIT IRQ 0), never from klog,
 * and while the splash is on screen its base level is SPLASH_WORKER_LEVEL,
 * above the boot task that runs init (probes_main, P_INTERACTIVE), so the
 * wake preempts init at init's next blocking wait or kpreempt_point. Each
 * drawn phase level is recorded once on the serial log. */
#define SPLASH_W 512u
#define SPLASH_H 384u
__asm__(".pushsection .rodata.splash,\"a\"\n.balign 4\n"
        ".global console_splash, console_splash_end\nconsole_splash:\n"
        ".incbin \"" CIUKI_SPLASH_BIN "\"\nconsole_splash_end:\n.popsection\n");
extern const uint8_t console_splash[], console_splash_end[];
extern const uint8_t font_cfn_sans[];      /* CiukiOS Sans: 95 widths + 95 * 16 rows (u16 LE) */
extern uint64_t g_tsc_per_ms;

#define SPLASH_TRACK        0xFFF4E4u     /* cream of the wordmark, 25 % over the photo */
#define SPLASH_TRACK_ALPHA  64u
#define SPLASH_FILL_FROM    0xD99A41u     /* Ciuki's fur, hue 35 degrees */
#define SPLASH_FILL_TO      0xF7D099u
#define SPLASH_TEXT         0xFFF4E4u
#define SPLASH_TEXT_ALPHA   236u
#define SPLASH_SHADOW_ALPHA 120u
#define SPLASH_CLAIM_PIXELS 1024u         /* per console claim: well under 1 ms */
#define SPLASH_TICK_MS      40u           /* indeterminate animation step */
/* Above the boot task (P_INTERACTIVE, SCHED_FOREGROUND) and its I/O
 * completion boost (base + 1, task_io_completed). */
#define SPLASH_WORKER_LEVEL (SCHED_FOREGROUND + 2u)
static const char splash_tagline[] = "Classic computing, one woof at a time.";

struct splash_layout {
    uint32_t x0, y0, dw, dh;              /* the photograph's 4:3 area */
    uint32_t bx, by, bw, bh;              /* progress bar */
    uint32_t tx, ty, tw;                  /* tagline, 16 rows */
    uint32_t ay;                          /* f1-97 accelerator line, 16 rows */
    bool bar, accel;
};
#define SPLASH_ACCEL_TEXT 72u
static struct {
    struct splash_layout g;
    bool active, busy, determinate, shown_valid, shown_determinate;
    bool ended;                           /* handover or stop: phases ignored */
    uint32_t level, shown_level;
    int32_t segment, shown_segment;
    uint32_t phases, updates, claims, deferred, max_claim_pixels;
    uint32_t recorded, records;           /* serial phase records (f0-55b) */
    uint64_t max_claim_cycles;
    /* f1-97: `2D acceleration: YES|NO (<reason>)` under the bar. The target
     * text is set by the accelerator decision; the console worker draws it. */
    char accel[SPLASH_ACCEL_TEXT], accel_shown[SPLASH_ACCEL_TEXT];
    bool accel_busy;
} splash_bar;
static bool splash_worker_raised;

static uint32_t splash_rgb565(const uint8_t *s)
{
    uint32_t v = s[0] | (uint32_t)s[1] << 8;
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static uint32_t splash_photo(const struct splash_layout *g, uint32_t x, uint32_t y)
{
    if (x < g->x0 || x >= g->x0 + g->dw || y < g->y0 || y >= g->y0 + g->dh) return 0;
    return splash_rgb565(console_splash + ((y - g->y0) * SPLASH_H / g->dh * SPLASH_W +
                                           (x - g->x0) * SPLASH_W / g->dw) * 2);
}

static uint32_t splash_blend(uint32_t under, uint32_t over, uint32_t alpha)   /* alpha 0..256 */
{
    uint32_t out = 0;
    for (unsigned s = 0; s < 24; s += 8)
        out |= (((under >> s & 255) * (256 - alpha) + (over >> s & 255) * alpha) >> 8) << s;
    return out;
}

static unsigned splash_glyph(char c) { return c >= 32 && c < 127 ? (unsigned)(c - 32) : (unsigned)('?' - 32); }
static uint32_t splash_string_width(const char *text)
{
    uint32_t w = 0;
    for (const char *s = text; *s; s++) w += font_cfn_sans[splash_glyph(*s)];
    return w;
}
static uint32_t splash_text_width(void) { return splash_string_width(splash_tagline); }

/* Reference layout 1024x768: wordmark ink ends at row 681; tagline at 690,
 * bar centred on row 728, 3/8 of the width, 6 rows. Scaled to the area. */
static void splash_layout(uint32_t w, uint32_t h, struct splash_layout *g)
{
    memset(g, 0, sizeof(*g));
    g->dw = w; g->dh = w * 3 / 4;
    if (g->dh > h) { g->dh = h; g->dw = h * 4 / 3; }
    g->x0 = (w - g->dw) / 2; g->y0 = (h - g->dh) / 2;
    if (g->dw < 320 || g->dh < 240) return;
    g->bh = (g->dh * 6 + 384) / 768;
    if (g->bh < 4) g->bh = 4;
    g->bw = g->dw * 3 / 8 & ~1u;
    g->bx = g->x0 + (g->dw - g->bw) / 2;
    g->by = g->y0 + g->dh * 728 / 768 - g->bh / 2;
    g->tw = splash_text_width();
    g->tx = g->x0 + (g->dw - g->tw) / 2;
    g->ty = g->y0 + g->dh * 690 / 768;
    g->bar = g->tw < g->dw && g->ty + 17 <= g->by && g->by + g->bh <= g->y0 + g->dh;
    /* f1-97: the accelerator line sits under the bar, inside the photograph. */
    g->ay = g->by + g->bh + (g->dh * 8 + 384) / 768;
    g->accel = g->bar && g->ay + 17 <= g->y0 + g->dh;
}

static uint32_t splash_fill_width(const struct splash_layout *g, uint32_t level)
{
    if (!level) return 0;
    uint32_t w = g->bw * (level > SPLASH_PERMILLE_FULL ? SPLASH_PERMILLE_FULL : level) / SPLASH_PERMILLE_FULL;
    return w < g->bh ? g->bh : w;
}
static uint32_t splash_segment_width(const struct splash_layout *g) { return g->bw / 4; }
/* Indeterminate: a quarter-length segment sweeps the track left to right,
 * one stride per SPLASH_TICK_MS. Tick 0 (the first frame, painted before
 * the PIT runs) shows it centred; every position keeps at least one stride
 * inside the track, so no frame of the sweep shows an empty track. */
static int32_t splash_segment_at(const struct splash_layout *g, uint64_t ticks)
{
    uint32_t segment = splash_segment_width(g), period = g->bw + segment;
    uint32_t stride = period / 40 ? period / 40 : 1, steps = (period - stride) / stride;
    uint32_t centred = ((g->bw - segment) / 2 + segment - stride) / stride;
    uint32_t step = (uint32_t)((ticks / SPLASH_TICK_MS + centred) % steps);
    return (int32_t)(step * stride) - (int32_t)segment + (int32_t)stride;
}

/* Capsule coverage in 4x4 samples (0..16), coordinates in 1/8 pixel. The
 * capsule spans [l8, r8) over the bar rows; a clipped capsule (the moving
 * segment) also has to be inside the track. */
static bool splash_in_capsule(int32_t l8, int32_t r8, int32_t cy8, int32_t rad8, int32_t px, int32_t py)
{
    int32_t a = l8 + rad8, b = r8 - rad8;
    if (b < a) a = b = (l8 + r8) / 2;
    int32_t dx = px < a ? a - px : px > b ? px - b : 0, dy = py - cy8;
    return dx * dx + dy * dy <= rad8 * rad8;
}
static unsigned splash_cover(const struct splash_layout *g, int32_t l8, int32_t r8, int32_t x, int32_t y)
{
    int32_t tl8 = (int32_t)g->bx * 8, tr8 = (int32_t)(g->bx + g->bw) * 8;
    int32_t rad8 = (int32_t)g->bh * 4, cy8 = (int32_t)g->by * 8 + rad8, x8 = x * 8, y8 = y * 8;
    bool clip = l8 < tl8 || r8 > tr8;
    if (r8 <= l8 || x8 + 8 <= l8 || x8 >= r8 || x8 + 8 <= tl8 || x8 >= tr8 ||
        y8 + 8 <= cy8 - rad8 || y8 >= cy8 + rad8) return 0;
    if (!clip && x8 >= l8 + rad8 && x8 + 8 <= r8 - rad8) return 16;
    unsigned n = 0;
    for (int32_t sy = 1; sy < 8; sy += 2)
        for (int32_t sx = 1; sx < 8; sx += 2)
            n += splash_in_capsule(l8, r8, cy8, rad8, x8 + sx, y8 + sy) &&
                 (!clip || splash_in_capsule(tl8, tr8, cy8, rad8, x8 + sx, y8 + sy));
    return n;
}

static uint32_t splash_bar_rgb(const struct splash_layout *g, uint32_t x, uint32_t y,
                               bool determinate, uint32_t level, int32_t segment)
{
    uint32_t c = splash_photo(g, x, y);
    int32_t l8 = (int32_t)g->bx * 8;
    unsigned track = splash_cover(g, l8, (int32_t)(g->bx + g->bw) * 8, (int32_t)x, (int32_t)y), fill;
    if (!track) return c;
    c = splash_blend(c, SPLASH_TRACK, SPLASH_TRACK_ALPHA * track / 16);
    if (determinate) fill = splash_cover(g, l8, l8 + (int32_t)splash_fill_width(g, level) * 8, (int32_t)x, (int32_t)y);
    else fill = splash_cover(g, l8 + segment * 8, l8 + (segment + (int32_t)splash_segment_width(g)) * 8,
                             (int32_t)x, (int32_t)y);
    if (!fill) return c;
    uint32_t t = (x - g->bx) * 256 / (g->bw - 1);
    return splash_blend(c, splash_blend(SPLASH_FILL_FROM, SPLASH_FILL_TO, t), fill * 16);
}

/* Columns [c0, c1) of the bar, relative to bx. */
static uint32_t splash_bar_pixels(uint32_t c0, uint32_t c1, bool determinate, uint32_t level, int32_t segment)
{
    const struct splash_layout *g = &splash_bar.g;
    for (uint32_t y = g->by; y < g->by + g->bh; y++)
        for (uint32_t x = g->bx + c0; x < g->bx + c1; x++)
            put_px(x, y, pack(splash_bar_rgb(g, x, y, determinate, level, segment)));
    return (c1 - c0) * g->bh;
}

static bool splash_text_bit(const char *text, uint32_t tx, uint32_t ty, uint32_t x, uint32_t y)
{
    if (y < ty || y >= ty + 16 || x < tx) return false;
    uint32_t cx = tx;
    for (const char *s = text; *s; s++) {
        unsigned i = splash_glyph(*s), w = font_cfn_sans[i];
        if (x < cx + w) {
            const uint8_t *r = font_cfn_sans + 95 + (i * 16 + (y - ty)) * 2;
            return ((uint32_t)(r[0] | r[1] << 8) & (0x8000u >> (x - cx))) != 0;
        }
        cx += w;
    }
    return false;
}

/* Pixel (x, y) of a text line over the photograph, with the tagline's ink
 * and shadow rules; `ink` false and no shadow leaves the plain photograph. */
static uint32_t splash_text_rgb(const struct splash_layout *g, const char *text, uint32_t tx, uint32_t ty,
                                uint32_t x, uint32_t y, bool *touched)
{
    bool ink = splash_text_bit(text, tx, ty, x, y), shadow = splash_text_bit(text, tx, ty, x - 1, y - 1);
    uint32_t c = splash_photo(g, x, y);
    *touched = ink || shadow;
    if (shadow) c = splash_blend(c, 0, SPLASH_SHADOW_ALPHA);
    if (ink) c = splash_blend(c, SPLASH_TEXT, SPLASH_TEXT_ALPHA);
    return c;
}

static void splash_draw_tagline(const struct splash_layout *g)
{
    for (uint32_t y = g->ty; y < g->ty + 17; y++)
        for (uint32_t x = g->tx; x < g->tx + g->tw + 1; x++) {
            bool touched;
            uint32_t c = splash_text_rgb(g, splash_tagline, g->tx, g->ty, x, y, &touched);
            if (touched) put_px(x, y, pack(c));
        }
}

/* f1-97: the accelerator line, rows [r0, r1) of its 17, columns [x0, x1):
 * the photograph is restored first so a changed text leaves no old ink. */
static uint32_t splash_accel_pixels(const char *text, uint32_t x0, uint32_t x1, uint32_t r0, uint32_t r1)
{
    const struct splash_layout *g = &splash_bar.g;
    uint32_t tx = g->x0 + (g->dw - splash_string_width(text)) / 2;
    for (uint32_t y = g->ay + r0; y < g->ay + r1; y++)
        for (uint32_t x = x0; x < x1; x++) {
            bool touched;
            put_px(x, y, pack(splash_text_rgb(g, text, tx, g->ay, x, y, &touched)));
        }
    return (x1 - x0) * (r1 - r0);
}

static void splash_record(uint32_t level);
static void draw_splash(void)
{
    /* Paint state only: the progress (phases a quiet boot reported before
     * this paint, ended, the serial records) belongs to the boot. */
    splash_bar.active = splash_bar.busy = splash_bar.shown_valid = splash_bar.shown_determinate = false;
    splash_bar.shown_level = 0;
    splash_bar.segment = splash_bar.shown_segment = 0;
    splash_bar.updates = splash_bar.claims = splash_bar.deferred = splash_bar.max_claim_pixels = 0;
    splash_bar.max_claim_cycles = 0;
    splash_bar.accel_busy = false;
    memset(splash_bar.accel_shown, 0, sizeof(splash_bar.accel_shown));
    if ((uint32_t)(console_splash_end - console_splash) != SPLASH_W * SPLASH_H * 2)
        return;
    struct splash_layout *g = &splash_bar.g;
    splash_layout(width, height, g);
    uint32_t black = pack(0);
    for (uint32_t y = 0; y < height; y++) {
        bool inside = y >= g->y0 && y < g->y0 + g->dh;
        const uint8_t *row = console_splash + (inside ? (y - g->y0) * SPLASH_H / g->dh : 0) * SPLASH_W * 2;
        for (uint32_t x = 0; x < width; x++) {
            if (!inside || x < g->x0 || x >= g->x0 + g->dw) { put_px(x, y, black); continue; }
            put_px(x, y, pack(splash_rgb565(row + (x - g->x0) * SPLASH_W / g->dw * 2)));
        }
    }
    if (!g->bar) return;
    splash_draw_tagline(g);
    if (g->accel && splash_bar.accel[0]) {          /* f1-97: a decision before this paint */
        splash_accel_pixels(splash_bar.accel, g->x0, g->x0 + g->dw, 0, 17);
        memcpy(splash_bar.accel_shown, splash_bar.accel, SPLASH_ACCEL_TEXT);
    }
    if (splash_bar.ended) return;
    /* f0-55b: the first frame shows the accumulated level, or the sweep. */
    bool determinate = splash_bar.determinate;
    uint32_t level = splash_bar.level;
    splash_bar.segment = splash_bar.shown_segment = splash_segment_at(g, g_ticks);
    splash_bar_pixels(0, g->bw, determinate, level, splash_bar.segment);
    splash_bar.shown_determinate = determinate;
    splash_bar.shown_level = level;
    splash_bar.shown_valid = splash_bar.active = true;
    if (determinate) splash_record(level);
}

static bool splash_drawable(void)
{
    return splash_bar.active && quiet && !display_owned && !panic_drawing && fb;
}

static bool splash_claim_open(void)
{
    uint32_t f = irq_save();
    bool claimed = splash_drawable() && fbdev_console_begin();
    irq_restore(f);
    return claimed;
}

/* Ends a claim opened by splash_claim_open and records its budget. */
static void splash_claim_close(uint64_t start, uint32_t pixels)
{
    fbdev_console_end();
    uint64_t cycles = cpu_path_clock ? cpu_path_clock() - start : 0;
    splash_bar.claims++;
    if (pixels > splash_bar.max_claim_pixels) splash_bar.max_claim_pixels = pixels;
    if (cycles > splash_bar.max_claim_cycles) splash_bar.max_claim_cycles = cycles;
    if (cpu_path_add) cpu_path_add(CPU_CONSOLE, cycles);
}

/* One console claim: columns [c0, c1) for the given state. */
static bool splash_chunk(uint32_t c0, uint32_t c1, bool determinate, uint32_t level, int32_t segment)
{
    if (!splash_claim_open()) return false;
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
    splash_claim_close(start, splash_bar_pixels(c0, c1, determinate, level, segment));
    return true;
}

/* f1-97: bring the accelerator line to its target text, in claims of at most
 * SPLASH_CLAIM_PIXELS pixels (whole rows). No LFB read, no disk read. A
 * failed claim leaves the rest to the console worker's next turn. */
static void splash_accel_update(bool yield)
{
    const struct splash_layout *g = &splash_bar.g;
    char want[SPLASH_ACCEL_TEXT], old[SPLASH_ACCEL_TEXT];
    uint32_t f = irq_save();
    bool start = !splash_bar.accel_busy && g->accel && splash_drawable() && splash_bar.accel[0] &&
                 memcmp(splash_bar.accel, splash_bar.accel_shown, SPLASH_ACCEL_TEXT);
    if (start) {
        splash_bar.accel_busy = true;
        memcpy(want, splash_bar.accel, SPLASH_ACCEL_TEXT);
        memcpy(old, splash_bar.accel_shown, SPLASH_ACCEL_TEXT);
    }
    irq_restore(f);
    if (!start) return;
    uint32_t nw = splash_string_width(want), ow = splash_string_width(old);
    uint32_t nx = g->x0 + (g->dw - nw) / 2, ox = g->x0 + (g->dw - ow) / 2;
    uint32_t x0 = old[0] && ox < nx ? ox : nx, x1 = nx + nw + 1;
    if (old[0] && ox + ow + 1 > x1) x1 = ox + ow + 1;
    if (x1 > g->x0 + g->dw) x1 = g->x0 + g->dw;
    uint32_t rows = SPLASH_CLAIM_PIXELS / (x1 - x0);
    if (!rows) rows = 1;
    bool done = true;
    for (uint32_t r = 0; done && r < 17; r += rows) {
        if (!splash_claim_open()) { done = false; break; }
        uint64_t t0 = cpu_path_clock ? cpu_path_clock() : 0;
        splash_claim_close(t0, splash_accel_pixels(want, x0, x1, r, r + rows < 17 ? r + rows : 17));
        if (yield) kpreempt_point();
        if (!splash_drawable()) { done = false; break; }
    }
    if (!done) splash_bar.deferred++;
    f = irq_save();
    if (done) memcpy(splash_bar.accel_shown, want, SPLASH_ACCEL_TEXT);
    splash_bar.accel_busy = false;
    irq_restore(f);
}

/* f1-97/f2-92: the accelerator state (thread context): the text
 * is `2D engine: NO (<reason>)`, `2D engine: OK (drawing: software)` until
 * the engine has drawn a presenter's request, then `2D acceleration: YES`;
 * drawn by the console worker (or at the handover), never from the caller. */
void console_splash_accel(bool accelerated, bool used, const char *reason)
{
    char text[SPLASH_ACCEL_TEXT] = { 0 };
    unsigned n = 0;
    const char *parts[] = { accelerated && used ? "2D acceleration: YES" : "2D engine: ",
                            !accelerated ? "NO (" : used ? "" : "OK (drawing: software)",
                            accelerated || !reason ? "" : reason, accelerated ? "" : ")" };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        for (const char *c = parts[i]; *c && n + 1 < SPLASH_ACCEL_TEXT; c++) text[n++] = *c;
    uint32_t f = irq_save();
    if (!splash_bar.ended) memcpy(splash_bar.accel, text, SPLASH_ACCEL_TEXT);
    irq_restore(f);
}

/* f0-55b: `[console] splash phase=<n> permille=<p>` once per drawn phase
 * level, when the serial log is on (BOOT.CFG serial=1, CBI_F_SERIAL_LOG).
 * Levels only rise and the phase shares are distinct, so a boot writes at
 * most SPLASH_PHASES records; the count is also capped. A level drawn for
 * several phases at once (before the paint, after a deferral) records the
 * last of them. Thread context with IF=1, or kmain's first paint. */
static void splash_record(uint32_t level)
{
    if (!(g_boot.flags & CBI_F_SERIAL_LOG)) return;
    unsigned phase = 0;
    while (phase < SPLASH_PHASES && splash_phase_permille((enum splash_phase)phase) != level) phase++;
    uint32_t f = irq_save();
    bool emit = phase < SPLASH_PHASES && level > splash_bar.recorded && splash_bar.records < SPLASH_PHASES;
    if (emit) { splash_bar.recorded = level; splash_bar.records++; }
    irq_restore(f);
    if (emit) klog("[console] splash phase=%u permille=%u", phase, level);
}

/* Bring the screen to the current target: only the columns that differ
 * from what is shown, in claims of at most SPLASH_CLAIM_PIXELS pixels.
 * A failed claim leaves the update to the console worker. */
static void splash_update(bool yield)
{
    const struct splash_layout *g = &splash_bar.g;
    uint32_t f = irq_save();
    bool start = !splash_bar.busy && splash_drawable();
    if (start) splash_bar.busy = true;
    irq_restore(f);
    if (!start) return;
    uint32_t per = SPLASH_CLAIM_PIXELS / g->bh, drawn = 0;
    for (unsigned pass = 0; pass < 4; pass++) {
        f = irq_save();
        bool det = splash_bar.determinate, valid = splash_bar.shown_valid && splash_bar.shown_determinate == det;
        uint32_t level = splash_bar.level;
        int32_t segment = splash_bar.segment, lo = 0, hi = (int32_t)g->bw;
        bool same = valid && (det ? splash_bar.shown_level == level : splash_bar.shown_segment == segment);
        if (valid && det) {
            int32_t a = (int32_t)splash_fill_width(g, splash_bar.shown_level), b = (int32_t)splash_fill_width(g, level);
            lo = (a < b ? a : b) - (int32_t)g->bh - 1; hi = (a < b ? b : a) + 1;
        } else if (valid) {
            int32_t a = splash_bar.shown_segment, b = segment;
            lo = (a < b ? a : b) - 1; hi = (a < b ? b : a) + (int32_t)splash_segment_width(g) + 1;
        }
        irq_restore(f);
        if (same) break;
        if (lo < 0) lo = 0;
        if (hi > (int32_t)g->bw) hi = (int32_t)g->bw;
        splash_bar.shown_valid = false;
        bool done = true;
        for (uint32_t c = (uint32_t)lo; done && c < (uint32_t)hi; c += per) {
            done = splash_chunk(c, c + per < (uint32_t)hi ? c + per : (uint32_t)hi, det, level, segment);
            if (done && yield) kpreempt_point();
        }
        if (!done) { splash_bar.deferred++; break; }
        if (!splash_drawable()) break;     /* the handover redrew it at a yield */
        splash_bar.shown_valid = true; splash_bar.shown_determinate = det;
        splash_bar.shown_level = level; splash_bar.shown_segment = segment;
        splash_bar.updates++;
        if (det) drawn = level;
    }
    splash_bar.busy = false;
    if (drawn) splash_record(drawn);       /* outside the busy section */
}

/* The display grant: complete the bar, then never draw it again. An update
 * interrupted at its yield is superseded by a full redraw at 100 %. */
static void splash_handover(void)
{
    uint32_t f = irq_save();
    bool draw = splash_bar.active && quiet && !display_owned;
    splash_bar.ended = true;
    if (draw) {
        splash_bar.phases++;
        splash_bar.determinate = true;
        splash_bar.level = SPLASH_PERMILLE_FULL;
        if (splash_bar.busy) { splash_bar.busy = false; splash_bar.shown_valid = false; }
    }
    irq_restore(f);
    if (!draw) return;
    splash_update(false);
    splash_accel_update(false);
    bool complete = splash_bar.shown_valid && splash_bar.shown_level == SPLASH_PERMILLE_FULL;
    splash_bar.active = false;
    uint32_t us = g_tsc_per_ms ? (uint32_t)(splash_bar.max_claim_cycles * 1000u / g_tsc_per_ms) : 0;
    klog("[console] splash handover progress=%u complete=%u phases=%u updates=%u claims=%u deferred=%u max_claim_pixels=%u max_claim_us=%u",
         complete ? SPLASH_PERMILLE_FULL : splash_bar.shown_level, complete, splash_bar.phases, splash_bar.updates,
         splash_bar.claims, splash_bar.deferred, splash_bar.max_claim_pixels, us);
}

void console_splash_phase(enum splash_phase phase)
{
    uint32_t level = splash_phase_permille(phase);
    if (!level) return;
    if (phase == SPLASH_PHASE_HANDOVER) { splash_handover(); return; }
    uint32_t f = irq_save();
    /* f0-55b: a quiet boot keeps a phase reported before the splash is
     * painted; the first paint draws its level. */
    bool kept = quiet && !splash_bar.ended, active = kept && splash_bar.active;
    if (kept) {
        splash_bar.phases++;
        splash_bar.determinate = true;
        if (level > splash_bar.level) splash_bar.level = level;
    }
    irq_restore(f);
    if (active) splash_update(true);
}

/* f0-55b: while the splash is on screen the console worker runs at
 * SPLASH_WORKER_LEVEL, above the boot task, so its timer wake sets
 * g_need_resched against init (task_start); at the handover or any stop it
 * is a plain P_NORMAL task again. Called at its creation and on each of
 * its turns. */
static void splash_worker_class(void)
{
    bool raise = splash_drawable();
    if (!console_task || raise == splash_worker_raised) return;
    splash_worker_raised = raise;
    task_set_base(console_task, P_NORMAL, raise ? SPLASH_WORKER_LEVEL : 0);
}

/* Console worker turn: animate the indeterminate bar, retry deferred
 * updates. Returns the next sleep in milliseconds. */
static uint32_t splash_service(void)
{
    splash_worker_class();
    if (!splash_drawable()) return 100;
    uint32_t f = irq_save();
    if (!splash_bar.determinate) splash_bar.segment = splash_segment_at(&splash_bar.g, g_ticks);
    irq_restore(f);
    splash_update(true);
    splash_accel_update(true);
    bool accel_pending = splash_drawable() && splash_bar.g.accel && splash_bar.accel[0] &&
                         memcmp(splash_bar.accel, splash_bar.accel_shown, SPLASH_ACCEL_TEXT);
    if (accel_pending) return SPLASH_TICK_MS;
    return splash_drawable() && (!splash_bar.determinate || !splash_bar.shown_valid ||
                                 splash_bar.shown_level != splash_bar.level) ? SPLASH_TICK_MS : 100;
}
static void splash_stop(void) { splash_bar.active = false; splash_bar.ended = true; }
#else
static void draw_splash(void) {}
static void splash_handover(void) {}
static uint32_t splash_service(void) { return 100; }
static void splash_stop(void) {}
static void splash_worker_class(void) {}
void console_splash_phase(enum splash_phase phase) { (void)phase; }
void console_splash_accel(bool accelerated, bool used, const char *reason) { (void)accelerated; (void)used; (void)reason; }
#endif

static void clear_screen(void)
{
    for (unsigned r = 0; r < rows; r++) {
        uint32_t flags = line_begin();
        for (unsigned c = 0; c < cols; c++)
            draw_cell(c, r, ' ');
        line_end(flags);
        if (!panic_drawing) kpreempt_point();
    }
    cur_col = cur_row = 0;
}

static void commit_line(void)
{
    linebuf[linelen] = 0;
    memcpy(hist[hist_head], linebuf, linelen + 1);
    hist_head = (hist_head + 1) % HIST_LINES;
    if (hist_count < HIST_LINES)
        hist_count++;
    linelen = 0;
    committed++;
}

void console_init_text(void)
{
    quiet = false;
    splash_stop();
    con_kind = CON_TEXT;
    cols = 80;
    rows = 25;
    clear_screen();
}

bool console_init_lfb(void)
{
    const struct ciuki_boot_info *bi = &g_boot;
    if ((bi->flags & CBI_F_TEXT_MODE) || !bi->fb_phys)
        return false;
    if (bi->fb_bpp != 32 && bi->fb_bpp != 24 && bi->fb_bpp != 16 && bi->fb_bpp != 15)
        return false;
    uint32_t bytes = bi->fb_pitch * bi->fb_height;
    void *va = vmm_map_framebuffer(bi->fb_phys, bytes);
    if (!va)
        return false;
    fb = va;
    pitch = bi->fb_pitch;
    bpp = bi->fb_bpp;
    width = bi->fb_width;
    height = bi->fb_height;
    cols = width / CELL_W;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    rows = height / CELL_H;
    fbdev_console_region(rows * CELL_H);
    con_kind = CON_LFB;
    if (quiet) {
        draw_splash();             /* nothing else is drawn until a wake */
        cur_col = cur_row = 0;
    } else {
        splash_stop();
        clear_screen();
    }
    return true;
}

/* Revoke the separate early-boot console alias before a VBE transaction.
 * Presenter quiescence prevents even panic/overlay writes while revoked. */
void console_framebuffer_revoke(void)
{
    uint8_t *old = __atomic_exchange_n(&fb, 0, __ATOMIC_ACQ_REL);
    if (con_kind == CON_LFB && old) vmm_revoke_mmio(old, pitch * height);
}
void console_framebuffer_changed(const struct fb_device *d)
{
    paint.active = false;
    splash_stop();                 /* the mode set replaced the splash */
    pitch = d->pitch; bpp = d->bpp; width = d->width; height = d->height;
    cols = width / CELL_W; if (cols > MAX_COLS) cols = MAX_COLS;
    rows = height / CELL_H; cur_col = cur_row = 0; con_kind = CON_LFB;
    fbdev_console_region(rows * CELL_H);
    repaint_pending = true; paint_valid = false;
    __atomic_store_n(&fb, d->mapped, __ATOMIC_RELEASE); /* publish after geometry */
}

/* Called once before console_init_lfb; a text console is never quiet. */
void console_set_quiet(bool on) { quiet = on; }
bool console_quiet(void) { return quiet; }

void console_wake(void)
{
    if (!quiet)
        return;
    splash_stop();
    quiet = false;
    repaint_pending = true;
}

void console_set_color(uint32_t f, uint32_t b) { fg = f; bg = b; }

/* Display grant hook (proc/grants.c): owned while a process holds it. The
 * first grant completes the splash bar; its pixels then belong to the
 * owner, whose first frame replaces the splash (f0-55). */
void console_display_owned(bool owned)
{
    if (owned) splash_handover();
    paint.active = false;
    display_owned = owned;
    paint_valid = false;
    if (!owned)
        repaint_pending = true;
}

/* Panic path: plain stores, no lock; the panic lines repaint the console,
 * also on a quiet boot. */
void console_reclaim(void)
{
    pinned_page=0;
    splash_stop();
    quiet = false;
    panic_drawing = true;
    console_display_owned(false);
}

/* The released LFB still shows the desktop: redraw history and the line in
 * progress before the next character. */
static void paint_row(unsigned row, const char *s)
{
    unsigned c = 0;
    bool end = false;
    while (c < cols) {
        uint32_t f = line_begin();
        unsigned limit = c + PAINT_CELLS;
        if (limit > cols) limit = cols;
        for (; c < limit; c++) {
            if (!end && !s[c]) end = true;
            draw_cell(c, row, end ? ' ' : s[c]);
        }
        line_end(f);
        if (!panic_drawing) kpreempt_point();
    }
}

static void repaint(void)
{
    if (display_owned || quiet || !rows || !cols) return;
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
    uint32_t f = irq_save();
    uint32_t target = committed;
    uint8_t *mapping = fb;
    unsigned paint_rows = rows, paint_cols = cols;
    unsigned head = hist_head, n = hist_count < rows - 1 ? hist_count : rows - 1;
    char tail[MAX_COLS + 1]; memcpy(tail, linebuf, linelen); tail[linelen] = 0;
    repaint_pending = false;
    irq_restore(f);
    unsigned delta = target - painted_lines, first = 0;
    if (paint_valid && painted_rows == rows - 1 && n == painted_rows && delta < n) {
        /* Move existing pixels, then rasterize only the newly exposed rows.
         * A claim covers one scanline; no VRAM move holds IF clear. */
        unsigned dy = delta * CELL_H;
        if (con_kind == CON_LFB) for (unsigned y = 0; dy && y < (rows * CELL_H - dy); y++) {
            if (fb != mapping || rows != paint_rows || cols != paint_cols || display_owned || quiet) {
                paint_valid = false; repaint_pending = true; return;
            }
            f = line_begin();
            if (draw_enabled) memmove(fb + y * pitch, fb + (y + dy) * pitch, cols * CELL_W * (bpp / 8));
            line_end(f);
            if ((y & 15) == 15 && !panic_drawing) kpreempt_point();
        }
        else if (con_kind == CON_TEXT && delta) {
            uint16_t *vga = P2V(0xB8000);
            memmove(vga, vga + delta * cols, (rows - delta) * cols * 2);
        }
        first = n - delta;
    } else if (paint_valid && !delta) first = n;
    for (unsigned r = first; r < rows; r++) {
        if (fb != mapping || rows != paint_rows || cols != paint_cols || display_owned || quiet) {
            paint_valid = false; repaint_pending = true; return;
        }
        char row[MAX_COLS + 1];
        f = irq_save();
        if (r < n) memcpy(row, hist[(head + HIST_LINES - n + r) % HIST_LINES], sizeof(row));
        else if (r == n) memcpy(row, tail, sizeof(row));
        else row[0] = 0;
        irq_restore(f);
        paint_row(r, row);
    }
    /* The in-progress line can change without a committed line. */
    if (first > n || first == rows) paint_row(n, tail);
    painted_lines = target; painted_rows = n;
    paint_valid = fb == mapping && rows == paint_rows && cols == paint_cols && !display_owned && !quiet;
    if (cpu_path_add && cpu_path_clock) cpu_path_add(CPU_CONSOLE, cpu_path_clock() - start);
}

static void paint_start(void)
{
    uint32_t f = irq_save();
    paint.mapping = fb; paint.rows = rows; paint.cols = cols;
    paint.target = committed; paint.head = hist_head;
    paint.count = hist_count < rows - 1 ? hist_count : rows - 1;
    memcpy(paint.tail, linebuf, linelen); paint.tail[linelen] = 0;
    paint.page = paint.cursor = pinned_page; paint.pinned = pinned_page != 0;
    repaint_pending = false;
    irq_restore(f);
    paint.row = paint.col = paint.move_y = paint.move_end = 0;
    paint.row_ready = false;
    unsigned delta = paint.target - painted_lines;
    if (!paint.pinned && paint_valid && painted_rows == rows - 1 &&
        paint.count == painted_rows && delta < paint.count) {
        paint.dy = delta * CELL_H;
        if (con_kind == CON_LFB && delta) paint.move_end = rows * CELL_H - paint.dy;
        else if (con_kind == CON_TEXT && delta) {
            uint16_t *vga = P2V(0xB8000);
            memmove(vga, vga + delta * cols, (rows - delta) * cols * 2);
        }
        paint.row = paint.count - delta;
    } else if (!paint.pinned && paint_valid && !delta) paint.row = paint.count;
    paint.active = true;
}

/* Resumable repaint: bounded work even when no other task requests a turn.
 * The history tail is snapshotted once; new lines request the next frame. */
void console_service(void)
{
    if (display_owned || quiet || !rows || !cols) return;
    if (!paint.active) {
        if (!repaint_pending || g_ticks < next_paint) return;
        paint_start();
        next_paint = g_ticks + 100;
    }
    if (fb != paint.mapping || rows != paint.rows || cols != paint.cols || pinned_page != paint.page) {
        paint.active = false; paint_valid = false; repaint_pending = true; return;
    }
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
    if (paint.move_y < paint.move_end) {
        uint32_t f = line_begin();
        bool drew = draw_enabled;
        if (drew) memmove(fb + paint.move_y * pitch, fb + (paint.move_y + paint.dy) * pitch,
                          cols * CELL_W * (bpp / 8));
        line_end(f);
        if (drew) paint.move_y++;
    } else {
        if (!paint.row_ready) {
            if (paint.pinned) {
                unsigned c = 0;
                while (c < cols && *paint.cursor && *paint.cursor != '\n') paint.line[c++] = *paint.cursor++;
                paint.line[c] = 0;
                if (*paint.cursor == '\n') paint.cursor++;
            } else {
                uint32_t f = irq_save();
                if (paint.row < paint.count)
                    memcpy(paint.line, hist[(paint.head + HIST_LINES - paint.count + paint.row) % HIST_LINES], sizeof(paint.line));
                else if (paint.row == paint.count) memcpy(paint.line, paint.tail, sizeof(paint.line));
                else paint.line[0] = 0;
                irq_restore(f);
            }
            unsigned end = (unsigned)strlen(paint.line);
            if (end > cols) end = cols;
            memset(paint.line + end, ' ', cols - end); paint.line[cols] = 0;
            paint.row_ready = true;
        }
        uint32_t f = line_begin_as(paint.pinned);
        bool drew = draw_enabled;
        if (drew) {
            unsigned limit = paint.col + PAINT_CELLS;
            if (limit > cols) limit = cols;
            for (; paint.col < limit; paint.col++) draw_cell(paint.col, paint.row, paint.line[paint.col]);
        }
        line_end(f);
        if (drew && paint.col == cols) {
            paint.col = 0; paint.row++; paint.row_ready = false;
            if (paint.row == rows) {
                paint.active = false;
                painted_lines = paint.target; painted_rows = paint.count;
                paint_valid = !paint.pinned;
            }
        }
    }
    if (cpu_path_add && cpu_path_clock) cpu_path_add(CPU_CONSOLE, cpu_path_clock() - start);
    if (!panic_drawing) kpreempt_point();
}
void console_pin_page(const char *text)
{ paint.active=false; pinned_page=text; paint_valid=false; repaint_pending=true; }
static void console_worker(void *arg)
{
    (void)arg;
    for (;;) {
        console_service();
        uint32_t splash_ms = splash_service();
        task_sleep_ms(paint.active ? 1 : splash_ms);
    }
}
void console_worker_init(void)
{
    struct task *t = task_create_kernel("console", console_worker, 0, P_NORMAL);
    if (!t) panic("console: cannot create worker");
    console_task = t;
    splash_worker_class();         /* f0-55b: above init while the splash is up */
    task_start(t);
}

/* The desktop owns and repaints the LFB. Refresh just one prompt row through
 * the existing console/presenter arbitration, without history scrolling or
 * holding a framebuffer claim across the operator wait. Serial is announced
 * once; retries only redraw the screen after an overlapping presentation. */
void console_operator_prompt(bool announce)
{
    static const char message[] = "[operator] press A, move the pointer, click (60 s)\n";
    if (announce)
        serial_write(message, sizeof(message) - 1);
    if (con_kind == CON_NONE || !rows || !cols) return;
    uint32_t flags = line_begin_as(true);
    unsigned c = 0;
    for (; c < cols && c < sizeof(message) - 2; c++)
        draw_cell(c, 0, message[c]);
    for (; c < cols; c++) draw_cell(c, 0, ' ');
    line_end(flags);
}

void console_write(const char *s, size_t n)
{
    if (con_kind == CON_NONE) return;
    size_t i = 0;
    while (i < n) {
        uint32_t flags = irq_save();
        if (cur_col >= cols || s[i] == '\n') {
            commit_line(); cur_col = 0;
            if (cur_row < rows - 1) cur_row++;
            if (s[i] == '\n') i++;
        } else {
            unsigned bytes = 0;
            while (i < n && cur_col < cols && s[i] != '\n' && bytes++ < 64) {
                char ch = s[i++];
                if (ch == '\r') continue;
                if (linelen < MAX_COLS) linebuf[linelen++] = ch;
                cur_col++;
            }
        }
        if (!pinned_page) repaint_pending = true;
        irq_restore(flags);
    }
    if (panic_drawing) { paint_valid = false; repaint(); }
}

/* Show history page `page` (0 = oldest) with a header line. Used after a
 * probe run to page through the evidence without input. */
unsigned console_pages(void)
{
    unsigned per = rows > 1 ? rows - 1 : 1;
    return (hist_count + per - 1) / per;
}

void console_show_page(unsigned page, const char *header)
{
    if (con_kind == CON_NONE)
        return;
    unsigned per = rows - 1;
    unsigned first = (hist_head + HIST_LINES - hist_count) % HIST_LINES;
    uint32_t flags = line_begin_as(true);
    unsigned c = 0;
    for (; c < cols && header[c]; c++)
        draw_cell(c, 0, header[c]);
    for (; c < cols; c++)
        draw_cell(c, 0, ' ');
    line_end(flags);
    for (unsigned r = 0; r < per; r++) {
        flags = line_begin_as(true);
        unsigned li = page * per + r;
        const char *s = li < hist_count ? hist[(first + li) % HIST_LINES] : "";
        unsigned k = 0;
        for (; k < cols && s[k]; k++)
            draw_cell(k, r + 1, s[k]);
        for (; k < cols; k++)
            draw_cell(k, r + 1, ' ');
        line_end(flags);
        if (!panic_drawing) kpreempt_point();
    }
}
