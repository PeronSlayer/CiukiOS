/* Independent expected vectors for the standalone software VGA. Not a guest
 * acceptance test: this does not execute DOS code or establish VM isolation. */
#include "virtual_vga.h"
#include "virtual_vga_bios.h"
#include "vga_presenter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cvga_state v;
static unsigned checks;
static void expect(unsigned actual, unsigned wanted, const char *what, unsigned line)
{
    ++checks;
    if (actual != wanted) {
        fprintf(stderr, "line %u: %s got %u, wanted %u\n", line, what, actual, wanted);
        exit(1);
    }
}
#define EQ(a,b) expect((unsigned)(a), (unsigned)(b), #a, __LINE__)
static void reg(unsigned port, unsigned index, unsigned value)
{
    cvga_write_port(&v, (uint16_t)port, (uint8_t)index);
    cvga_write_port(&v, (uint16_t)(port + 1), (uint8_t)value);
}
static void seq(unsigned index, unsigned value) { reg(0x3c4, index, value); }
static void gc(unsigned index, unsigned value) { reg(0x3ce, index, value); }
static void cr(unsigned index, unsigned value) { reg(0x3d4, index, value); }
static void ar(unsigned index, unsigned value)
{
    cvga_read_port(&v, 0x3da, 0);
    cvga_write_port(&v, 0x3c0, (uint8_t)index);
    cvga_write_port(&v, 0x3c0, (uint8_t)value);
    cvga_read_port(&v, 0x3da, 0);
    cvga_write_port(&v, 0x3c0, 0x20);
}
static void unchained(void)
{
    EQ(cvga_set_bios_mode(&v, 0x13, 0), 1);
    seq(4, 6);
    cr(20, 0);
    cr(23, 0xe3);
}
static void plane_write(unsigned plane, unsigned address, unsigned value)
{
    seq(2, 1u << plane);
    cvga_write_vram(&v, 0xa0000UL + address, (uint8_t)value);
}
static unsigned plane_read(unsigned plane, unsigned address)
{
    gc(4, plane);
    return cvga_read_vram(&v, 0xa0000UL + address);
}

static void test_registers(void)
{
    unsigned i;
    cvga_init(&v);
    cvga_write_port(&v, 0x3c8, 255);
    cvga_write_port(&v, 0x3c9, 255);
    cvga_write_port(&v, 0x3c9, 17);
    EQ(cvga_read_port(&v, 0x3c8, 0), 255);
    cvga_write_port(&v, 0x3c9, 32);
    EQ(cvga_read_port(&v, 0x3c8, 0), 0);
    cvga_write_port(&v, 0x3c9, 4);
    cvga_write_port(&v, 0x3c9, 5);
    cvga_write_port(&v, 0x3c9, 6);
    cvga_write_port(&v, 0x3c7, 255);
    EQ(cvga_read_port(&v, 0x3c7, 0), 3);
    EQ(cvga_read_port(&v, 0x3c8, 0), 0); /* read-mode visible index is save + 1 */
    EQ(cvga_read_port(&v, 0x3c9, 0), 63);
    EQ(cvga_read_port(&v, 0x3c9, 0), 17);
    EQ(cvga_read_port(&v, 0x3c9, 0), 32);
    EQ(cvga_read_port(&v, 0x3c8, 0), 1);
    EQ(cvga_read_port(&v, 0x3c9, 0), 4);
    EQ(cvga_read_port(&v, 0x3c9, 0), 5);
    EQ(cvga_read_port(&v, 0x3c9, 0), 6);
    cvga_write_port(&v, 0x3c8, 0);
    EQ(cvga_read_port(&v, 0x3c7, 0), 0);
    cvga_write_port(&v, 0x3c6, 0x7f);
    EQ(cvga_read_port(&v, 0x3c6, 0), 0x7f);
    ar(3, 0x2a);
    cvga_read_port(&v, 0x3da, 0);
    cvga_write_port(&v, 0x3c0, 0x23);
    cvga_write_port(&v, 0x3c0, 7); /* PAS locks palette */
    EQ(cvga_read_port(&v, 0x3c1, 0), 0x2a);
    cvga_read_port(&v, 0x3da, 0);
    cvga_write_port(&v, 0x3c0, 0x12);
    EQ(cvga_read_port(&v, 0x3ba, 9), 255); /* inactive mono port */
    cvga_write_port(&v, 0x3c0, 5);
    EQ(cvga_read_port(&v, 0x3c1, 0), 5);
    for (i = 0; i < 100; ++i) EQ(cvga_read_port(&v, 0x3da, 9), 9);
    EQ(cvga_read_port(&v, 0x3da, 0), 0); /* polling never fabricates retrace */
    cr(0, 0x11); EQ(v.crtc[0], 0x5f); /* CRTC protection */
    cr(7, 0); EQ(v.crtc[7], 0x0f); /* only line-compare bit writable */
    cr(17, 0x0e); cr(0, 0x11); EQ(v.crtc[0], 0x11);
    seq(2, 255); EQ(cvga_read_port(&v, 0x3c5, 0), 15);
    gc(4, 255); EQ(cvga_read_port(&v, 0x3cf, 0), 3);
    cr(255, 0x55); EQ(cvga_read_port(&v, 0x3d5, 0), 255);
    EQ(cvga_set_bios_mode(&v, 0x04, 0), 0);
    EQ(v.crtc[0], 0x11); /* unsupported preset is transactional */
}

