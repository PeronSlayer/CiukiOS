#include "virtual_vga.h"

/* Standard IBM VGA video-parameter-table register values. These are timing
 * and addressing presets, not executable firmware. Palette and font policy
 * belongs to the virtual BIOS adapter. scripts/test_virtual_vga.py checks each
 * array byte-for-byte against QEMU's SeaVGABIOS ROM when it is installed. */
static const cvga_mode_params modes[] = {
    {0x01, 1, 40, 25, 16, 2, 0x0800, 0xb800, 360, 400,
     {0x08,0x03,0x00,0x02}, 0x67,
     {0x2d,0x27,0x28,0x90,0x2b,0xa0,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x00,
      0x9c,0x8e,0x8f,0x14,0x1f,0x96,0xb9,0xa3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
      0x0c,0x00,0x0f,0x08},
     {0x00,0x00,0x00,0x00,0x00,0x10,0x0e,0x0f,0xff}},
    {0x03, 1, 80, 25, 16, 2, 0x1000, 0xb800, 720, 400,
     {0x00,0x03,0x00,0x02}, 0x67,
     {0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x00,
      0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
      0x0c,0x00,0x0f,0x08},
     {0x00,0x00,0x00,0x00,0x00,0x10,0x0e,0x0f,0xff}},
    {0x0d, 0, 40, 25, 8, 1, 0x2000, 0xa000, 320, 200,
     {0x09,0x0f,0x00,0x06}, 0x63,
     {0x2d,0x27,0x28,0x90,0x2b,0x80,0xbf,0x1f,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,
      0x9c,0x8e,0x8f,0x14,0x00,0x96,0xb9,0xe3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
      0x01,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff}},
    {0x0e, 0, 80, 25, 8, 1, 0x4000, 0xa000, 640, 200,
     {0x01,0x0f,0x00,0x06}, 0x63,
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,
      0x9c,0x8e,0x8f,0x28,0x00,0x96,0xb9,0xe3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
      0x01,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff}},
    {0x10, 0, 80, 25, 14, 2, 0x8000, 0xa000, 640, 350,
     {0x01,0x0f,0x00,0x06}, 0xa3,
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
      0x83,0x85,0x5d,0x28,0x0f,0x63,0xba,0xe3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
      0x01,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff}},
    {0x11, 0, 80, 30, 16, 2, 0xa000, 0xa000, 640, 480,
     {0x01,0x0f,0x00,0x06}, 0xe3,
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
      0xea,0x8c,0xdf,0x28,0x00,0xe7,0x04,0xe3,0xff},
     {0x00,0x3f,0x00,0x3f,0x00,0x3f,0x00,0x3f,0x00,0x3f,0x00,0x3f,0x00,0x3f,0x00,0x3f,
      0x01,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff}},
    {0x12, 0, 80, 30, 16, 2, 0xa000, 0xa000, 640, 480,
     {0x01,0x0f,0x00,0x06}, 0xe3,
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
      0xea,0x8c,0xdf,0x28,0x00,0xe7,0x04,0xe3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
      0x01,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff}},
    {0x13, 0, 40, 25, 8, 3, 0xfa00, 0xa000, 320, 200,
     {0x01,0x0f,0x00,0x0e}, 0x63,
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
      0x9c,0x8e,0x8f,0x28,0x40,0x96,0xb9,0xa3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
      0x41,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0f,0xff}},
};
static const uint8_t seq_mask[5] = {3,0x3d,15,0x3f,14};
static const uint8_t gc_mask[9] = {15,15,15,31,3,0x7b,15,15,255};

static void zero_bytes(uint8_t *p, uint32_t count)
{
    while (count--) *p++ = 0;
}

static void fill_bytes(uint8_t *p, uint8_t value, uint32_t count)
{
    while (count--) *p++ = value;
}

static void mark_dirty(cvga_state *v, unsigned plane, unsigned at)
{
    at /= CVGA_DIRTY_GRANULE;
    v->dirty[plane][at >> 3] |= (uint8_t)(1u << (at & 7));
}

static void store_plane(cvga_state *v, unsigned plane, unsigned at, uint8_t value)
{
    if (v->plane[plane][at] == value) return;
    v->plane[plane][at] = value;
    mark_dirty(v, plane, at);
}

