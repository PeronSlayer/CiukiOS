#include "virtual_vga_bios.h"

/* Original CiukiOS implementation of the documented IBM VGA BIOS interface
 * (IBM Personal System/2 and Personal Computer BIOS Interface Technical
 * Reference, INT 10h). BDA offsets follow the same reference. Behavioural
 * details that the reference leaves open were cross-checked against the
 * SeaVGABIOS sources (vgasrc/vgabios.c, stdvga.c) at rel-1.16.3, without
 * copying code. Default palettes are verified by scripts/test_virtual_vga.py
 * against the installed QEMU VGA ROM. */

#define BDA 0x400u
#define B_MODE 0x49
#define B_COLS 0x4a
#define B_PAGE_SIZE 0x4c
#define B_PAGE_START 0x4e
#define B_CURSOR 0x50
#define B_CURSOR_TYPE 0x60
#define B_PAGE 0x62
#define B_CRTC 0x63
#define B_MSR 0x65
#define B_PAL 0x66
#define B_ROWS 0x84
#define B_CHEIGHT 0x85
#define B_CTL 0x87
#define B_SWITCHES 0x88
#define B_MODESET 0x89
#define B_DCC 0x8a

#define AL(r) ((r)->eax & 0xff)
#define AH(r) (((r)->eax >> 8) & 0xff)
#define BL(r) ((r)->ebx & 0xff)
#define BH(r) (((r)->ebx >> 8) & 0xff)
#define CL(r) ((r)->ecx & 0xff)
#define CH(r) (((r)->ecx >> 8) & 0xff)
#define DL(r) ((r)->edx & 0xff)
#define DH(r) (((r)->edx >> 8) & 0xff)