static void test_latches(void)
{
    static const unsigned source[4] = {0x0f,0x33,0x55,0xaa};
    static const unsigned mode0[4] = {0xff,0x73,0x55,0xea};
    static const unsigned mode2[4] = {0x0f,0x3f,0x55,0xbe};
    static const unsigned mode3[4] = {0x0f,0x33,0x5d,0xa2};
    unsigned p;
    unchained();
    for (p = 0; p < 4; ++p) plane_write(p, 0x100, source[p]);
    plane_read(0, 0x100); seq(2, 15);
    gc(0, 1); gc(1, 5); gc(3, 25); gc(8, 0xf0);
    cvga_write_vram(&v, 0xa0200UL, 0x96);
    /* Mode 1 must copy the original read latches, not preceding write output. */
    gc(5, 0x41); gc(8, 0); gc(3, 31);
    cvga_write_vram(&v, 0xa0201UL, 0);
    gc(5, 0x40);
    for (p = 0; p < 4; ++p) {
        EQ(plane_read(p, 0x200), mode0[p]);
        EQ(plane_read(p, 0x201), source[p]);
    }
    plane_read(0, 0x100);
    gc(5, 0x42); gc(3, 16); gc(8, 0x3c);
    cvga_write_vram(&v, 0xa0202UL, 0x0a);
    gc(5, 0x40);
    for (p = 0; p < 4; ++p) EQ(plane_read(p, 0x202), mode2[p]);
    plane_read(0, 0x100);
    gc(5, 0x43); gc(3, 4); gc(0, 5); gc(1, 0); gc(8, 0x3c);
    cvga_write_vram(&v, 0xa0203UL, 0xf0);
    gc(5, 0x40);
    for (p = 0; p < 4; ++p) EQ(plane_read(p, 0x203), mode3[p]);
    plane_read(0, 0x100); gc(5, 0x41); seq(2, 5);
    cvga_write_vram(&v, 0xa0204UL, 0xff);
    gc(5, 0x40);
    EQ(plane_read(0, 0x204), 0x0f); EQ(plane_read(1, 0x204), 0);
    EQ(plane_read(2, 0x204), 0x55); EQ(plane_read(3, 0x204), 0);
    gc(0, 0); gc(1, 0); gc(3, 0); gc(8, 255);
    plane_write(0, 0x300, 0xaa); plane_write(1, 0x300, 0xcc);
    plane_write(2, 0x300, 0xf0); plane_write(3, 0x300, 0);
    gc(5, 0x48); gc(2, 5); gc(7, 15);
    EQ(cvga_read_vram(&v, 0xa0300UL), 0x20);
    gc(7, 5); EQ(cvga_read_vram(&v, 0xa0300UL), 0xa0);
    gc(2, 0); gc(7, 1); EQ(cvga_read_vram(&v, 0xa0300UL), 0x55);
    gc(7, 0); EQ(cvga_read_vram(&v, 0xa0300UL), 255);
}

static void test_apertures(void)
{
    cvga_init(&v);
    cvga_write_vram(&v, 0xb8000UL, 65);
    cvga_write_vram(&v, 0xb8001UL, 7);
    cvga_write_vram(&v, 0xb8002UL, 66);
    cvga_write_vram(&v, 0xb8003UL, 0x1f);
    EQ(cvga_read_vram(&v, 0xb8000UL), 65);
    EQ(cvga_read_vram(&v, 0xb8001UL), 7);
    EQ(cvga_read_vram(&v, 0xb8002UL), 66);
    EQ(cvga_read_vram(&v, 0xb8003UL), 0x1f);
    EQ(v.plane[0][0], 65); EQ(v.plane[1][0], 7);
    EQ(v.plane[0][2], 66); EQ(v.plane[1][2], 0x1f);
    EQ(cvga_read_vram(&v, 0xb7fffUL), 255);
    EQ(cvga_read_vram(&v, 0xc0000UL), 255);
    cvga_write_vram(&v, 0x1b8000UL, 0xff);
    EQ(cvga_read_vram(&v, 0xb8000UL), 65);
    gc(6, 10); /* mono window */
    EQ(cvga_read_vram(&v, 0xb0000UL), 65);
    EQ(cvga_read_vram(&v, 0xb8000UL), 255);
    gc(6, 2); /* 128 KB map and A16 -> plane A0 in odd/even */
    cvga_write_vram(&v, 0xb0000UL, 0x52);
    EQ(v.plane[0][1], 0x52);
    seq(4, 0); EQ(cvga_read_vram(&v, 0xa0000UL), 255);
    cvga_write_vram(&v, 0xa0000UL, 99); EQ(v.plane[0][0], 65);
    seq(4, 2); cvga_write_port(&v, 0x3c2, 0x61);
    EQ(cvga_read_vram(&v, 0xa0000UL), 255); /* RAM disabled */
    unchained(); seq(2, 15);
    cvga_write_vram(&v, 0xaffffUL, 0x77);
    EQ(plane_read(3, 65535), 0x77);
    EQ(cvga_read_vram(&v, 0xb0000UL), 255);
}

static void test_packed(void)
{
    uint8_t row[322];
    unsigned x, y;
    cvga_geometry g;
    cvga_init(&v); EQ(cvga_set_bios_mode(&v, 0x13, 0), 1);
    EQ(cvga_get_geometry(&v, &g), 1);
    EQ(g.width, 320); EQ(g.height, 200); EQ(g.scan_repeat, 2);
    for (y = 0; y < 200; ++y)
        for (x = 0; x < 320; ++x)
            cvga_write_vram(&v, 0xa0000UL + y * 320UL + x, (uint8_t)(x + 3 * y));
    row[0] = 0xaa; row[321] = 0xbb;
    for (y = 0; y < 200; ++y) {
        EQ(cvga_render_row8(&v, y, row + 1, 320, 0), 320);
        for (x = 0; x < 320; ++x) EQ(row[x + 1], (x + 3 * y) & 255);
    }
    EQ(row[0], 0xaa); EQ(row[321], 0xbb);
    EQ(cvga_render_row8(&v, 200, row, 322, 0), 0);
    EQ(cvga_render_row8(&v, 0, row, 319, 0), 0); EQ(row[0], 0xaa);
    seq(2, 1); cvga_write_vram(&v, 0xa0001UL, 0xff);
    EQ(cvga_read_vram(&v, 0xa0001UL), 1); /* chain4 also honours map mask */
    cvga_write_port(&v, 0x3c6, 7);
    cvga_render_row8(&v, 2, row, 322, 0);
    EQ(row[0], 6); EQ(row[3], 1);
    seq(4, 6); cr(20, 0); cr(23, 0xe3);
    /* Changing CRTC addressing must never move stored physical VRAM. */
    EQ(plane_read(0, 0), 0); EQ(plane_read(1, 0), 1);
    EQ(plane_read(0, 1), 0); EQ(plane_read(0, 4), 4);
    EQ(cvga_set_bios_mode(&v, 0x93, 0), 1);
    EQ(cvga_read_vram(&v, 0xa0004UL), 4);
    /* A split inside a doubled pair cannot be collapsed into 200 rows. */
    cr(17, 0x0e); cr(7, 0x0f); cr(9, 1); cr(24, 0); cr(13, 80);
    EQ(cvga_get_geometry(&v, &g), 1); EQ(g.height, 400); EQ(g.scan_repeat, 1);
    cvga_render_row8(&v, 0, row, 322, 0); EQ(row[0], 3);
    cvga_render_row8(&v, 1, row, 322, 0); EQ(row[0], 0);
    cvga_render_row8(&v, 2, row, 322, 0); EQ(row[0], 0);
    cvga_render_row8(&v, 3, row, 322, 0); EQ(row[0], 3);
}