void cvga_store_plane(cvga_state *v, unsigned plane, unsigned at, uint8_t value)
{
    store_plane(v, plane & 3, at & 0xffffu, value);
    ++v->changes;
}

const cvga_mode_params *cvga_find_mode(unsigned mode)
{
    unsigned i;
    mode &= 0x7f;
    /* Colour-burst variants 00h/02h share the 01h/03h VGA register values. */
    if (mode == 0 || mode == 2) ++mode;
    for (i = 0; i != sizeof(modes) / sizeof(modes[0]); ++i)
        if (modes[i].mode == mode) return &modes[i];
    return 0;
}

int cvga_set_bios_mode(cvga_state *v, unsigned mode, int preserve_vram)
{
    unsigned i;
    const cvga_mode_params *m = cvga_find_mode(mode);
    if (!m || (mode & ~0x80u) > 0x13) return 0;
    preserve_vram = preserve_vram || (mode & 0x80);
    if (!preserve_vram) {
        zero_bytes(&v->plane[0][0], 4UL * CVGA_PLANE_SIZE);
        fill_bytes(&v->dirty[0][0], 0xff, sizeof(v->dirty));
    }
    zero_bytes(v->latch, 4);
    v->seq[0] = 3;
    for (i = 0; i != 4; ++i) v->seq[i + 1] = m->seq[i];
    for (i = 0; i != 9; ++i) v->gc[i] = m->gc[i];
    for (i = 0; i != 25; ++i) v->crtc[i] = m->crtc[i];
    for (i = 0; i != 20; ++i) v->attr[i] = m->attr[i];
    v->attr[20] = 0;
    v->seq_index = v->gc_index = v->crtc_index = 0;
    v->attr_index = 0x20;
    v->attr_data_phase = 0;
    v->misc = m->misc;
    v->feature = 0;
    v->enable = 1;
    ++v->changes;
    ++v->display_changes;
    return 1;
}

void cvga_init(cvga_state *v)
{
    zero_bytes((uint8_t *)v, (uint32_t)sizeof(*v));
    v->dac_mask = 255;
    cvga_set_bios_mode(v, 3, 1);
}

void cvga_load_font_8x16(cvga_state *v, const uint8_t *font4096)
{
    unsigned c, row;
    for (c = 0; c < 256; ++c)
        for (row = 0; row < 32; ++row)
            store_plane(v, 2, c * 32 + row, row < 16 ? font4096[c * 16 + row] : 0);
    ++v->changes;
}

static int port_active(const cvga_state *v, uint16_t port)
{
    if ((port & 0xfff0) == 0x3b0) return !(v->misc & 1);
    if ((port & 0xfff0) == 0x3d0) return !!(v->misc & 1);
    return 1;
}

static void dac_advance(cvga_state *v)
{
    if (++v->dac_component == 3) {
        v->dac_component = 0;
        ++v->dac_index;
    }
}

uint8_t cvga_read_port(cvga_state *v, uint16_t port, uint8_t status1)
{
    uint8_t value;
    if (!port_active(v, port)) return 255;
    switch (port) {
    case 0x3c0: return v->attr_index;
    case 0x3c1:
        return (v->attr_index & 31) < 21 ? v->attr[v->attr_index & 31] : 255;
    case 0x3c2: return 0x10; /* fixed switch sense; no invented IRQ status */
    case 0x3c3: return v->enable;
    case 0x3c4: return v->seq_index;
    case 0x3c5: return v->seq_index < 5 ? v->seq[v->seq_index] : 255;
    case 0x3c6: return v->dac_mask;
    case 0x3c7: return v->dac_read_mode ? 3 : 0;
    case 0x3c8: return (uint8_t)(v->dac_index + v->dac_read_mode);
    case 0x3c9:
        value = v->dac[v->dac_index][v->dac_component];
        dac_advance(v);
        return value;
    case 0x3ca: return v->feature;
    case 0x3cc: return v->misc;
    case 0x3ce: return v->gc_index;
    case 0x3cf: return v->gc_index < 9 ? v->gc[v->gc_index] : 255;
    case 0x3b4: case 0x3d4: return v->crtc_index;
    case 0x3b5: case 0x3d5: return v->crtc_index < 25 ? v->crtc[v->crtc_index] : 255;
    case 0x3ba: case 0x3da:
        v->attr_data_phase = 0;
        return status1 & 9;
    default: return 255;
    }
}

