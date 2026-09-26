/* Independent expected vectors for the standalone software VGA. Not a guest
 * acceptance test: this does not execute DOS code or establish VM isolation. */
#include "virtual_vga.h"
#include <stdio.h>
#include <stdlib.h>

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
    EQ(cvga_set_bios_mode(&v, 0x12, 0), 0);
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

int main(void)
{
    test_registers(); test_latches(); test_apertures();
    test_packed(); test_modex(); test_text_and_planar();
    printf("PASS: %u independent VGA assertions; state %lu bytes\n", checks, (unsigned long)sizeof(v));
    return 0;
}