static const uint8_t cga16[16][3] = {
    {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
    {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}
};
static const uint8_t grays[16] = {0,5,8,11,14,17,20,24,28,32,36,40,45,50,56,63};
/* Five component levels (low .. high) of each of the nine 24-hue groups. */
static const uint8_t levels[9][5] = {
    {0,16,31,47,63},{31,39,47,55,63},{45,49,54,58,63},
    {0,7,14,21,28},{14,17,21,24,28},{20,22,24,26,28},
    {0,4,8,12,16},{8,10,12,14,16},{11,12,13,15,16}
};
static const uint8_t hue_r[24] = {0,1,2,3,4,4,4,4,4,4,4,4,4,3,2,1,0,0,0,0,0,0,0,0};
static const uint8_t hue_g[24] = {0,0,0,0,0,0,0,0,0,1,2,3,4,4,4,4,4,4,4,4,4,3,2,1};
static const uint8_t hue_b[24] = {4,4,4,4,4,3,2,1,0,0,0,0,0,0,0,0,0,1,2,3,4,4,4,4};
/* CGA mode-select register values for text modes 00h-03h. */
static const uint8_t text_msr[4] = {0x2c,0x28,0x2d,0x29};

unsigned cvbios_default_palette(unsigned which, uint8_t *dest)
{
    unsigned i, c;
    if (which == 1) {
        for (i = 0; i < 64; ++i)
            for (c = 0; c < 3; ++c) dest[i * 3 + c] = cga16[(i & 7) + ((i >> 1) & 8)][c];
        return 64;
    }
    if (which == 2) {
        for (i = 0; i < 64; ++i) {
            dest[i * 3] = (uint8_t)(((i >> 2) & 1) * 42 + ((i >> 5) & 1) * 21);
            dest[i * 3 + 1] = (uint8_t)(((i >> 1) & 1) * 42 + ((i >> 4) & 1) * 21);
            dest[i * 3 + 2] = (uint8_t)((i & 1) * 42 + ((i >> 3) & 1) * 21);
        }
        return 64;
    }
    if (which != 3) return 0;
    for (i = 0; i < 16; ++i)
        for (c = 0; c < 3; ++c) {
            dest[i * 3 + c] = cga16[i][c];
            dest[48 + i * 3 + c] = grays[i];
        }
    for (i = 0; i < 216; ++i) {
        const uint8_t *l = levels[i / 24];
        dest[96 + i * 3] = l[hue_r[i % 24]];
        dest[96 + i * 3 + 1] = l[hue_g[i % 24]];
        dest[96 + i * 3 + 2] = l[hue_b[i % 24]];
    }
    for (i = 744; i < 768; ++i) dest[i] = 0;
    return 256;
}

static uint8_t rd8(cvbios *b, uint32_t linear)
{
    uint8_t v = 0;
    b->bus->read(b->bus->context, linear, &v);
    return v;
}

static void wr8(cvbios *b, uint32_t linear, uint8_t v)
{
    b->bus->write(b->bus->context, linear, v);
}

static unsigned rd16(cvbios *b, uint32_t linear)
{
    return rd8(b, linear) | ((unsigned)rd8(b, linear + 1) << 8);
}

static void wr16(cvbios *b, uint32_t linear, unsigned v)
{
    wr8(b, linear, (uint8_t)v);
    wr8(b, linear + 1, (uint8_t)(v >> 8));
}

static uint32_t farptr_linear(uint32_t pointer)
{
    return ((pointer >> 16) << 4) + (pointer & 0xffff);
}

static uint32_t es_linear(const cvbios_regs *r, uint32_t offset)
{
    return ((uint32_t)r->es << 4) + (offset & 0xffff);
}

static void set_al(cvbios_regs *r, unsigned v) { r->eax = (r->eax & ~0xffUL) | (v & 0xff); }
static void set_ax(cvbios_regs *r, unsigned v) { r->eax = (r->eax & ~0xffffUL) | (v & 0xffff); }
static void set_bx(cvbios_regs *r, unsigned v) { r->ebx = (r->ebx & ~0xffffUL) | (v & 0xffff); }
static void set_cx(cvbios_regs *r, unsigned v) { r->ecx = (r->ecx & ~0xffffUL) | (v & 0xffff); }
static void set_dx(cvbios_regs *r, unsigned v) { r->edx = (r->edx & ~0xffffUL) | (v & 0xffff); }
static void set_bp(cvbios_regs *r, unsigned v) { r->ebp = (r->ebp & ~0xffffUL) | (v & 0xffff); }

static void crtc_set(cvga_state *v, unsigned index, uint8_t value)
{
    if (v->crtc[index] == value) return;
    v->crtc[index] = value;
    ++v->changes;
    ++v->display_changes;
}

static void attr_set(cvga_state *v, unsigned index, uint8_t value)
{
    if (index < 16) value &= 63;
    else if (index == 16) value &= 0xef;
    else if (index == 18) value &= 63;
    else if (index >= 19) value &= 15;
    if (index > 20 || v->attr[index] == value) return;
    v->attr[index] = value;
    ++v->changes;
    ++v->display_changes;
}

static void dac_set(cvbios *b, unsigned index, uint8_t red, uint8_t green, uint8_t blue)
{
    cvga_state *v = b->vga;
    uint8_t *e = v->dac[index & 255];
    red &= 63; green &= 63; blue &= 63;
    if (rd8(b, BDA + B_MODESET) & 2) {
        /* Gray-scale summing: 30% red, 59% green, 11% blue. */
        unsigned gray = (77u * red + 151u * green + 28u * blue + 0x80u) >> 8;
        red = green = blue = (uint8_t)(gray > 63 ? 63 : gray);
    }
    if (e[0] == red && e[1] == green && e[2] == blue) return;
    e[0] = red; e[1] = green; e[2] = blue;
    ++v->changes;
    ++v->display_changes;
}

unsigned cvbios_current_mode(const cvbios *b)
{
    uint8_t v = 0;
    b->bus->read(b->bus->context, BDA + B_MODE, &v);
    return v;
}

static const cvga_mode_params *current(cvbios *b)
{
    return cvga_find_mode(cvbios_current_mode(b));
}

/* ---- Text cells use the CPU path, exactly like firmware writing B800. ---- */

static uint32_t cell_address(cvbios *b, const cvga_mode_params *m, unsigned page,
                             unsigned row, unsigned col)
{
    unsigned cols = rd16(b, BDA + B_COLS);
    return ((uint32_t)m->segment << 4) + (uint32_t)page * rd16(b, BDA + B_PAGE_SIZE) +
           ((uint32_t)row * cols + col) * 2u;
}

static void text_put(cvbios *b, uint32_t at, int ch, int attribute)
{
    if (ch >= 0) cvga_write_vram(b->vga, at, (uint8_t)ch);
    if (attribute >= 0) cvga_write_vram(b->vga, at + 1, (uint8_t)attribute);
}

/* ---- Graphics primitives write planes directly, as firmware does after
 * reprogramming the sequencer/graphics controller. ---- */

static unsigned graphics_cheight(cvbios *b)
{
    unsigned h = rd16(b, BDA + B_CHEIGHT);
    return h ? h : 8;
}

/* Bit 7 of a 16-colour pixel requests XOR; in 256-colour mode it is part
 * of the colour index (IBM INT 10h functions 09h/0Ch). */
static int xor_colour(const cvga_mode_params *m, unsigned colour)
{
    return m->palette != 3 && (colour & 0x80);
}

static void pixel_put(cvbios *b, const cvga_mode_params *m, unsigned page,
                      unsigned x, unsigned y, unsigned colour)
{
    cvga_state *v = b->vga;
    unsigned at, p, bit;
    uint8_t old;
    if (x >= m->width || y >= m->height) return;
    if (m->palette == 3) {
        at = y * 320u + x;
        cvga_store_plane(v, at & 3, at & 0xfffc, (uint8_t)colour);
        return;
    }
    at = page * rd16(b, BDA + B_PAGE_SIZE) + y * (m->width / 8) + x / 8;
    bit = 0x80u >> (x & 7);
    for (p = 0; p < 4; ++p) {
        old = v->plane[p][at & 0xffff];
        if (colour & 0x80) {
            if (colour & (1u << p)) old ^= (uint8_t)bit;
        } else old = (uint8_t)((colour & (1u << p)) ? (old | bit) : (old & ~bit));
        cvga_store_plane(v, p, at, old);
    }
}

static unsigned pixel_get(cvbios *b, const cvga_mode_params *m, unsigned page,
                          unsigned x, unsigned y)
{
    cvga_state *v = b->vga;
    unsigned at, p, colour = 0;
    if (x >= m->width || y >= m->height) return 0;
    if (m->palette == 3) {
        at = y * 320u + x;
        return v->plane[at & 3][at & 0xfffc];
    }
    at = (page * rd16(b, BDA + B_PAGE_SIZE) + y * (m->width / 8) + x / 8) & 0xffff;
    for (p = 0; p < 4; ++p)
        if (v->plane[p][at] & (0x80u >> (x & 7))) colour |= 1u << p;
    return colour;
}

static void glyph_put(cvbios *b, const cvga_mode_params *m, unsigned page, unsigned row,
                      unsigned col, unsigned ch, unsigned colour)
{
    unsigned h = graphics_cheight(b), line, x, bits;
    uint32_t font = farptr_linear(rd16(b, 0x43 * 4) | ((uint32_t)rd16(b, 0x43 * 4 + 2) << 16));
    for (line = 0; line < h; ++line) {
        bits = rd8(b, font + ch * h + line);
        for (x = 0; x < 8; ++x) {
            if (bits & (0x80u >> x))
                pixel_put(b, m, page, col * 8 + x, row * h + line, colour);
            else if (!xor_colour(m, colour))
                pixel_put(b, m, page, col * 8 + x, row * h + line, 0);
        }
    }
}

/* Move one character cell (all scanlines) within a graphics page. */
static void graphics_cell_copy(cvbios *b, const cvga_mode_params *m, unsigned page,
                               unsigned dst_row, unsigned src_row, unsigned col)
{
    unsigned h = graphics_cheight(b), line, x;
    for (line = 0; line < h; ++line)
        for (x = 0; x < 8; ++x)
            pixel_put(b, m, page, col * 8 + x, dst_row * h + line,
                      pixel_get(b, m, page, col * 8 + x, src_row * h + line));
}

static void graphics_cell_fill(cvbios *b, const cvga_mode_params *m, unsigned page,
                               unsigned row, unsigned col, unsigned colour)
{
    unsigned h = graphics_cheight(b), line, x;
    for (line = 0; line < h; ++line)
        for (x = 0; x < 8; ++x)
            pixel_put(b, m, page, col * 8 + x, row * h + line, m->palette == 3 ? colour : colour & 0x7f);
}

/* ---- Cursor, pages, scrolling ---- */

static void cursor_crtc(cvbios *b, unsigned page)
{
    unsigned pos = rd16(b, BDA + B_CURSOR + page * 2), cols = rd16(b, BDA + B_COLS);
    unsigned loc = page * rd16(b, BDA + B_PAGE_SIZE) / 2 + (pos >> 8) * cols + (pos & 0xff);
    crtc_set(b->vga, 14, (uint8_t)(loc >> 8));
    crtc_set(b->vga, 15, (uint8_t)loc);
}

static void set_cursor(cvbios *b, unsigned page, unsigned row, unsigned col)
{
    page &= 7;
    wr16(b, BDA + B_CURSOR + page * 2, (row << 8) | col);
    if (page == rd8(b, BDA + B_PAGE)) cursor_crtc(b, page);
}

static void set_cursor_shape(cvbios *b, unsigned start, unsigned end)
{
    unsigned h = rd16(b, BDA + B_CHEIGHT);
    wr16(b, BDA + B_CURSOR_TYPE, (start << 8) | end);
    /* CGA cursor emulation, enabled while BDA 0487 bit 0 is clear. */
    if (!(rd8(b, BDA + B_CTL) & 1) && h > 8 && end < 8 && start < 0x20) {
        if (end != start + 1) start = ((start + 1) * h / 8) - 1;
        else start = ((end + 1) * h / 8) - 2;
        end = ((end + 1) * h / 8) - 1;
    }
    crtc_set(b->vga, 10, (uint8_t)((b->vga->crtc[10] & 0xc0) | (start & 0x3f)));
    crtc_set(b->vga, 11, (uint8_t)((b->vga->crtc[11] & 0xe0) | (end & 0x1f)));
}

static void set_page(cvbios *b, unsigned page)
{
    const cvga_mode_params *m = current(b);
    uint32_t start;
    if (!m || page > 7) return;
    start = (uint32_t)page * rd16(b, BDA + B_PAGE_SIZE);
    if (start >= 0x10000UL) return;
    wr8(b, BDA + B_PAGE, (uint8_t)page);
    wr16(b, BDA + B_PAGE_START, (unsigned)start);
    if (m->text) start /= 2;
    crtc_set(b->vga, 12, (uint8_t)(start >> 8));
    crtc_set(b->vga, 13, (uint8_t)start);
    cursor_crtc(b, page);
}

/* direction 0 = up (06h), 1 = down (07h); lines 0 clears the window. */
static void scroll(cvbios *b, unsigned page, unsigned lines, unsigned attribute,
                   unsigned top, unsigned left, unsigned bottom, unsigned right, int down)
{
    const cvga_mode_params *m = current(b);
    unsigned cols, rows, height, row, col, src;
    if (!m) return;
    cols = rd16(b, BDA + B_COLS);
    rows = rd8(b, BDA + B_ROWS) + 1u;
    if (right >= cols) right = cols - 1;
    if (bottom >= rows) bottom = rows - 1;
    if (top > bottom || left > right) return;
    height = bottom - top + 1;
    if (!lines || lines > height) lines = height;
    for (row = 0; row < height; ++row) {
        unsigned dst = down ? bottom - row : top + row;
        int clear = row >= height - lines;
        src = down ? dst - lines : dst + lines;
        for (col = left; col <= right; ++col) {
            if (m->text) {
                uint32_t at = cell_address(b, m, page, dst, col);
                if (clear) text_put(b, at, ' ', (int)attribute);
                else {
                    uint32_t from = cell_address(b, m, page, src, col);
                    uint8_t ch = cvga_read_vram(b->vga, from);
                    uint8_t at2 = cvga_read_vram(b->vga, from + 1);
                    text_put(b, at, ch, at2);
                }
            } else if (clear) graphics_cell_fill(b, m, page, dst, col, attribute);
            else graphics_cell_copy(b, m, page, dst, src, col);
        }
    }
}

/* Write one printable character at a cell. attribute < 0 keeps text attr. */
static void write_cell(cvbios *b, unsigned page, unsigned row, unsigned col,
                       unsigned ch, int attribute)
{
    const cvga_mode_params *m = current(b);
    if (!m) return;
    if (m->text) text_put(b, cell_address(b, m, page, row, col), (int)ch, attribute);
    else glyph_put(b, m, page, row, col, ch, attribute < 0 ? 0 : (unsigned)attribute);
}

static void teletype(cvbios *b, unsigned page, unsigned ch, int attribute)
{
    const cvga_mode_params *m = current(b);
    unsigned pos = rd16(b, BDA + B_CURSOR + (page & 7) * 2);
    unsigned row = pos >> 8, col = pos & 0xff;
    unsigned cols = rd16(b, BDA + B_COLS), rows = rd8(b, BDA + B_ROWS) + 1u;
    if (!m) return;
    switch (ch) {
    case 7: break;
    case 8: if (col) --col; break;
    case 10: ++row; break;
    case 13: col = 0; break;
    default:
        write_cell(b, page, row, col, ch, attribute);
        if (++col >= cols) {
            col = 0;
            ++row;
        }
        break;
    }
    if (row >= rows) {
        unsigned fill = 0;
        row = rows - 1;
        if (m->text)
            fill = cvga_peek_vram(b->vga, cell_address(b, m, page, row, col) + 1);
        scroll(b, page, 1, fill, 0, 0, rows - 1, cols - 1, 0);
    }
    set_cursor(b, page, row, col);
}

/* ---- Fonts ---- */

static uint32_t font_offset(unsigned block)
{
    return ((uint32_t)(block & 3) << 14) | ((uint32_t)(block & 4) << 11);
}

static void load_font(cvbios *b, uint32_t source, unsigned first, unsigned count,
                      unsigned block, unsigned height)
{
    unsigned c, line;
    uint32_t base = font_offset(block);
    for (c = 0; c < count && first + c < 256; ++c)
        for (line = 0; line < 32; ++line)
            cvga_store_plane(b->vga, 2, base + (first + c) * 32 + line,
                             line < height ? rd8(b, source + c * height + line) : 0);
}

static int rom_font(cvbios *b, unsigned which, unsigned *height, uint32_t *linear)
{
    static const unsigned heights[CVBIOS_FONT_COUNT] = {8, 8, 14, 16};
    if (which >= CVBIOS_FONT_COUNT || !b->font[which]) return 0;
    *height = heights[which];
    *linear = farptr_linear(b->font[which]);
    return 1;
}

static void text_rows_for_height(cvbios *b, unsigned height)
{
    cvga_state *v = b->vga;
    unsigned vde, rows, cols;
    crtc_set(v, 9, (uint8_t)((v->crtc[9] & 0xe0) | ((height - 1) & 0x1f)));
    wr16(b, BDA + B_CHEIGHT, height);
    vde = (v->crtc[18] | ((unsigned)(v->crtc[7] & 2) << 7) |
           ((unsigned)(v->crtc[7] & 64) << 3)) + 1;
    rows = vde / height;
    cols = rd16(b, BDA + B_COLS);
    wr8(b, BDA + B_ROWS, (uint8_t)(rows - 1));
    wr16(b, BDA + B_PAGE_SIZE, ((cols * rows * 2) + 0x7ff) & ~0x7ffu);
    if (height == 8) set_cursor_shape(b, 6, 7);
    else set_cursor_shape(b, height - 3, height - 2);
}

static void set_vector(cvbios *b, unsigned vector, uint32_t pointer)
{
    wr16(b, vector * 4, pointer & 0xffff);
    wr16(b, vector * 4 + 2, pointer >> 16);
}

static int font_services(cvbios *b, cvbios_regs *r)
{
    unsigned sub = AL(r), height = 0, rows;
    uint32_t linear = 0, pointer = 0;
    switch (sub) {
    case 0x00: case 0x10:
        load_font(b, es_linear(r, r->ebp), r->edx & 0xffff, r->ecx & 0xffff, BL(r), BH(r));
        if (sub == 0x10 && BH(r)) text_rows_for_height(b, BH(r));
        return 1;
    case 0x01: case 0x11: case 0x02: case 0x12: case 0x04: case 0x14:
        if (!rom_font(b, (sub & 7) == 1 ? CVBIOS_FONT_8X14 : (sub & 7) == 2 ?
                         CVBIOS_FONT_8X8 : CVBIOS_FONT_8X16, &height, &linear)) return 0;
        load_font(b, linear, 0, (sub & 7) == 2 ? 128 : 256, BL(r), height);
        if ((sub & 7) == 2 && b->font[CVBIOS_FONT_8X8_HIGH])
            load_font(b, farptr_linear(b->font[CVBIOS_FONT_8X8_HIGH]), 128, 128, BL(r), 8);
        if (sub & 0x10) text_rows_for_height(b, height);
        return 1;
    case 0x03: {
        uint8_t value = (uint8_t)(BL(r) & 0x3f);
        if (b->vga->seq[3] != value) {
            b->vga->seq[3] = value;
            ++b->vga->changes;
            ++b->vga->display_changes;
        }
        return 1;
    }
    case 0x20: set_vector(b, 0x1f, ((uint32_t)r->es << 16) | (r->ebp & 0xffff)); return 1;
    case 0x21: case 0x22: case 0x23: case 0x24:
        if (sub == 0x21) {
            pointer = ((uint32_t)r->es << 16) | (r->ebp & 0xffff);
            height = r->ecx & 0xffff;
        } else {
            if (!rom_font(b, sub == 0x22 ? CVBIOS_FONT_8X14 : sub == 0x23 ?
                             CVBIOS_FONT_8X8 : CVBIOS_FONT_8X16, &height, &linear)) return 0;
            pointer = b->font[sub == 0x22 ? CVBIOS_FONT_8X14 : sub == 0x23 ?
                          CVBIOS_FONT_8X8 : CVBIOS_FONT_8X16];
        }
        set_vector(b, 0x43, pointer);
        switch (BL(r)) {
        case 0: rows = DL(r); break;
        case 1: rows = 14; break;
        case 3: rows = 43; break;
        default: rows = 25; break;
        }
        if (rows) wr8(b, BDA + B_ROWS, (uint8_t)(rows - 1));
        wr16(b, BDA + B_CHEIGHT, height);
        return 1;
    case 0x30:
        switch (BH(r)) {
        case 0: pointer = rd16(b, 0x1f * 4) | ((uint32_t)rd16(b, 0x1f * 4 + 2) << 16); break;
        case 1: pointer = rd16(b, 0x43 * 4) | ((uint32_t)rd16(b, 0x43 * 4 + 2) << 16); break;
        case 2: case 5: pointer = b->font[CVBIOS_FONT_8X14]; break;
        case 3: pointer = b->font[CVBIOS_FONT_8X8]; break;
        case 4: pointer = b->font[CVBIOS_FONT_8X8_HIGH]; break;
        case 6: case 7: pointer = b->font[CVBIOS_FONT_8X16]; break;
        default: return 0;
        }
        r->es = (uint16_t)(pointer >> 16);
        set_bp(r, pointer & 0xffff);
        set_cx(r, rd16(b, BDA + B_CHEIGHT));
        r->edx = (r->edx & ~0xffUL) | rd8(b, BDA + B_ROWS);
        return 1;
    default:
        return 0;
    }
}

/* ---- Palette and DAC ---- */

static int palette_services(cvbios *b, cvbios_regs *r)
{
    cvga_state *v = b->vga;
    unsigned i, index = r->ebx & 0xffff, count = r->ecx & 0xffff;
    uint32_t table = es_linear(r, r->edx);
    switch (AL(r)) {
    case 0x00: attr_set(v, BL(r) & 31, (uint8_t)BH(r)); return 1;
    case 0x01: attr_set(v, 17, (uint8_t)BH(r)); return 1;
    case 0x02:
        for (i = 0; i < 16; ++i) attr_set(v, i, rd8(b, table + i));
        attr_set(v, 17, rd8(b, table + 16));
        return 1;
    case 0x03:
        attr_set(v, 16, (uint8_t)(BL(r) ? v->attr[16] | 8 : v->attr[16] & ~8u));
        return 1;
    case 0x07:
        if (BL(r) < 21) r->ebx = (r->ebx & ~0xff00UL) | ((uint32_t)v->attr[BL(r)] << 8);
        return 1;
    case 0x08: r->ebx = (r->ebx & ~0xff00UL) | ((uint32_t)v->attr[17] << 8); return 1;
    case 0x09:
        for (i = 0; i < 16; ++i) wr8(b, table + i, v->attr[i]);
        wr8(b, table + 16, v->attr[17]);
        return 1;
    case 0x10: dac_set(b, index, (uint8_t)DH(r), (uint8_t)CH(r), (uint8_t)CL(r)); return 1;
    case 0x12:
        for (i = 0; i < count && index + i < 256; ++i)
            dac_set(b, index + i, rd8(b, table + i * 3), rd8(b, table + i * 3 + 1),
                    rd8(b, table + i * 3 + 2));
        return 1;
    case 0x13:
        if (!BL(r)) attr_set(v, 16, (uint8_t)(BH(r) ? v->attr[16] | 0x80 : v->attr[16] & 0x7f));
        else if (v->attr[16] & 0x80) attr_set(v, 20, (uint8_t)(BH(r) & 15));
        else attr_set(v, 20, (uint8_t)((BH(r) << 2) & 12));
        return 1;
    case 0x15:
        i = BL(r);
        r->edx = (r->edx & ~0xff00UL) | ((uint32_t)v->dac[i][0] << 8);
        r->ecx = (r->ecx & ~0xffffUL) | ((uint32_t)v->dac[i][1] << 8) | v->dac[i][2];
        return 1;
    case 0x17:
        for (i = 0; i < count && index + i < 256; ++i) {
            wr8(b, table + i * 3, v->dac[index + i][0]);
            wr8(b, table + i * 3 + 1, v->dac[index + i][1]);
            wr8(b, table + i * 3 + 2, v->dac[index + i][2]);
        }
        return 1;
    case 0x18:
        if (v->dac_mask != BL(r)) {
            v->dac_mask = (uint8_t)BL(r);
            ++v->changes;
            ++v->display_changes;
        }
        return 1;
    case 0x19: r->ebx = (r->ebx & ~0xffUL) | v->dac_mask; return 1;
    case 0x1a:
        i = (v->attr[16] & 0x80) ? v->attr[20] & 15 : (v->attr[20] >> 2) & 3;
        set_bx(r, (i << 8) | (unsigned)(v->attr[16] >> 7));
        return 1;
    case 0x1b:
        for (i = 0; i < count && index + i < 256; ++i) {
            uint8_t *e = v->dac[index + i];
            unsigned gray = (77u * e[0] + 151u * e[1] + 28u * e[2] + 0x80u) >> 8;
            if (gray > 63) gray = 63;
            if (e[0] != gray || e[1] != gray || e[2] != gray) {
                e[0] = e[1] = e[2] = (uint8_t)gray;
                ++v->changes;
                ++v->display_changes;
            }
        }
        return 1;
    default:
        return 0;
    }
}

/* ---- Mode set ---- */

static int set_mode(cvbios *b, cvbios_regs *r)
{
    cvga_state *v = b->vga;
    unsigned mode = AL(r) & 0x7f, preserve = AL(r) & 0x80, i, count, page;
    const cvga_mode_params *m = cvga_find_mode(mode);
    static uint8_t palette[768];
    uint32_t font;
    if (!m || mode > 0x13) return 0;
    if (!cvga_set_bios_mode(v, mode | 0x80, 1)) return 0;
    if (!preserve) {
        for (i = 0; i < 0x10000UL; ++i) {
            uint8_t zero = 0;
            if (m->text && i < 0x8000UL && !(i & 1)) {
                cvga_store_plane(v, 0, i, 0x20);
                cvga_store_plane(v, 1, i, 0x07);
            } else {
                cvga_store_plane(v, 0, i, zero);
                cvga_store_plane(v, 1, i, zero);
            }
            if (!m->text) cvga_store_plane(v, 2, i, zero);
            cvga_store_plane(v, 3, i, zero);
        }
    }
    if (v->dac_mask != 0xff) v->dac_mask = 0xff;
    if (!(rd8(b, BDA + B_MODESET) & 8)) {
        count = cvbios_default_palette(m->palette, palette);
        for (i = 0; i < 256; ++i)
            if (i < count) dac_set(b, i, palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2]);
            else dac_set(b, i, 0, 0, 0);
    }
    wr8(b, BDA + B_MODE, (uint8_t)mode);
    wr16(b, BDA + B_COLS, m->columns);
    wr16(b, BDA + B_PAGE_SIZE, m->page_bytes);
    wr16(b, BDA + B_PAGE_START, 0);
    for (page = 0; page < 8; ++page) wr16(b, BDA + B_CURSOR + page * 2, 0);
    wr8(b, BDA + B_PAGE, 0);
    wr16(b, BDA + B_CRTC, 0x3d4);
    wr8(b, BDA + B_MSR, mode < 4 ? text_msr[mode] : 0x29);
    wr8(b, BDA + B_PAL, (uint8_t)(mode == 0x13 || !m->text ? 0x3f : 0x30));
    wr8(b, BDA + B_ROWS, (uint8_t)(m->rows - 1));
    wr16(b, BDA + B_CHEIGHT, m->char_height);
    wr8(b, BDA + B_CTL, (uint8_t)(0x60 | (rd8(b, BDA + B_CTL) & 0x1f) | (preserve ? 0x80 : 0)));
    wr8(b, BDA + B_DCC, 8);
    wr16(b, BDA + B_CURSOR_TYPE, 0x0607);
    if (m->text) {
        uint32_t source;
        unsigned height;
        if (rom_font(b, CVBIOS_FONT_8X16, &height, &source)) load_font(b, source, 0, 256, 0, 16);
    } else {
        font = b->font[m->char_height == 8 ? CVBIOS_FONT_8X8 :
                       m->char_height == 14 ? CVBIOS_FONT_8X14 : CVBIOS_FONT_8X16];
        if (font) set_vector(b, 0x43, font);
    }
    ++b->mode_sets;
    return 1;
}