/* Store a register byte; returns 1 when scanout-visible state changed. */
static int set_reg(uint8_t *reg, uint8_t value)
{
    if (*reg == value) return 0;
    *reg = value;
    return 1;
}

void cvga_write_port(cvga_state *v, uint16_t port, uint8_t value)
{
    unsigned index;
    int display = 0;
    if (!port_active(v, port)) return;
    switch (port) {
    case 0x3c0:
        if (!v->attr_data_phase) {
            /* Palette address source (bit 5) enables or blanks the display. */
            display = ((v->attr_index ^ value) & 32) != 0;
            v->attr_index = value & 63;
        } else {
            index = v->attr_index & 31;
            if (index < 16) {
                if (!(v->attr_index & 32)) display = set_reg(&v->attr[index], value & 63);
            } else if (index < 21) {
                if (index == 16) value &= 0xef;
                if (index == 18) value &= 63;
                if (index >= 19) value &= 15;
                display = set_reg(&v->attr[index], value);
            }
        }
        v->attr_data_phase ^= 1;
        break;
    case 0x3c2: display = set_reg(&v->misc, value & 0xef); break;
    case 0x3c3: display = set_reg(&v->enable, value & 1); break;
    case 0x3c4: v->seq_index = value & 7; return;
    case 0x3c5:
        if (v->seq_index < 5) {
            index = v->seq_index;
            if (set_reg(&v->seq[index], value & seq_mask[index]) &&
                (index == 0 || index == 1 || index == 3)) display = 1;
        }
        break;
    case 0x3c6: display = set_reg(&v->dac_mask, value); break;
    case 0x3c7: case 0x3c8:
        v->dac_index = value;
        v->dac_component = 0;
        v->dac_read_mode = port == 0x3c7;
        return;
    case 0x3c9:
        display = set_reg(&v->dac[v->dac_index][v->dac_component], value & 63);
        dac_advance(v);
        break;
    case 0x3ce: v->gc_index = value & 15; return;
    case 0x3cf:
        if (v->gc_index < 9) {
            /* Scanout reads only GC05 bits 5-6 (shift/256-colour) and GC06
             * bit 0 (graphics); write/read modes and memory maps do not. */
            uint8_t old;
            index = v->gc_index;
            old = v->gc[index];
            if (set_reg(&v->gc[index], value & gc_mask[index]) &&
                ((index == 5 && ((old ^ v->gc[5]) & 0x60)) ||
                 (index == 6 && ((old ^ v->gc[6]) & 0x01))))
                display = 1;
        }
        break;
    case 0x3b4: case 0x3d4: v->crtc_index = value; return;
    case 0x3b5: case 0x3d5:
        index = v->crtc_index;
        if (index < 25) {
            if ((v->crtc[17] & 128) && index < 8) {
                if (index == 7)
                    display = set_reg(&v->crtc[7], (uint8_t)((v->crtc[7] & 0xef) | (value & 16)));
            } else display = set_reg(&v->crtc[index], value);
        }
        break;
    case 0x3ba: case 0x3da: v->feature = value & 0x0b; return;
    default: return;
    }
    ++v->changes;
    if (display) ++v->display_changes;
}

static int aperture(const cvga_state *v, uint32_t address, uint32_t *offset)
{
    uint32_t base, size;
    if (!(v->misc & 2) || !(v->seq[4] & 2) || !v->enable) return 0;
    switch ((v->gc[6] >> 2) & 3) {
    case 0: base = 0xa0000UL; size = 0x20000UL; break;
    case 1: base = 0xa0000UL; size = 0x10000UL; break;
    case 2: base = 0xb0000UL; size = 0x08000UL; break;
    default: base = 0xb8000UL; size = 0x08000UL; break;
    }
    if (address < base || address - base >= size) return 0;
    *offset = address - base;
    return 1;
}

