/* Screen console: VGA text mode or the boot LFB with the bundled
 * Inconsolata bitmap (assets/fonts/native/INCONSOL.CFN, OFL 1.1).
 * Keeps a history of lines so earlier records can be shown again
 * (docs/design/f0-acceptance.md, screen evidence and paging).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>

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

static uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    const struct ciuki_boot_info *bi = &g_boot;
    if (bpp == 16 || bpp == 15)
        return ((r >> (8 - bi->fb_red_size)) << bi->fb_red_pos) |
               ((g >> (8 - bi->fb_green_size)) << bi->fb_green_pos) |
               ((b >> (8 - bi->fb_blue_size)) << bi->fb_blue_pos);
    return (r << bi->fb_red_pos) | (g << bi->fb_green_pos) | (b << bi->fb_blue_pos);
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

static void clear_screen(void)
{
    for (unsigned r = 0; r < rows; r++)
        for (unsigned c = 0; c < cols; c++)
            draw_cell(c, r, ' ');
    cur_col = cur_row = 0;
}

static void scroll(void)
{
    /* Redraw from history: robust for both kinds and avoids reading VRAM. */
    unsigned shown = rows - 1;
    unsigned n = hist_count < shown ? hist_count : shown;
    for (unsigned r = 0; r < rows; r++) {
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
    con_kind = CON_LFB;
    clear_screen();
    return true;
}

void console_set_color(uint32_t f, uint32_t b) { fg = f; bg = b; }

void console_write(const char *s, size_t n)
{
    if (con_kind == CON_NONE)
        return;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\r')
            continue;
        if (ch == '\n' || cur_col >= cols) {
            commit_line();
            cur_col = 0;
            if (++cur_row >= rows)
                scroll();
            if (ch == '\n')
                continue;
        }
        if (linelen < MAX_COLS)
            linebuf[linelen++] = ch;
        draw_cell(cur_col++, cur_row, ch);
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
    unsigned c = 0;
    for (; c < cols && header[c]; c++)
        draw_cell(c, 0, header[c]);
    for (; c < cols; c++)
        draw_cell(c, 0, ' ');
    for (unsigned r = 0; r < per; r++) {
        unsigned li = page * per + r;
        const char *s = li < hist_count ? hist[(first + li) % HIST_LINES] : "";
        unsigned k = 0;
        for (; k < cols && s[k]; k++)
            draw_cell(k, r + 1, s[k]);
        for (; k < cols; k++)
            draw_cell(k, r + 1, ' ');
    }
}