static int alternate_select(cvbios *b, cvbios_regs *r)
{
    cvga_state *v = b->vga;
    uint8_t flags;
    switch (BL(r)) {
    case 0x10:
        flags = rd8(b, BDA + B_SWITCHES);
        set_bx(r, 0x0003);
        set_cx(r, ((unsigned)(flags >> 4) << 8) | (flags & 15u));
        return 1;
    case 0x30:
        flags = rd8(b, BDA + B_MODESET) & 0x6f;
        if (AL(r) == 0) flags |= 0x80;
        else if (AL(r) == 2) flags |= 0x10;
        else if (AL(r) != 1) return 0;
        wr8(b, BDA + B_MODESET, flags);
        break;
    case 0x31: case 0x33:
        flags = rd8(b, BDA + B_MODESET);
        {
            uint8_t bit = BL(r) == 0x31 ? 8 : 2;
            /* 31h: AL=1 disables default palette loading.
             * 33h: AL=0 enables gray-scale summing. */
            int set = BL(r) == 0x31 ? AL(r) != 0 : AL(r) == 0;
            wr8(b, BDA + B_MODESET, (uint8_t)(set ? flags | bit : flags & ~bit));
        }
        break;
    case 0x32:
        if (v->enable != (AL(r) ? 0 : 1)) {
            v->enable = (uint8_t)(AL(r) ? 0 : 1);
            ++v->changes;
            ++v->display_changes;
        }
        break;
    case 0x34:
        flags = rd8(b, BDA + B_CTL);
        wr8(b, BDA + B_CTL, (uint8_t)(AL(r) ? flags | 1 : flags & ~1u));
        break;
    case 0x36:
        flags = (uint8_t)(AL(r) ? v->seq[1] | 0x20 : v->seq[1] & ~0x20u);
        if (flags != v->seq[1]) {
            v->seq[1] = flags;
            ++v->changes;
            ++v->display_changes;
        }
        break;
    default:
        return 0;
    }
    set_al(r, 0x12);
    return 1;
}