static void test_modex(void)
{
    uint8_t row[320];
    unsigned x, y, p;
    cvga_geometry g;
    cvga_init(&v); unchained(); cr(17, 0x0e);
    cr(9, 0x40); cr(7, 0x1d); cr(18, 0xef);
    cr(12, 0x20); cr(13, 0);
    EQ(cvga_get_geometry(&v, &g), 1);
    EQ(g.width, 320); EQ(g.height, 240); EQ(g.scan_repeat, 1);
    for (p = 0; p < 4; ++p) {
        seq(2, 1u << p);
        for (y = 0; y < 240; ++y)
            for (x = p; x < 320; x += 4)
                cvga_write_vram(&v, 0xa2000UL + y * 80UL + x / 4, (uint8_t)(x * 5 + y * 7));
    }
    for (y = 0; y < 240; ++y) {
        EQ(cvga_render_row8(&v, y, row, 320, 0), 320);
        for (x = 0; x < 320; ++x) EQ(row[x], (x * 5 + y * 7) & 255);
    }
    ar(19, 4); cvga_render_row8(&v, 0, row, 320, 0);
    EQ(row[0], 10); EQ(row[1], 15);
    cr(19, 44); /* explicit 88-byte pitch, independent of width */
    cvga_render_row8(&v, 1, row, 320, 0); EQ(row[0], 177);
    ar(19, 0); cr(12, 255); cr(13, 255); cr(19, 40);
    for (p = 0; p < 4; ++p) {
        plane_write(p, 65535, p + 30);
        plane_write(p, 0, p + 40);
    }
    cvga_render_row8(&v, 0, row, 320, 0);
    for (x = 0; x < 4; ++x) { EQ(row[x], x + 30); EQ(row[x + 4], x + 40); }
    cr(12, 0x20); cr(13, 0); cr(24, 1); cr(7, 0x0d); cr(9, 0);
    ar(19, 2); ar(16, 0x61); /* split resets panning */
    cvga_render_row8(&v, 2, row, 320, 0);
    EQ(row[0], 40); EQ(row[3], 43);
    /* Unsupported timing/interleave returns no pixels, not fabricated output. */
    gc(5, 0x20); row[0] = 0x99;
    EQ(cvga_render_row8(&v, 0, row, 320, 0), 0); EQ(row[0], 0x99);
}

static void test_text_and_planar(void)
{
    uint8_t row[720], font[4096] = {0};
    cvga_geometry g;
    unsigned i;
    cvga_init(&v);
    font[65 * 16 + 3] = 0x81;
    font[0xc4 * 16 + 3] = 1;
    cvga_load_font_8x16(&v, font);
    cvga_write_vram(&v, 0xb8000UL, 65); cvga_write_vram(&v, 0xb8001UL, 7);
    cvga_write_vram(&v, 0xb8002UL, 0xc4); cvga_write_vram(&v, 0xb8003UL, 2);
    EQ(cvga_get_geometry(&v, &g), 1);
    EQ(g.width, 720); EQ(g.height, 400); EQ(g.char_height, 16);
    EQ(cvga_render_row8(&v, 3, row, 720, CVGA_BLINK_VISIBLE), 720);
    EQ(row[0], 7); EQ(row[7], 7); EQ(row[8], 0);
    EQ(row[16], 2); EQ(row[17], 2); /* ninth-column line graphics replication */
    for (i = 1; i < 7; ++i) EQ(row[i], 0);
    cvga_write_vram(&v, 0xb8001UL, 0x87);
    cvga_render_row8(&v, 3, row, 720, 0); EQ(row[0], 0);
    cvga_render_row8(&v, 3, row, 720, CVGA_BLINK_VISIBLE); EQ(row[0], 7);
    cvga_render_row8(&v, 13, row, 720, CVGA_CURSOR_VISIBLE);
    for (i = 0; i < 9; ++i) EQ(row[i], 7);
    cr(10, 0x20); cvga_render_row8(&v, 13, row, 720, CVGA_CURSOR_VISIBLE); EQ(row[0], 0);
    cr(13, 1); cvga_render_row8(&v, 3, row, 720, 0); EQ(row[7], 2); EQ(row[8], 2);
    unchained(); gc(5, 0); cr(17, 0x0e); cr(1, 39);
    plane_write(0, 0, 0xaa); plane_write(1, 0, 0xcc);
    plane_write(2, 0, 0xf0); plane_write(3, 0, 0);
    EQ(cvga_render_row8(&v, 0, row, 720, 0), 320);
    for (i = 0; i < 8; ++i) EQ(row[i], 7 - i);
    ar(7, 0x32); ar(16, 0x81); ar(20, 9);
    cvga_render_row8(&v, 0, row, 720, 0); EQ(row[0], 0x92);
    seq(1, 0x21); ar(17, 0x55);
    EQ(cvga_get_geometry(&v, &g), 1); EQ(g.blank, 1);
    cvga_render_row8(&v, 0, row, 720, 0);
    for (i = 0; i < 320; ++i) EQ(row[i], 0);
}


static int dirty_bit(unsigned plane, unsigned at)
{
    at /= CVGA_DIRTY_GRANULE;
    return (v.dirty[plane][at >> 3] >> (at & 7)) & 1;
}

static void clear_dirty(void) { memset(v.dirty, 0, sizeof(v.dirty)); }

static void test_modes(void)
{
    static const unsigned supported[][5] = {
        /* mode, width, rows after collapsing identical scanlines, repeat, text */
        {0x00, 360, 400, 1, 1}, {0x01, 360, 400, 1, 1}, {0x02, 720, 400, 1, 1},
        {0x03, 720, 400, 1, 1}, {0x0d, 320, 200, 2, 0}, {0x0e, 640, 200, 2, 0},
        {0x10, 640, 350, 1, 0}, {0x11, 640, 480, 1, 0}, {0x12, 640, 480, 1, 0},
        {0x13, 320, 200, 2, 0}
    };
    static const unsigned unsupported[] = {0x04, 0x05, 0x06, 0x07, 0x0f, 0x14, 0x6a, 0x7f};
    cvga_geometry g;
    unsigned i;
    uint32_t before;
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        cvga_init(&v);
        before = v.display_changes;
        EQ(cvga_set_bios_mode(&v, supported[i][0], 0), 1);
        EQ(v.display_changes, before + 1);
        EQ(cvga_get_geometry(&v, &g), 1);
        EQ(g.width, supported[i][1]);
        EQ(g.height, supported[i][2]);
        EQ(g.scan_repeat, supported[i][3]);
        EQ(g.text, supported[i][4]);
        EQ(g.blank, 0);
        EQ(cvga_find_mode(supported[i][0] | 0x80) != 0, 1);
    }
    for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        cvga_init(&v);
        cr(17, 0x0e); cr(0, 0x33);
        EQ(cvga_set_bios_mode(&v, unsupported[i], 0), 0);
        EQ(cvga_find_mode(unsupported[i]) == 0, 1);
        EQ(v.crtc[0], 0x33);
    }
    /* Planar 12h rendering reaches all 480 physical rows. */
    cvga_init(&v); cvga_set_bios_mode(&v, 0x12, 0);
    seq(2, 15);
    gc(0, 9); gc(1, 15);
    cvga_write_vram(&v, 0xa0000UL + 479 * 80 + 79, 0);
    {
        uint8_t row[640];
        EQ(cvga_render_row8(&v, 479, row, 640, 0), 640);
        EQ(row[639], 0x39); EQ(row[632], 0x39); EQ(row[631], 0);
    }
}

