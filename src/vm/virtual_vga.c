#include "virtual_vga.h"

/* The register bytes below describe standard VGA timings, not executable
 * firmware. Palette and font policy belongs to the virtual BIOS adapter. */
static const uint8_t mode03_crtc[25] = {
    0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0,0,0,0,
    0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff
};
static const uint8_t mode13_crtc[25] = {
    0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0x41,0,0,0,0,0,0,
    0x9c,0x8e,0x8f,0x28,0x40,0x96,0xb9,0xa3,0xff
};
static const uint8_t seq_mask[5] = {3,0x3d,15,0x3f,14};
static const uint8_t gc_mask[9] = {15,15,15,31,3,0x7b,15,15,255};

static void zero_bytes(uint8_t *p, uint32_t count)
{
    while (count--) *p++ = 0;
}

int cvga_set_bios_mode(cvga_state *v, unsigned mode, int preserve_vram)
{
    unsigned i, graphics;
    preserve_vram = preserve_vram || (mode & 0x80);
    mode &= ~0x80u;
    if (mode != 3 && mode != 0x13) return 0;
    graphics = mode == 0x13;
    if (!preserve_vram) zero_bytes(&v->plane[0][0], 4UL * CVGA_PLANE_SIZE);
    zero_bytes(v->latch, 4);
    zero_bytes(v->seq, 5);
    zero_bytes(v->gc, 9);
    zero_bytes(v->attr, 21);
    v->seq[0] = 3;
    v->seq[1] = graphics ? 1 : 0;
    v->seq[2] = graphics ? 15 : 3;
    v->seq[4] = graphics ? 14 : 2;
    v->gc[5] = graphics ? 0x40 : 0x10;
    v->gc[6] = graphics ? 5 : 14;
    v->gc[7] = 15;
    v->gc[8] = 255;
    for (i = 0; i != 25; ++i)
        v->crtc[i] = graphics ? mode13_crtc[i] : mode03_crtc[i];
    for (i = 0; i != 16; ++i)
        v->attr[i] = (uint8_t)(graphics ? i : (i < 8 ? i : i + 48));
    if (!graphics) v->attr[6] = 20;
    v->attr[16] = graphics ? 0x41 : 0x0c;
    v->attr[18] = 15;
    v->attr[19] = graphics ? 0 : 8;
    v->seq_index = v->gc_index = v->crtc_index = 0;
    v->attr_index = 0x20;
    v->attr_data_phase = 0;
    v->misc = 0x63;
    v->feature = 0;
    v->enable = 1;
    ++v->changes;
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
            v->plane[2][c * 32 + row] = row < 16 ? font4096[c * 16 + row] : 0;
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

void cvga_write_port(cvga_state *v, uint16_t port, uint8_t value)
{
    unsigned index;
    if (!port_active(v, port)) return;
    switch (port) {
    case 0x3c0:
        if (!v->attr_data_phase) v->attr_index = value & 63;
        else {
            index = v->attr_index & 31;
            if (index < 16) {
                if (!(v->attr_index & 32)) v->attr[index] = value & 63;
            } else if (index < 21) {
                if (index == 16) value &= 0xef;
                if (index == 18) value &= 63;
                if (index >= 19) value &= 15;
                v->attr[index] = value;
            }
        }
        v->attr_data_phase ^= 1;
        break;
    case 0x3c2: v->misc = value & 0xef; break;
    case 0x3c3: v->enable = value & 1; break;
    case 0x3c4: v->seq_index = value & 7; return;
    case 0x3c5:
        if (v->seq_index < 5) v->seq[v->seq_index] = value & seq_mask[v->seq_index];
        break;
    case 0x3c6: v->dac_mask = value; break;
    case 0x3c7: case 0x3c8:
        v->dac_index = value;
        v->dac_component = 0;
        v->dac_read_mode = port == 0x3c7;
        return;
    case 0x3c9:
        v->dac[v->dac_index][v->dac_component] = value & 63;
        dac_advance(v);
        break;
    case 0x3ce: v->gc_index = value & 15; return;
    case 0x3cf:
        if (v->gc_index < 9) v->gc[v->gc_index] = value & gc_mask[v->gc_index];
        break;
    case 0x3b4: case 0x3d4: v->crtc_index = value; return;
    case 0x3b5: case 0x3d5:
        index = v->crtc_index;
        if (index < 25) {
            if ((v->crtc[17] & 128) && index < 8) {
                if (index == 7) v->crtc[7] = (v->crtc[7] & 0xef) | (value & 16);
            } else v->crtc[index] = value;
        }
        break;
    case 0x3ba: case 0x3da: v->feature = value & 0x0b; return;
    default: return;
    }
    ++v->changes;
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

uint8_t cvga_read_vram(cvga_state *v, uint32_t address)
{
    uint32_t offset;
    unsigned at, plane, i;
    uint8_t result = 255;
    if (!aperture(v, address, &offset)) return 255;
    at = memory_address(v, offset);
    for (i = 0; i != 4; ++i) v->latch[i] = v->plane[i][at];
    if (v->gc[5] & 8) {
        for (i = 0; i != 4; ++i)
            if (v->gc[7] & (1u << i))
                result &= (uint8_t)(v->latch[i] ^ ((v->gc[2] & (1u << i)) ? 0 : 255));
        return result;
    }
    plane = v->gc[4];
    if (v->seq[4] & 8) plane = (unsigned)(offset & 3);
    else if (v->gc[5] & 16) plane = (plane & 2) | (unsigned)(offset & 1);
    return v->latch[plane];
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
        v->plane[i][at] = data;
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

unsigned cvga_render_row8(const cvga_state *v, unsigned y, uint8_t *dest,
                          unsigned capacity, unsigned frame_flags)
{
    cvga_geometry g;
    unsigned x, plane, shift, row, subrow, physical_y, compare, base, pan, at, colour;
    if (!cvga_get_geometry(v, &g) || y >= g.height || capacity < g.width) return 0;
    if (g.blank) {
        for (x = 0; x < g.width; ++x) dest[x] = 0;
        return g.width;
    }
    shift = (v->gc[5] >> 5) & 3;
    physical_y = y * g.scan_repeat;
    compare = v->crtc[24] | ((unsigned)(v->crtc[7] & 16) << 4) |
              ((unsigned)(v->crtc[9] & 64) << 3);
    base = ((unsigned)v->crtc[12] << 8) | v->crtc[13];
    pan = v->attr[19] & 7;
    if (g.text && g.char_width == 9) pan = v->attr[19] == 8 ? 0 : pan + 1;
    if (!g.text && shift >= 2) pan >>= 1;
    if (physical_y > compare) {
        physical_y -= compare + 1;
        base = 0;
        if (v->attr[16] & 32) pan = 0;
        subrow = 0;
    } else subrow = v->crtc[8] & 31;
    row = physical_y / (1u << ((v->crtc[9] >> 7) & 1)) + subrow;
    subrow = row % g.char_height;
    row /= g.char_height;
    base += row * (unsigned)v->crtc[19] * 2;
    for (x = 0; x < g.width; ++x) {
        if (g.text) dest[x] = text_pixel(v, &g, base, x + pan, subrow, frame_flags);
        else if (shift >= 2) {
            at = scan_address(v, base + (x + pan) / 4);
            dest[x] = v->plane[(x + pan) & 3][at] & v->dac_mask;
        } else {
            at = scan_address(v, base + (x + pan) / 8);
            colour = 0;
            for (plane = 0; plane < 4; ++plane)
                if (v->plane[plane][at] & (128u >> ((x + pan) & 7))) colour |= 1u << plane;
            dest[x] = attr_colour(v, colour);
        }
    }
    return g.width;
}