static unsigned memory_address(const cvga_state *v, uint32_t offset)
{
    if (v->seq[4] & 8)
        return (unsigned)(offset & 0xfffcUL);
    if (v->gc[6] & 2)
        return (unsigned)((offset & 0xfffeUL) | ((offset >> 16) & 1));
    return (unsigned)(offset & 0xffffUL);
}

/* Common read path. latch == 0 means a side-effect-free peek. */
static uint8_t read_vram(const cvga_state *v, uint32_t address, uint8_t *latch)
{
    uint32_t offset;
    unsigned at, plane, i;
    uint8_t loaded[4], result = 255;
    if (!aperture(v, address, &offset)) return 255;
    at = memory_address(v, offset);
    for (i = 0; i != 4; ++i) loaded[i] = v->plane[i][at];
    if (latch) for (i = 0; i != 4; ++i) latch[i] = loaded[i];
    if (v->gc[5] & 8) {
        for (i = 0; i != 4; ++i)
            if (v->gc[7] & (1u << i))
                result &= (uint8_t)(loaded[i] ^ ((v->gc[2] & (1u << i)) ? 0 : 255));
        return result;
    }
    plane = v->gc[4];
    if (v->seq[4] & 8) plane = (unsigned)(offset & 3);
    else if (v->gc[5] & 16) plane = (plane & 2) | (unsigned)(offset & 1);
    return loaded[plane];
}

uint8_t cvga_read_vram(cvga_state *v, uint32_t address)
{
    return read_vram(v, address, v->latch);
}

uint8_t cvga_peek_vram(const cvga_state *v, uint32_t address)
{
    return read_vram(v, address, 0);
}

void cvga_write_vram(cvga_state *v, uint32_t address, uint8_t value)
{
    uint32_t offset;
    unsigned at, i, mask, mode, rotate, operation;
    uint8_t rotated, bits, data;
    if (!aperture(v, address, &offset)) return;
    at = memory_address(v, offset);
    mask = v->seq[2];
    if (v->seq[4] & 8) mask &= 1u << (offset & 3);
    else if (!(v->seq[4] & 4)) mask &= (offset & 1) ? 10 : 5;
    mode = v->gc[5] & 3;
    rotate = v->gc[3] & 7;
    rotated = (uint8_t)((value >> rotate) | ((unsigned)value << ((8 - rotate) & 7)));
    operation = (v->gc[3] >> 3) & 3;
    for (i = 0; i != 4; ++i) {
        if (!(mask & (1u << i))) continue;
        bits = v->gc[8];
        if (mode == 1) data = v->latch[i];
        else {
            if (mode == 2) data = (value & (1u << i)) ? 255 : 0;
            else if (mode == 3) {
                data = (v->gc[0] & (1u << i)) ? 255 : 0;
                bits &= rotated;
            } else if (v->gc[1] & (1u << i)) data = (v->gc[0] & (1u << i)) ? 255 : 0;
            else data = rotated;
            switch (operation) {
            case 1: data &= v->latch[i]; break;
            case 2: data |= v->latch[i]; break;
            case 3: data ^= v->latch[i]; break;
            default: break;
            }
            data = (uint8_t)((data & bits) | (v->latch[i] & (uint8_t)~bits));
        }
        store_plane(v, i, at, data);
    }
    if (mask) ++v->changes;
}