/* cvga_direct_plane promises that mapping its plane at A0000-AFFFF is exact
 * for writes: an emulated store then changes exactly that plane's byte to
 * the CPU value. Check that for Mode X, every single-plane mask, and random
 * register states against the full write path. */
static void test_direct_plane(void)
{
    static const unsigned seq_vals[] = {0x06, 0x0e, 0x02, 0x04, 0x07};
    unsigned seed = 12345, n, p, i, at, before[4];
    int plane;
    cvga_init(&v); unchained();
    for (p = 0; p < 4; ++p) { seq(2, 1u << p); EQ(cvga_direct_plane(&v), p); }
    seq(2, 3); EQ(cvga_direct_plane(&v), -1);
    seq(2, 1); gc(5, 1); EQ(cvga_direct_plane(&v), -1);
    gc(5, 0); gc(8, 0x7f); EQ(cvga_direct_plane(&v), -1);
    gc(8, 0xff); gc(1, 1); EQ(cvga_direct_plane(&v), -1);
    gc(1, 0); gc(3, 0x08); EQ(cvga_direct_plane(&v), -1);
    gc(3, 0); seq(4, 0x0e); EQ(cvga_direct_plane(&v), -1);   /* chain-4 */
    for (n = 0; n < 4000; ++n) {
        seed = seed * 1103515245u + 12345u;
        cvga_init(&v); unchained();
        seq(2, (seed >> 4) & 15);
        seq(4, seq_vals[(seed >> 9) % 5]);
        gc(1, (seed >> 12) & 1 ? (seed >> 13) & 15 : 0);
        gc(3, (seed >> 17) & 1 ? (seed >> 18) & 31 : 0);
        gc(5, (seed >> 23) & 1 ? (seed >> 24) & 0x13 : 0);
        gc(8, (seed >> 27) & 1 ? (seed >> 5) & 255 : 0xff);
        gc(6, (seed >> 28) & 1 ? 0x01 : 0x05);
        plane = cvga_direct_read_plane(&v);
        if (plane >= 0) {
            gc(4, (seed >> 1) & 3);
            plane = cvga_direct_read_plane(&v);
            at = (seed >> 7) & 0xffff;
            EQ(plane, (seed >> 1) & 3);
            v.plane[plane][at] = (uint8_t)(seed >> 19);
            EQ(cvga_peek_vram(&v, 0xa0000UL + at), (seed >> 19) & 255);
        }
        plane = cvga_direct_plane(&v);
        if (plane < 0) continue;
        at = (seed >> 3) & 0xffff;
        for (i = 0; i < 4; ++i) before[i] = plane_read(i, at);
        cvga_write_vram(&v, 0xa0000UL + at, (uint8_t)(seed >> 11));
        for (i = 0; i < 4; ++i)
            EQ(plane_read(i, at), i == (unsigned)plane ? ((seed >> 11) & 255) : before[i]);
    }
}

static void test_dirty_and_peek(void)
{
    uint32_t before;
    unsigned i;
    cvga_init(&v); cvga_set_bios_mode(&v, 0x13, 0);
    for (i = 0; i < CVGA_DIRTY_BYTES; ++i) EQ(v.dirty[3][i], 0xff); /* clear marks all */
    clear_dirty();
    cvga_write_vram(&v, 0xa0000UL + 1000, 7);
    EQ(dirty_bit(0, 1000), 1); EQ(dirty_bit(1, 1000), 0); EQ(dirty_bit(0, 1032), 0);
    clear_dirty();
    cvga_write_vram(&v, 0xa0000UL + 1000, 7);           /* unchanged value */
    EQ(dirty_bit(0, 1000), 0);
    before = v.display_changes;
    seq(2, 3); gc(8, 0x55); gc(5, 0x41); gc(3, 8); /* latch/mask/write-mode state */
    EQ(v.display_changes, before);
    gc(5, 0x40);
    EQ(v.display_changes, before);
    cr(12, 1); EQ(v.display_changes, before + 1);
    cr(12, 1); EQ(v.display_changes, before + 1);      /* same value */
    cvga_write_port(&v, 0x3c8, 9); cvga_write_port(&v, 0x3c9, 1);
    EQ(v.display_changes, before + 2);
    cvga_write_port(&v, 0x3c9, 0); cvga_write_port(&v, 0x3c9, 0);
    EQ(v.display_changes, before + 2);                 /* unchanged components */
    cvga_read_port(&v, 0x3da, 0); cvga_write_port(&v, 0x3c0, 0x20);
    EQ(v.display_changes, before + 2);                 /* PAS already set */
    cvga_read_port(&v, 0x3da, 0); cvga_write_port(&v, 0x3c0, 0x00);
    EQ(v.display_changes, before + 3);                 /* PAS cleared: blank */
    cvga_write_port(&v, 0x3c0, 0); cvga_read_port(&v, 0x3da, 0);
    cvga_write_port(&v, 0x3c0, 0x20);
    /* Peek reports what a read returns but never loads the latches. */
    cr(12, 0); seq(2, 15); gc(3, 0); gc(8, 0xff);
    cvga_write_vram(&v, 0xa0000UL + 4, 0x11); cvga_write_vram(&v, 0xa0000UL + 5, 0x22);
    cvga_read_vram(&v, 0xa0000UL + 5);
    EQ(v.latch[0], 0x11); EQ(v.latch[1], 0x22);
    EQ(cvga_peek_vram(&v, 0xa0000UL), 0);
    EQ(v.latch[0], 0x11);
    EQ(cvga_peek_vram(&v, 0xa0000UL + 4), 0x11);
    EQ(cvga_peek_vram(&v, 0xa0000UL + 5), cvga_read_vram(&v, 0xa0000UL + 5));
    EQ(cvga_peek_vram(&v, 0xb8000UL), 255);
}