static void write_string(cvbios *b, cvbios_regs *r)
{
    unsigned flags = AL(r), page = BH(r) & 7, count = r->ecx & 0xffff, i;
    unsigned saved = rd16(b, BDA + B_CURSOR + page * 2);
    uint32_t text = es_linear(r, r->ebp);
    int attribute = (int)BL(r);
    set_cursor(b, page, DH(r), DL(r));
    for (i = 0; i < count; ++i) {
        unsigned ch = rd8(b, text + ((flags & 2) ? i * 2 : i));
        if (flags & 2) attribute = rd8(b, text + i * 2 + 1);
        if (ch == 7 || ch == 8 || ch == 10 || ch == 13) teletype(b, page, ch, -1);
        else teletype(b, page, ch, attribute);
    }
    if (!(flags & 1)) set_cursor(b, page, saved >> 8, saved & 0xff);
}

int cvbios_int10(cvbios *b, cvbios_regs *r)
{
    const cvga_mode_params *m;
    unsigned page, pos, i, cols;
    int handled = 1;
    ++b->calls;
    m = current(b);
    switch (AH(r)) {
    case 0x00: handled = set_mode(b, r); break;
    case 0x01: set_cursor_shape(b, CH(r), CL(r)); break;
    case 0x02: set_cursor(b, BH(r), DH(r), DL(r)); break;
    case 0x03:
        set_cx(r, rd16(b, BDA + B_CURSOR_TYPE));
        set_dx(r, rd16(b, BDA + B_CURSOR + (BH(r) & 7) * 2));
        break;
    case 0x05: set_page(b, AL(r)); break;
    case 0x06: case 0x07:
        scroll(b, rd8(b, BDA + B_PAGE), AL(r), BH(r), CH(r), CL(r), DH(r), DL(r), AH(r) == 7);
        break;
    case 0x08:
        if (m && m->text) {
            pos = rd16(b, BDA + B_CURSOR + (BH(r) & 7) * 2);
            i = (unsigned)cell_address(b, m, BH(r) & 7, pos >> 8, pos & 0xff);
            set_ax(r, cvga_read_vram(b->vga, i) | ((unsigned)cvga_read_vram(b->vga, i + 1) << 8));
        } else set_ax(r, 0);
        break;
    case 0x09: case 0x0a:
        page = BH(r) & 7;
        pos = rd16(b, BDA + B_CURSOR + page * 2);
        cols = rd16(b, BDA + B_COLS);
        for (i = 0; i < (r->ecx & 0xffff); ++i) {
            unsigned col = (pos & 0xff) + i;
            unsigned row = (pos >> 8) + col / cols;
            if (m && m->text) write_cell(b, page, row, col % cols, AL(r),
                                         AH(r) == 9 ? (int)BL(r) : -1);
            else write_cell(b, page, row, col % cols, AL(r), (int)BL(r));
        }
        break;
    case 0x0b:
        if (!BH(r)) attr_set(b->vga, 17, (uint8_t)(BL(r) & 0x1f));
        break;
    case 0x0c:
        if (m && !m->text) pixel_put(b, m, BH(r) & 7, r->ecx & 0xffff, r->edx & 0xffff, AL(r));
        break;
    case 0x0d:
        set_al(r, m && !m->text ? pixel_get(b, m, BH(r) & 7, r->ecx & 0xffff, r->edx & 0xffff) : 0);
        break;
    case 0x0e: teletype(b, rd8(b, BDA + B_PAGE), AL(r), m && !m->text ? (int)BL(r) : -1); break;
    case 0x0f:
        set_ax(r, (rd8(b, BDA + B_MODE) | (rd8(b, BDA + B_CTL) & 0x80)) |
                  ((unsigned)rd8(b, BDA + B_COLS) << 8));
        r->ebx = (r->ebx & ~0xff00UL) | ((uint32_t)rd8(b, BDA + B_PAGE) << 8);
        break;
    case 0x10: handled = palette_services(b, r); break;
    case 0x11: handled = font_services(b, r); break;
    case 0x12: handled = alternate_select(b, r); break;
    case 0x13: write_string(b, r); break;
    case 0x1a:
        if (AL(r) == 0) {
            set_bx(r, rd8(b, BDA + B_DCC) == 8 ? 0x0008 : rd8(b, BDA + B_DCC));
            set_al(r, 0x1a);
        } else if (AL(r) == 1) {
            wr8(b, BDA + B_DCC, (uint8_t)BL(r));
            set_al(r, 0x1a);
        } else handled = 0;
        break;
    default:
        handled = 0;
        break;
    }
    if (!handled) {
        ++b->unsupported;
        b->last_unsupported = (uint16_t)r->eax;
    }
    return handled;
}
