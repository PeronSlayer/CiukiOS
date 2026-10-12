/* Screen console: VGA text mode or the boot LFB with the bundled
 * Inconsolata bitmap (assets/fonts/native/INCONSOL.CFN, OFL 1.1).
 * Keeps a history of lines so earlier records can be shown again
 * (docs/design/f0-acceptance.md, screen evidence and paging).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/fbdev.h>

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

static uint32_t line_begin_as(bool overlay)
{
    uint32_t flags = irq_save();
    draw_enabled = con_kind != CON_LFB ||
                   (__atomic_load_n(&fb, __ATOMIC_ACQUIRE) && (overlay || (!display_owned && !quiet)) && fbdev_console_begin());
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
 * area that fits and centred on black. Drawn once, before any line. */
#define SPLASH_W 512u
#define SPLASH_H 384u
__asm__(".pushsection .rodata.splash,\"a\"\n.balign 4\n"
        ".global console_splash, console_splash_end\nconsole_splash:\n"
        ".incbin \"" CIUKI_SPLASH_BIN "\"\nconsole_splash_end:\n.popsection\n");
extern const uint8_t console_splash[], console_splash_end[];

static void draw_splash(void)
{
    if ((uint32_t)(console_splash_end - console_splash) != SPLASH_W * SPLASH_H * 2)
        return;
    uint32_t dw = width, dh = width * 3 / 4;
    if (dh > height) { dh = height; dw = height * 4 / 3; }
    uint32_t x0 = (width - dw) / 2, y0 = (height - dh) / 2, black = pack(0);
    for (uint32_t y = 0; y < height; y++) {
        bool inside = y >= y0 && y < y0 + dh;
        const uint8_t *row = console_splash + (inside ? (y - y0) * SPLASH_H / dh : 0) * SPLASH_W * 2;
        for (uint32_t x = 0; x < width; x++) {
            if (!inside || x < x0 || x >= x0 + dw) { put_px(x, y, black); continue; }
            const uint8_t *s = row + (x - x0) * SPLASH_W / dw * 2;
            uint32_t v = s[0] | (uint32_t)s[1] << 8;
            uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            put_px(x, y, pack((r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2)));
        }
    }
}
#else
static void draw_splash(void) {}
#endif

static void clear_screen(void)
{
    for (unsigned r = 0; r < rows; r++) {
        uint32_t flags = line_begin();
        for (unsigned c = 0; c < cols; c++)
            draw_cell(c, r, ' ');
        line_end(flags);
    }
    cur_col = cur_row = 0;
}

static void scroll(void)
{
    /* Redraw from history: robust for both kinds and avoids reading VRAM. */
    unsigned shown = rows - 1;
    unsigned n = hist_count < shown ? hist_count : shown;
    if (con_kind == CON_LFB && (display_owned || quiet)) {
        cur_row = n;
        cur_col = 0;
        return;
    }
    for (unsigned r = 0; r < rows; r++) {
        uint32_t flags = line_begin();
        const char *s = "";
        if (r < n) {
            unsigned idx = (hist_head + HIST_LINES - n + r) % HIST_LINES;
            s = hist[idx];
        }
        unsigned c = 0;
        for (; c < cols && s[c]; c++)
            draw_cell(c, r, s[c]);
        for (; c < cols; c++)
            draw_cell(c, r, ' ');
        line_end(flags);
    }
    cur_row = n;
    cur_col = 0;
}

static void commit_line(void)
{
    linebuf[linelen] = 0;
    memcpy(hist[hist_head], linebuf, linelen + 1);
    hist_head = (hist_head + 1) % HIST_LINES;
    if (hist_count < HIST_LINES)
        hist_count++;
    linelen = 0;
}

void console_init_text(void)
{
    quiet = false;
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
    void *va = vmm_map_mmio(bi->fb_phys, bytes, true);
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
    } else
        clear_screen();
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
    pitch = d->pitch; bpp = d->bpp; width = d->width; height = d->height;
    cols = width / CELL_W; if (cols > MAX_COLS) cols = MAX_COLS;
    rows = height / CELL_H; cur_col = cur_row = 0; con_kind = CON_LFB;
    fbdev_console_region(rows * CELL_H);
    repaint_pending = true;
    __atomic_store_n(&fb, d->mapped, __ATOMIC_RELEASE); /* publish after geometry */
}

/* Called once before console_init_lfb; a text console is never quiet. */
void console_set_quiet(bool on) { quiet = on; }
bool console_quiet(void) { return quiet; }

void console_wake(void)
{
    if (!quiet)
        return;
    quiet = false;
    repaint_pending = true;
}

void console_set_color(uint32_t f, uint32_t b) { fg = f; bg = b; }

/* Display grant hook (proc/grants.c): owned while a process holds it. */
void console_display_owned(bool owned)
{
    display_owned = owned;
    if (!owned)
        repaint_pending = true;
}

/* Panic path: plain stores, no lock; the panic lines repaint the console,
 * also on a quiet boot. */
void console_reclaim(void)
{
    quiet = false;
    console_display_owned(false);
}

/* The released LFB still shows the desktop: redraw history and the line in
 * progress before the next character. */
static void repaint(void)
{
    repaint_pending = false;
    scroll();
    if (!linelen)
        return;
    uint32_t flags = line_begin();
    unsigned c = 0;
    for (; c < linelen && c < cols; c++)
        draw_cell(c, cur_row, linebuf[c]);
    cur_col = c;
    line_end(flags);
}

/* The desktop owns and repaints the LFB. Refresh just one prompt row through
 * the existing console/presenter arbitration, without history scrolling or
 * holding a framebuffer claim across the operator wait. Serial is announced
 * once; retries only redraw the screen after an overlapping presentation. */
void console_operator_prompt(bool announce)
{
    static const char message[] = "[operator] press A, move the pointer, click (60 s)";
    if (announce) {
        serial_write(message, sizeof(message) - 1);
        serial_write("\n", 1);
    }
    if (con_kind == CON_NONE || !rows || !cols) return;
    uint32_t flags = line_begin_as(true);
    unsigned c = 0;
    for (; c < cols && c < sizeof(message) - 1; c++)
        draw_cell(c, 0, message[c]);
    for (; c < cols; c++) draw_cell(c, 0, ' ');
    line_end(flags);
}

void console_write(const char *s, size_t n)
{
    if (con_kind == CON_NONE)
        return;
    if (repaint_pending && !display_owned && !quiet && con_kind == CON_LFB)
        repaint();
    size_t i = 0;
    while (i < n) {
        if (cur_col >= cols || s[i] == '\n') {
            uint32_t flags = irq_save();
            commit_line();
            cur_col = 0;
            bool redraw = ++cur_row >= rows;
            if (s[i] == '\n') i++;
            irq_restore(flags);
            if (redraw) scroll();
            continue;
        }
        uint32_t flags = line_begin();
        while (i < n && cur_col < cols && s[i] != '\n') {
            char ch = s[i++];
            if (ch == '\r') continue;
            if (linelen < MAX_COLS)
                linebuf[linelen++] = ch;
            draw_cell(cur_col++, cur_row, ch);
        }
        line_end(flags);
    }
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
    }
}