static void test_row_damage(void)
{
    unsigned y;
    cvga_init(&v); cvga_set_bios_mode(&v, 0x13, 0); clear_dirty();
    cvga_write_vram(&v, 0xa0000UL + 10 * 320 + 5, 9);
    for (y = 0; y < 200; ++y)
        EQ(cvga_row_reads_dirty(&v, y, (const uint8_t (*)[CVGA_DIRTY_BYTES])v.dirty), y == 10);
    cvga_init(&v); cvga_set_bios_mode(&v, 0x12, 0); clear_dirty();
    seq(2, 4);
    cvga_write_vram(&v, 0xa0000UL + 100 * 80 + 3, 0xff);
    for (y = 0; y < 480; ++y)
        EQ(cvga_row_reads_dirty(&v, y, (const uint8_t (*)[CVGA_DIRTY_BYTES])v.dirty), y == 100);
    cvga_init(&v); cvga_set_bios_mode(&v, 3, 0); clear_dirty();
    cvga_write_vram(&v, 0xb8000UL + (2 * 80 + 5) * 2 + 1, 0x1e);   /* attribute byte */
    for (y = 0; y < 400; ++y)
        EQ(cvga_row_reads_dirty(&v, y, (const uint8_t (*)[CVGA_DIRTY_BYTES])v.dirty), y >= 32 && y < 48);
    /* Mode X page flip: rows follow the CRTC start address, not the write. */
    cvga_init(&v); unchained(); cr(17, 0x0e); cr(12, 0x40); cr(13, 0); clear_dirty();
    plane_write(2, 0x4000 + 7 * 80 + 1, 0x77);
    for (y = 0; y < 200; ++y)
        EQ(cvga_row_reads_dirty(&v, y, (const uint8_t (*)[CVGA_DIRTY_BYTES])v.dirty), y == 7);
    clear_dirty(); plane_write(2, 7 * 80 + 1, 0x77);            /* hidden page */
    for (y = 0; y < 200; ++y)
        EQ(cvga_row_reads_dirty(&v, y, (const uint8_t (*)[CVGA_DIRTY_BYTES])v.dirty), 0);
}

/* ---- Virtual BIOS over a 1 MiB RAM bus ---- */

static uint8_t ram[0x110000];
static unsigned bus_faults;
static int ram_read(void *c, uint32_t a, uint8_t *value)
{
    (void)c;
    if (a >= sizeof(ram) || (a >= 0xa0000UL && a < 0xc0000UL)) { ++bus_faults; return 1; }
    *value = ram[a];
    return 0;
}
static int ram_write(void *c, uint32_t a, uint8_t value)
{
    (void)c;
    if (a >= sizeof(ram) || (a >= 0xa0000UL && a < 0xc0000UL)) { ++bus_faults; return 1; }
    ram[a] = value;
    return 0;
}
static const cvx_bus ram_bus = {ram_read, ram_write, ram_read, 0, 0};