int cvga_get_geometry(const cvga_state *v, cvga_geometry *g)
{
    unsigned lines, shift, compare;
    if ((v->crtc[23] & 3) != 3 || (v->seq[1] & 0x14) ||
        (v->crtc[23] & 8) || (v->crtc[20] & 32) || (v->crtc[8] & 0x60)) return 0;
    shift = (v->gc[5] >> 5) & 3;
    if (shift == 1) return 0;
    g->text = !(v->gc[6] & 1);
    if (g->text && shift) return 0;
    g->blank = !(v->attr_index & 32) || (v->seq[1] & 32) || !v->enable ||
               (v->seq[0] & 3) != 3 || !(v->crtc[23] & 128);
    g->char_width = (v->seq[1] & 1) ? 8 : 9;
    g->char_height = (v->crtc[9] & 31) + 1;
    lines = v->crtc[18] | ((unsigned)(v->crtc[7] & 2) << 7) |
            ((unsigned)(v->crtc[7] & 64) << 3);
    ++lines;
    g->scan_repeat = 1u << ((v->crtc[9] >> 7) & 1);
    if (!g->text) g->scan_repeat *= g->char_height;
    compare = v->crtc[24] | ((unsigned)(v->crtc[7] & 16) << 4) |
              ((unsigned)(v->crtc[9] & 64) << 3);
    /* Collapse repeated rows only when every displayed row remains identical.
     * A split/preset in the middle of a repeated group needs physical rows. */
    if ((v->crtc[8] & 31) || (compare < lines && (compare + 1) % g->scan_repeat))
        g->scan_repeat = 1;
    g->height = (lines + g->scan_repeat - 1) / g->scan_repeat;
    g->width = ((unsigned)v->crtc[1] + 1) * (g->text ? g->char_width : 8);
    if (!g->text && shift >= 2) g->width /= 2;
    return 1;
}

static unsigned scan_address(const cvga_state *v, unsigned logical)
{
    logical &= 65535u;
    if (v->crtc[20] & 64) return (logical << 2) & 65532u;
    if (!(v->crtc[23] & 64))
        return ((logical << 1) & 65534u) | ((logical >> ((v->crtc[23] & 32) ? 15 : 13)) & 1);
    return logical;
}

static uint8_t attr_colour(const cvga_state *v, unsigned colour)
{
    unsigned index = v->attr[colour & v->attr[18] & 15];
    if (v->attr[16] & 128) index = (index & 15) | ((v->attr[20] & 3) << 4);
    index |= (v->attr[20] & 12) << 4;
    return (uint8_t)(index & v->dac_mask);
}

static uint8_t text_pixel(const cvga_state *v, const cvga_geometry *g,
                          unsigned base, unsigned x, unsigned glyph_row,
                          unsigned flags)
{
    unsigned cell, at, ch, attribute, font_a, font_b, bank, dot, fg, bg, on;
    cell = (base + x / g->char_width) & 65535u;
    at = scan_address(v, cell);
    ch = v->plane[0][at];
    attribute = v->plane[1][at];
    font_a = ((v->seq[3] >> 2) & 3) * 2 + ((v->seq[3] >> 5) & 1);
    font_b = (v->seq[3] & 3) * 2 + ((v->seq[3] >> 4) & 1);
    bank = (attribute & 8) ? font_a : font_b;
    dot = x % g->char_width;
    on = v->plane[2][bank * 8192 + ch * 32 + glyph_row];
    if (dot < 8) on &= 128u >> dot;
    else on = ((v->attr[16] & 4) && ch >= 0xc0 && ch <= 0xdf) ? (on & 1) : 0;
    if ((attribute & 0x77) == 1 && glyph_row == (v->crtc[20] & 31)) on = 1;
    if ((v->attr[16] & 8) && (attribute & 128) && !(flags & CVGA_BLINK_VISIBLE)) on = 0;
    if ((flags & CVGA_CURSOR_VISIBLE) && !(v->crtc[10] & 32) &&
        cell == (((unsigned)v->crtc[14] << 8) | v->crtc[15]) &&
        glyph_row >= (v->crtc[10] & 31) && glyph_row <= (v->crtc[11] & 31)) on = 1;
    fg = attribute & (font_a == font_b ? 15 : 7);
    bg = (attribute >> 4) & ((v->attr[16] & 8) ? 7 : 15);
    return attr_colour(v, on ? fg : bg);
}

/* Scanout addressing of one returned geometry row, shared by rendering and
 * damage checks so both always agree on which plane bytes a row reads. */
typedef struct row_setup {
    cvga_geometry g;
    unsigned shift, base, pan, subrow;
} row_setup;