static cvbios bios;
static cvbios_regs regs;
static int int10(unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{
    memset(&regs, 0, sizeof(regs));
    regs.eax = ax; regs.ebx = bx; regs.ecx = cx; regs.edx = dx;
    return cvbios_int10(&bios, &regs);
}
static unsigned bda16(unsigned o)
{
    if (o < 0x400) o += 0x400;               /* relative or absolute BDA offset */
    return ram[o] | (ram[o + 1] << 8);
}

static void bios_setup(void)
{
    unsigned i;
    memset(ram, 0, sizeof(ram));
    memset(&bios, 0, sizeof(bios));
    cvga_init(&v);
    bios.vga = &v;
    bios.bus = &ram_bus;
    /* Synthetic firmware fonts: glyph byte = character code ^ scanline. */
    for (i = 0; i < 256 * 16; ++i) ram[0xc1000 + i] = (uint8_t)((i / 16) ^ (i % 16));
    for (i = 0; i < 256 * 14; ++i) ram[0xc2000 + i] = (uint8_t)((i / 14) + 3);
    for (i = 0; i < 128 * 8; ++i) ram[0xc3000 + i] = (uint8_t)(i / 8);
    for (i = 0; i < 128 * 8; ++i) ram[0xc3400 + i] = (uint8_t)(128 + i / 8);
    bios.font[CVBIOS_FONT_8X16] = 0xc1000000UL;           /* C100:0000 */
    bios.font[CVBIOS_FONT_8X14] = 0xc2000000UL;
    bios.font[CVBIOS_FONT_8X8] = 0xc3000000UL;
    bios.font[CVBIOS_FONT_8X8_HIGH] = 0xc3400000UL;
    ram[0x488] = 0xf9; ram[0x489] = 0x51;
}

static void test_bios(void)
{
    uint8_t palette[768];
    unsigned i, c;
    cvga_geometry g;
    bios_setup();
    /* 13h: BDA, default 256-colour DAC, geometry and INT 43h. */
    EQ(int10(0x0013, 0, 0, 0), 1);
    EQ(ram[0x449], 0x13); EQ(bda16(0x4a), 40); EQ(bda16(0x4c), 0xfa00); EQ(ram[0x484], 24);
    EQ(bda16(0x485), 8); EQ(bda16(0x463), 0x3d4); EQ(ram[0x487] & 0x80, 0);
    EQ(ram[0x43 * 4 + 2] | (ram[0x43 * 4 + 3] << 8), 0xc300);
    EQ(cvbios_default_palette(3, palette), 256);
    for (i = 0; i < 768; ++i) EQ(v.dac[i / 3][i % 3], palette[i]);
    EQ(cvga_get_geometry(&v, &g), 1); EQ(g.width, 320);
    EQ(int10(0x0f00, 0, 0, 0), 1); EQ(regs.eax & 0xffff, 0x2813);
    /* Pixels: 13h has no XOR bit; 12h XORs with bit 7. */
    EQ(int10(0x0c85, 0, 3, 2), 1);
    EQ(v.plane[(2 * 320 + 3) & 3][(2 * 320 + 3) & 0xfffc], 0x85);
    EQ(int10(0x0d00, 0, 3, 2), 1); EQ(regs.eax & 0xff, 0x85);
    EQ(int10(0x0012, 0, 0, 0), 1);
    EQ(int10(0x0c0a, 0, 9, 1), 1);
    EQ(int10(0x0d00, 0, 9, 1), 1); EQ(regs.eax & 0xff, 0x0a);
    EQ(int10(0x0c83, 0, 9, 1), 1);
    EQ(int10(0x0d00, 0, 9, 1), 1); EQ(regs.eax & 0xff, 0x09);
    EQ(cvbios_default_palette(2, palette), 64);
    for (i = 0; i < 192; ++i) EQ(v.dac[i / 3][i % 3], palette[i]);
    EQ(v.dac[64][0] | v.dac[255][2], 0);
    /* Graphics text through INT 43h (8x16 in 12h). */
    EQ(bda16(0x485), 16);
    EQ(int10(0x0941, 0x000c, 1, 0), 1);
    EQ(int10(0x0d00, 0, 1, 0), 1); EQ(regs.eax & 0xff, ('A' & 0x40) ? 0x0c : 0);
    /* Text mode: font, clear, TTY, scroll, cursor, read-back. */
    EQ(int10(0x0003, 0, 0, 0), 1);
    for (c = 0; c < 256; ++c)
        for (i = 0; i < 32; ++i) EQ(v.plane[2][c * 32 + i], i < 16 ? (c ^ i) & 255 : 0);
    EQ(v.plane[0][0], 0x20); EQ(v.plane[1][0], 0x07); EQ(v.plane[1][2 * 1999], 0x07);
    EQ(int10(0x0e41, 0, 0, 0), 1); EQ(int10(0x0e42, 0, 0, 0), 1);
    EQ(int10(0x0e0d, 0, 0, 0), 1); EQ(int10(0x0e0a, 0, 0, 0), 1);
    EQ(v.plane[0][0], 'A'); EQ(v.plane[0][2], 'B'); EQ(bda16(0x450), 0x0100);
    EQ(v.crtc[14], 0); EQ(v.crtc[15], 80);
    EQ(int10(0x0200, 0, 0, 0x1800 + 79), 1);             /* bottom-right */
    EQ(int10(0x0e5a, 0, 0, 0), 1);                       /* wraps and scrolls */
    EQ(bda16(0x450), 0x1800);
    EQ(v.plane[0][(23 * 80 + 79) * 2], 'Z');
    EQ(v.plane[0][0], 0x20);                             /* 'A' row scrolled out */
    EQ(int10(0x0800, 0, 0, 0), 1); EQ(regs.eax & 0xffff, 0x0720);
    EQ(int10(0x0932, 0x1e, 3, 0), 1);
    EQ(v.plane[0][(24 * 80) * 2 + 4], '2'); EQ(v.plane[1][(24 * 80) * 2 + 4], 0x1e);
    EQ(int10(0x0a33, 0x4f, 1, 0), 1);
    EQ(v.plane[0][(24 * 80) * 2], '3'); EQ(v.plane[1][(24 * 80) * 2], 0x1e);
    EQ(int10(0x0601, 0x7000, 0x1800, 0x184f), 1);        /* scroll bottom row */
    EQ(v.plane[0][(24 * 80) * 2], ' '); EQ(v.plane[1][(24 * 80) * 2], 0x70);
    EQ(int10(0x0100, 0, 0x0607, 0), 1);                   /* cursor emulation */
    EQ(v.crtc[10], 14); EQ(v.crtc[11], 15); EQ(bda16(0x460), 0x0607); /* SeaVGABIOS emulation */
    EQ(int10(0x0300, 0, 0, 0), 1); EQ(regs.ecx & 0xffff, 0x0607); EQ(regs.edx & 0xffff, 0x1800);
    EQ(int10(0x0501, 0, 0, 0), 1);
    EQ(ram[0x462], 1); EQ(bda16(0x44e), 0x1000); EQ(v.crtc[12], 0x08); EQ(v.crtc[13], 0);
    /* Write string with attributes, cursor updated. */
    ram[0x5000] = 'H'; ram[0x5001] = 0x2f; ram[0x5002] = 'i'; ram[0x5003] = 0x3e;
    memset(&regs, 0, sizeof(regs));
    regs.eax = 0x1303; regs.ebx = 0x0100; regs.ecx = 2; regs.edx = 0x0102; regs.es = 0x500;
    EQ(cvbios_int10(&bios, &regs), 1);
    EQ(v.plane[0][0x1000 + (80 + 2) * 2], 'H'); EQ(v.plane[1][0x1000 + (80 + 2) * 2], 0x2f);
    EQ(v.plane[1][0x1000 + (80 + 3) * 2], 0x3e); EQ(bda16(0x452), 0x0104);
    /* DAC and attribute services. */
    EQ(int10(0x1010, 5, 0x0203, 0x0100), 1);
    EQ(v.dac[5][0], 1); EQ(v.dac[5][1], 2); EQ(v.dac[5][2], 3);
    EQ(int10(0x1015, 5, 0, 0), 1); EQ((regs.edx >> 8) & 255, 1); EQ(regs.ecx & 0xffff, 0x0203);
    for (i = 0; i < 6; ++i) ram[0x6000 + i] = (uint8_t)(10 + i);
    memset(&regs, 0, sizeof(regs)); regs.eax = 0x1012; regs.ebx = 250; regs.ecx = 2; regs.es = 0x600;
    EQ(cvbios_int10(&bios, &regs), 1); EQ(v.dac[251][2], 15);
    memset(&regs, 0, sizeof(regs)); regs.eax = 0x1017; regs.ebx = 5; regs.ecx = 1; regs.edx = 0x10; regs.es = 0x600;
    EQ(cvbios_int10(&bios, &regs), 1); EQ(ram[0x6010], 1); EQ(ram[0x6012], 3);
    EQ(int10(0x1018, 0x0f, 0, 0), 1); EQ(v.dac_mask, 0x0f);
    EQ(int10(0x1019, 0, 0, 0), 1); EQ(regs.ebx & 0xff, 0x0f);
    EQ(int10(0x1000, 0x2a03, 0, 0), 1); EQ(v.attr[3], 0x2a);
    EQ(int10(0x1007, 3, 0, 0), 1); EQ((regs.ebx >> 8) & 255, 0x2a);
    EQ(int10(0x1003, 0, 0, 0), 1); EQ(v.attr[16] & 8, 0);
    EQ(int10(0x1003, 1, 0, 0), 1); EQ(v.attr[16] & 8, 8);
    ram[0x489] |= 2;                                      /* gray-scale summing */
    EQ(int10(0x1010, 7, 0x0000, 0x3f00), 1);
    EQ(v.dac[7][0], 19); EQ(v.dac[7][1], 19); EQ(v.dac[7][2], 19);
    ram[0x489] &= ~2;
    /* Identification: VGA colour display, EGA info, font pointers. */
    EQ(int10(0x1a00, 0, 0, 0), 1); EQ(regs.eax & 0xff, 0x1a); EQ(regs.ebx & 0xffff, 8);
    EQ(int10(0x1200, 0x10, 0, 0), 1); EQ(regs.ebx & 0xffff, 3); EQ(regs.ecx & 0xffff, 0x0f09);
    EQ(int10(0x1130, 0x0600, 0, 0), 1);
    EQ(regs.es, 0xc100); EQ(regs.ebp, 0); EQ(regs.ecx & 0xffff, 16); EQ(regs.edx & 0xff, 24);
    EQ(int10(0x1201, 0x31, 0, 0), 1); EQ(regs.eax & 0xff, 0x12); EQ(ram[0x489] & 8, 8);
    EQ(int10(0x0003, 0, 0, 0), 1); EQ(v.dac[5][0], 1);  /* loading disabled: DAC kept */
    EQ(int10(0x1200, 0x31, 0, 0), 1); EQ(ram[0x489] & 8, 0);
    /* 8x8 ROM font in text mode: 50 rows. */
    EQ(int10(0x1112, 0, 0, 0), 1);
    EQ(ram[0x484], 49); EQ(bda16(0x485), 8); EQ(v.crtc[9] & 31, 7); EQ(bda16(0x44c), 0x2000);
    EQ(v.plane[2][0x41 * 32], 0x41); EQ(v.plane[2][0xc1 * 32 + 7], 0xc1);
    /* Unsupported functions leave registers and counters explicit. */
    i = bios.unsupported;
    EQ(int10(0x4f00, 0x1234, 0x5678, 0x9abc), 0);
    EQ(regs.eax, 0x4f00); EQ(regs.ebx, 0x1234); EQ(bios.unsupported, i + 1);
    EQ(bios.last_unsupported, 0x4f00);
    EQ(int10(0x0004, 0, 0, 0), 0); EQ(ram[0x449], 3);
    EQ(bus_faults, 0);
}

static void test_bios_mode_matrix(void)
{
    static const unsigned mode_list[] = {0, 1, 2, 3, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x13};
    unsigned i;
    cvga_geometry g;
    for (i = 0; i < sizeof(mode_list) / sizeof(mode_list[0]); ++i) {
        const cvga_mode_params *m = cvga_find_mode(mode_list[i]);
        bios_setup();
        EQ(int10(mode_list[i], 0, 0, 0), 1);
        EQ(ram[0x449], mode_list[i]);
        EQ(bda16(0x44a), m->columns);
        EQ(ram[0x484] + 1u, m->rows);
        EQ(bda16(0x485), m->char_height);
        EQ(cvga_get_geometry(&v, &g), 1);
        EQ(g.width, m->width);
        EQ(g.height, m->height);
        EQ(int10(0x0f00, 0, 0, 0), 1); EQ(regs.eax & 0xff, mode_list[i]);
        /* bit 7 preserves memory and is reported by 0Fh. */
        v.plane[3][1234] = 0x5a;
        EQ(int10(0x80 | mode_list[i], 0, 0, 0), 1);
        EQ(v.plane[3][1234], 0x5a); EQ(ram[0x487] & 0x80, 0x80);
        EQ(int10(0x0f00, 0, 0, 0), 1); EQ(regs.eax & 0xff, 0x80 | mode_list[i]);
    }
}

/* ---- Presenter ---- */

static uint8_t surface[800 * 600 * 4];
static uint8_t reference_surface[800 * 600 * 4];
static cvp_state pstate;

static uint32_t px32(const uint8_t *s, unsigned x, unsigned y)
{
    const uint8_t *p = s + (y * 800 + x) * 4;
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rgb(unsigned index)
{
    index &= 255;
    unsigned r = v.dac[index][0], g = v.dac[index][1], b = v.dac[index][2];
    r = (r << 2) | (r >> 4); g = (g << 2) | (g >> 4); b = (b << 2) | (b >> 4);
    return (r << 16) | (g << 8) | b;
}

static void test_presenter(void)
{
    static const cvp_format f32 = {800 * 4, 800, 600, 4, 8, 16, 8, 8, 8, 0, 0};
    static const cvp_format f16 = {800 * 2, 800, 600, 2, 5, 11, 6, 5, 5, 0, 0};
    static const cvp_format f24 = {800 * 3, 800, 600, 3, 8, 16, 8, 8, 8, 0, 0};
    cvp_request q;
    cvp_stats st;
    unsigned x, y, calls;
    bios_setup();
    EQ(int10(0x0013, 0, 0, 0), 1);
    for (y = 0; y < 200; ++y)
        for (x = 0; x < 320; ++x)
            cvga_write_vram(&v, 0xa0000UL + y * 320 + x, (uint8_t)(x ^ y));
    memset(surface, 0xee, sizeof(surface));
    memset(&q, 0, sizeof(q));
    q.window.left = 10; q.window.top = 20; q.window.right = 650; q.window.bottom = 420;
    cvp_init(&pstate);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
    EQ(st.complete, 1); EQ(st.rows_drawn, 400); EQ(st.pixels_written, 640u * 400u);
    EQ(st.source_width, 320); EQ(st.source_height, 400);
    for (y = 0; y < 400; y += 7)
        for (x = 0; x < 640; x += 3)
            EQ(px32(surface, 10 + x, 20 + y), rgb((x / 2) ^ (y / 2)));
    EQ(px32(surface, 9, 20), 0xeeeeeeeeUL); EQ(px32(surface, 650, 20), 0xeeeeeeeeUL);
    EQ(px32(surface, 10, 19), 0xeeeeeeeeUL); EQ(px32(surface, 10, 420), 0xeeeeeeeeUL);
    /* Nothing changed: nothing drawn. */
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
    EQ(st.rows_drawn, 0); EQ(st.complete, 1);
    /* One pixel: exactly its two destination rows. */
    cvga_write_vram(&v, 0xa0000UL + 50 * 320 + 7, 200);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
    EQ(st.rows_drawn, 2); EQ(px32(surface, 10 + 14, 20 + 100), rgb(200));
    /* A latch/mask update alone is not damage. */
    seq(2, 3); gc(8, 0x0f);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 0);
    seq(2, 15); gc(8, 0xff);
    /* DAC change redraws every row. */
    cvga_write_port(&v, 0x3c8, 200); cvga_write_port(&v, 0x3c9, 63);
    cvga_write_port(&v, 0x3c9, 0); cvga_write_port(&v, 0x3c9, 0);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
    EQ(st.rows_drawn, 400); EQ(px32(surface, 10 + 14, 20 + 100), 0xff0000UL);
    /* Bounded work: 10 rows per call, same final image. */
    memcpy(reference_surface, surface, sizeof(surface));
    for (y = 0; y < 200; ++y) cvga_write_vram(&v, 0xa0000UL + y * 320 + 100, (uint8_t)y);
    q.budget_pixels = 640 * 10;
    calls = 0;
    do {
        EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
        EQ(st.rows_drawn <= 10, 1);
        ++calls;
    } while (!st.complete && calls < 1000);
    EQ(calls, 40);
    EQ(st.passes >= 2, 1);
    for (y = 0; y < 400; ++y) EQ(px32(surface, 10 + 200, 20 + y), rgb(y / 2));
    /* A change during an incomplete frame is not lost. */
    q.budget_pixels = 640 * 3;
    cvga_write_vram(&v, 0xa0000UL + 199 * 320, 1);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.complete, 1);
    for (y = 0; y < 200; ++y) cvga_write_vram(&v, 0xa0000UL + y * 320 + 101, 3);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.complete, 0);
    for (y = 0; y < 200; ++y) cvga_write_vram(&v, 0xa0000UL + y * 320 + 101, 4);
    calls = 0;
    do { cvp_present(&pstate, &v, &f32, &q, surface, &st); } while (!st.complete && ++calls < 1000);
    for (y = 0; y < 400; ++y) EQ(px32(surface, 10 + 202, 20 + y), rgb(4));
    /* Clipping: only the visible rectangles are written. */
    q.budget_pixels = 0;
    q.clip_count = 2;
    q.clip[0].left = 0; q.clip[0].top = 0; q.clip[0].right = 100; q.clip[0].bottom = 600;
    q.clip[1].left = 300; q.clip[1].top = 300; q.clip[1].right = 800; q.clip[1].bottom = 600;
    memset(surface, 0xee, sizeof(surface));
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
    EQ(st.pixels_written, 90u * 400 + 350u * 120);
    EQ(px32(surface, 99, 30), rgb((89 / 2) ^ (10 / 2)));
    EQ(px32(surface, 100, 30), 0xeeeeeeeeUL); EQ(px32(surface, 300, 299), 0xeeeeeeeeUL);
    EQ(px32(surface, 300, 300), rgb((290 / 2) ^ (280 / 2)));
    /* A visibility change is a full frame; then exposure rendering keeps
     * the damage state for the next frame. */
    q.clip_count = 0;
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 400);
    cvga_write_vram(&v, 0xa0000UL, 77);
    q.flags = CVP_NO_DAMAGE;
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 400);
    q.flags = 0;
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 2);
    /* Other native formats; resizing rebuilds scaling. */
    q.window.left = 0; q.window.top = 0; q.window.right = 320; q.window.bottom = 200;
    EQ(cvp_present(&pstate, &v, &f16, &q, surface, &st), CVP_OK);
    EQ(st.rows_drawn, 200);
    {
        unsigned idx = 77, r = v.dac[idx][0], g = v.dac[idx][1], b = v.dac[idx][2];
        unsigned r8 = (r << 2) | (r >> 4), g8 = (g << 2) | (g >> 4), b8 = (b << 2) | (b >> 4);
        EQ(surface[0] | (surface[1] << 8), ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
    }
    q.window.right = 960; q.window.bottom = 600;
    EQ(cvp_present(&pstate, &v, &f24, &q, surface, &st), CVP_OK);
    EQ(st.rows_drawn, 600); EQ(st.pixels_written, 800u * 600u);   /* clipped at surface */
    EQ(surface[(599 * 800 + 799) * 3 + 2], (rgb(((799 * 320) / 960) ^ 199) >> 16) & 255);
    /* Invalid requests are rejected without drawing. */
    q.window.right = q.window.left;
    EQ(cvp_present(&pstate, &v, &f24, &q, surface, &st), CVP_BAD_RECT);
    {
        cvp_format bad = f32;
        bad.pitch = 10;
        q.window.right = 100;
        EQ(cvp_present(&pstate, &v, &bad, &q, surface, &st), CVP_BAD_FORMAT);
        bad = f32; bad.red_pos = 30;
        EQ(cvp_present(&pstate, &v, &bad, &q, surface, &st), CVP_BAD_FORMAT);
    }
    /* Text: a cursor phase change redraws only the cursor scanlines. */
    EQ(int10(0x0003, 0, 0, 0), 1);
    EQ(int10(0x0e41, 0, 0, 0), 1);
    q.window.left = 0; q.window.top = 0; q.window.right = 720; q.window.bottom = 400;
    q.flags = CVP_CURSOR_VISIBLE;
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 400);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 0);
    q.flags = 0;
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 2);
    for (y = 0; y < 400; ++y)
        EQ(cvga_row_phase_sensitive(&v, y), y == 13 || y == 14);   /* CR0A/0B = 0D/0E */
    /* A blinking character (attribute bit 7, blink enabled) makes its row
     * phase sensitive; a hidden cursor does not. */
    cvga_write_vram(&v, 0xb8000UL + (5 * 80 + 3) * 2 + 1, 0x87);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 16);
    EQ(int10(0x0100, 0, 0x2000, 0), 1);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK);
    q.flags = CVP_BLINK_VISIBLE | CVP_CURSOR_VISIBLE;
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 16);
    for (y = 0; y < 400; ++y) EQ(cvga_row_phase_sensitive(&v, y), y >= 80 && y < 96);
    ar(16, 0x04);                                   /* blink disabled */
    EQ(cvga_row_phase_sensitive(&v, 85), 0);
    /* Font change in text mode redraws all rows. */
    cvga_store_plane(&v, 2, 0x41 * 32 + 3, 0);
    EQ(cvp_present(&pstate, &v, &f32, &q, surface, &st), CVP_OK); EQ(st.rows_drawn, 400);
}

static void dump_tables(void)
{
    static const unsigned mode_list[] = {0x01, 0x03, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x13};
    uint8_t palette[768];
    unsigned i, j, which, count;
    printf("{\"modes\": [");
    for (i = 0; i < sizeof(mode_list) / sizeof(mode_list[0]); ++i) {
        const cvga_mode_params *m = cvga_find_mode(mode_list[i]);
        printf("%s{\"mode\": %u, \"seq\": \"", i ? ", " : "", m->mode);
        for (j = 0; j < 4; ++j) printf("%02x", m->seq[j]);
        printf("\", \"misc\": %u, \"crtc\": \"", m->misc);
        for (j = 0; j < 25; ++j) printf("%02x", m->crtc[j]);
        printf("\", \"attr\": \"");
        for (j = 0; j < 20; ++j) printf("%02x", m->attr[j]);
        printf("\", \"gc\": \"");
        for (j = 0; j < 9; ++j) printf("%02x", m->gc[j]);
        printf("\"}");
    }
    printf("], \"palettes\": {");
    for (which = 1; which <= 3; ++which) {
        count = cvbios_default_palette(which, palette);
        printf("%s\"%u\": \"", which > 1 ? ", " : "", which);
        for (j = 0; j < count * 3; ++j) printf("%02x", palette[j]);
        printf("\"");
    }
    printf("}}\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--dump-tables")) {
        dump_tables();
        return 0;
    }
    test_registers(); test_latches(); test_apertures();
    test_packed(); test_modex(); test_text_and_planar();
    test_modes(); test_direct_plane(); test_dirty_and_peek(); test_row_damage();
    test_bios(); test_bios_mode_matrix(); test_presenter();
    printf("PASS: %u independent VGA assertions; state %lu bytes\n", checks, (unsigned long)sizeof(v));
    return 0;
}