static int setup_row(const cvga_state *v, unsigned y, row_setup *r)
{
    unsigned row, physical_y, compare, subrow;
    if (!cvga_get_geometry(v, &r->g) || y >= r->g.height) return 0;
    r->shift = (v->gc[5] >> 5) & 3;
    physical_y = y * r->g.scan_repeat;
    compare = v->crtc[24] | ((unsigned)(v->crtc[7] & 16) << 4) |
              ((unsigned)(v->crtc[9] & 64) << 3);
    r->base = ((unsigned)v->crtc[12] << 8) | v->crtc[13];
    r->pan = v->attr[19] & 7;
    if (r->g.text && r->g.char_width == 9) r->pan = v->attr[19] == 8 ? 0 : r->pan + 1;
    if (!r->g.text && r->shift >= 2) r->pan >>= 1;
    if (physical_y > compare) {
        physical_y -= compare + 1;
        r->base = 0;
        if (v->attr[16] & 32) r->pan = 0;
        subrow = 0;
    } else subrow = v->crtc[8] & 31;
    row = physical_y / (1u << ((v->crtc[9] >> 7) & 1)) + subrow;
    r->subrow = row % r->g.char_height;
    row /= r->g.char_height;
    r->base += row * (unsigned)v->crtc[19] * 2;
    return 1;
}

unsigned cvga_render_row8(const cvga_state *v, unsigned y, uint8_t *dest,
                          unsigned capacity, unsigned frame_flags)
{
    row_setup r;
    unsigned x, plane, at, colour;
    if (!setup_row(v, y, &r) || capacity < r.g.width) return 0;
    if (r.g.blank) {
        for (x = 0; x < r.g.width; ++x) dest[x] = 0;
        return r.g.width;
    }
    for (x = 0; x < r.g.width; ++x) {
        if (r.g.text) dest[x] = text_pixel(v, &r.g, r.base, x + r.pan, r.subrow, frame_flags);
        else if (r.shift >= 2) {
            at = scan_address(v, r.base + (x + r.pan) / 4);
            dest[x] = v->plane[(x + r.pan) & 3][at] & v->dac_mask;
        } else {
            at = scan_address(v, r.base + (x + r.pan) / 8);
            colour = 0;
            for (plane = 0; plane < 4; ++plane)
                if (v->plane[plane][at] & (128u >> ((x + r.pan) & 7))) colour |= 1u << plane;
            dest[x] = attr_colour(v, colour);
        }
    }
    return r.g.width;
}

int cvga_row_reads_dirty(const cvga_state *v, unsigned y,
                         const uint8_t dirty[4][CVGA_DIRTY_BYTES])
{
    row_setup r;
    unsigned unit, first, last, at, granule, plane;
    if (!setup_row(v, y, &r)) return 1;
    if (r.g.blank) return 0;
    if (r.g.text) {
        first = r.pan / r.g.char_width;
        last = (r.g.width - 1 + r.pan) / r.g.char_width;
    } else if (r.shift >= 2) {
        first = r.pan / 4;
        last = (r.g.width - 1 + r.pan) / 4;
    } else {
        first = r.pan / 8;
        last = (r.g.width - 1 + r.pan) / 8;
    }
    for (unit = first; unit <= last; ++unit) {
        at = scan_address(v, r.base + unit);
        granule = at / CVGA_DIRTY_GRANULE;
        for (plane = 0; plane < 4; ++plane) {
            /* Text cells live in planes 0/1; font dependencies are separate. */
            if (r.g.text && plane >= 2) break;
            if (dirty[plane][granule >> 3] & (1u << (granule & 7))) return 1;
        }
    }
    return 0;
}

int cvga_row_phase_sensitive(const cvga_state *v, unsigned y)
{
    row_setup r;
    unsigned unit, first, last, cell, at, cursor;
    int cursor_row;
    if (!setup_row(v, y, &r) || !r.g.text || r.g.blank) return 0;
    cursor = ((unsigned)v->crtc[14] << 8) | v->crtc[15];
    cursor_row = !(v->crtc[10] & 32) && r.subrow >= (v->crtc[10] & 31u) &&
                 r.subrow <= (v->crtc[11] & 31u);
    first = r.pan / r.g.char_width;
    last = (r.g.width - 1 + r.pan) / r.g.char_width;
    for (unit = first; unit <= last; ++unit) {
        cell = (r.base + unit) & 65535u;
        at = scan_address(v, cell);
        if ((v->attr[16] & 8) && (v->plane[1][at] & 128)) return 1;
        if (cursor_row && cell == cursor) return 1;
    }
    return 0;
}
