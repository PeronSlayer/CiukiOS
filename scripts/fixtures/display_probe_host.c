#define DISP_PROBE_HOST_TEST
#define DISP_PROBE_TEST_BIOS
#include "../../src/apps/display_probe.c"
#include <stdio.h>
#include <stdlib.h>

static u16 mapped_off[8];
static void *mapped_ptr[8];
static int mapped_count;
static u16 listed_modes[] = {0x101, 0x110, 0x111, 0x112, 0x118, 0x119,
                             0x120, 0x122, 0xFFFF};
static u16 current_mode = 0x121;
static u16 current_flags = 0x4000;
static u16 active_pitch = 4000;

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void set_mode(u8 *p, u16 id)
{
    int lfb_only = id == 0x118;
    int current_lfb = id == 0x121;
    u16 width = 800, height = 600, bpp = 32, model = 6, pitch = 3200;
    mem_set(p, 0, 256);
    put16(p, (u16)(0x19 | ((!current_lfb && id != 0x120) ? 0x80 : 0)));
    if (id == 0x101 || (id >= 0x110 && id <= 0x112)) {
        width = 640;
        height = 480;
        bpp = id == 0x101 ? 8 : (id == 0x110 ? 15 : (id == 0x111 ? 16 : 24));
        model = id == 0x101 ? 4 : 6;
        pitch = (u16)(width * ((bpp + 7) / 8));
    }
    if (id == 0x101 || (id >= 0x110 && id <= 0x112) ||
        id == 0x119 || id == 0x120 || id == 0x122) {
        p[2] = 5; p[3] = 7;
        put16(p + 4, 16); put16(p + 6, 64); put16(p + 8, 0xA000); put16(p + 10, 0xA000);
    } else if (lfb_only) {
        /* Attribute D7 advertises linear access; no writable WinA exists. */
        p[2] = 0; put16(p + 4, 0); put16(p + 6, 0);
    }
    if (id == 0x110) {
        p[31] = 5; p[32] = 10; p[33] = 5; p[34] = 5; p[35] = 5; p[36] = 0;
    } else if (id == 0x111) {
        p[31] = 5; p[32] = 11; p[33] = 6; p[34] = 5; p[35] = 5; p[36] = 0;
    } else if (id == 0x112) {
        p[31] = 8; p[32] = 16; p[33] = 8; p[34] = 8; p[35] = 8; p[36] = 0;
    } else {
        p[31] = 8; p[32] = 16; p[33] = 8; p[34] = 8; p[35] = 8; p[36] = 0;
    }
    p[37] = 8; p[38] = 24;
    put16(p + 16, pitch); put16(p + 18, width); put16(p + 20, height);
    p[24] = 1; p[25] = (u8)bpp; p[27] = (u8)model;
    if (id != 0x120) put32(p + 40, 0xE0000000UL);
    put16(p + 50, (id == 0x118 || id == 0x119 || id == 0x122) ? 3328 : pitch);
    /* The synthetic 800x600 modes retain a separate linear RGB layout. */
    p[54] = 8; p[55] = 24; p[56] = 8; p[57] = 16;
    p[58] = 8; p[59] = 8; p[60] = 8; p[61] = 0;
    if (id >= 0x110 && id <= 0x112) {
        p[54] = p[31]; p[55] = p[32]; p[56] = p[33]; p[57] = p[34];
        p[58] = p[35]; p[59] = p[36];
    }
    if (id == 0x122) p[57] = 24; /* overlaps red: linear descriptor is invalid */
}

u16 disp_probe_host_ptr(const void *p)
{
    int i;
    for (i = 0; i < mapped_count; ++i) if (mapped_ptr[i] == p) return mapped_off[i];
    mapped_ptr[mapped_count] = (void *)p;
    mapped_off[mapped_count] = (u16)(0x1000 + mapped_count * 0x0200);
    return mapped_off[mapped_count++];
}
u16 app_seg(void) { return 0x2000; }
void mem_set(void *dst, int value, int bytes)
{
    u8 *p = (u8 *)dst;
    while (bytes-- > 0) *p++ = (u8)value;
}
u16 peek16(u16 seg, u16 off)
{
    u32 address = ((u32)seg << 4) + off;
    u32 base = 0xF0000UL + 0x0100;
    u32 index = (address - base) / 2;
    return index < sizeof listed_modes / sizeof listed_modes[0] ? listed_modes[index] : 0xFFFF;
}

int intr(int vector, struct regs *r)
{
    u8 *buffer = 0;
    int i;
    if (vector == 0x1A) { r->ax = 0x8600; return 1; }
    if (vector != 0x10) return 1;
    if (r->di) for (i = 0; i < mapped_count; ++i)
        if (mapped_off[i] == r->di) buffer = (u8 *)mapped_ptr[i];
    switch (r->ax) {
    case 0x4F00:
        if (!buffer) return 1;
        mem_set(buffer, 0, 512); buffer[0]='V'; buffer[1]='E'; buffer[2]='S'; buffer[3]='A';
        put16(buffer + 4, 0x0300); put16(buffer + 14, 0x0100); put16(buffer + 16, 0xF000);
        put16(buffer + 18, 512); r->ax = 0x004F; return 0;
    case 0x4F03: r->bx = (u16)(current_mode | current_flags); r->ax = 0x004F; return 0;
    case 0x4F01:
        if (!buffer) return 1;
        set_mode(buffer, r->cx); r->ax = 0x004F; return 0;
    case 0x4F06: r->bx = active_pitch; r->cx = active_pitch / 4; r->dx = 1200; r->ax = 0x004F; return 0;
    case 0x4F15: r->ax = 0x014F; return 0;
    default: r->ax = 0x014F; return 1;
    }
}

static const struct disp_mode *find_mode(const struct disp_probe *p, u16 id)
{
    u16 i;
    for (i = 0; i < p->mode_count; ++i) if (p->modes[i].id == id) return &p->modes[i];
    return 0;
}
static void require(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main(void)
{
    struct disp_probe p;
    struct disp_mode_diag d;
    const struct disp_mode *m;

    require(disp_probe_init(&p, 1), "V86 probe sees VBE");
    require(p.current_width == 800 && p.current_height == 600 && p.current_pitch == 4000,
            "active LFB readback uses 4F06 pitch even when 4F01 D7 is clear");
    require(p.current_mode_flags == 0x4000, "4F03 D14 is retained");
    require(!find_mode(&p, 0x101), "indexed 640x480x8 is omitted from the desktop modes");
    m = find_mode(&p, 0x110);
    require(m && m->width == 640 && m->height == 480 && m->bpp == 15 &&
            m->pitch == 1280 && (m->flags & DISP_MODE_BANKED),
            "first 640x480 candidate is banked direct-color 15-bit");
    require(find_mode(&p, 0x111) && find_mode(&p, 0x112),
            "banked 640x480 direct-color 16/24-bit modes remain available");
    disp_probe_sort_modes(&p);
    require(p.modes[0].id == 0x110,
            "DISPLAY's first selectable geometry/depth is direct-color 640x480x15");
    require(!find_mode(&p, 0x118), "V86 does not offer linear-only mode with unusable WinA");
    m = find_mode(&p, 0x119);
    require(m && m->pitch == 3200 && (m->flags & DISP_MODE_BANKED),
            "V86 uses the validated banked pitch for dual-access mode");
    require(!(m->frame_bytes == 3328UL * 600UL), "V86 frame size does not use linear pitch");
    require(disp_probe_get_mode_diag(0x119, &d) && d.window_a_attributes == 5 &&
            d.window_b_attributes == 7 && d.granularity_kb == 16,
            "mode diagnostics retain A/B and granularity fields");
    require(d.bank_masks[1] == 16 && d.linear_masks[1] == 24,
            "banked and linear RGB descriptors remain separate");
    m = find_mode(&p, 0x122);
    require(m && m->pitch == 3200 && (m->flags & DISP_MODE_BANKED) &&
            !(m->flags & DISP_MODE_LINEAR),
            "V86 accepts banked masks when the alternate linear masks overlap");

    require(disp_probe_init(&p, 0), "real-mode probe sees VBE");
    m = find_mode(&p, 0x118);
    require(m && m->pitch == 3328 && (m->flags & DISP_MODE_LINEAR) &&
            !(m->flags & DISP_MODE_BANKED), "real-mode probe can offer valid linear-only mode");
    m = find_mode(&p, 0x119);
    require(m && m->pitch == 3328 && (m->flags & DISP_MODE_LINEAR) &&
            (m->flags & DISP_MODE_BANKED), "real-mode dual-access mode uses linear metadata");
    m = find_mode(&p, 0x120);
    require(m && m->pitch == 3200 && (m->flags & DISP_MODE_BANKED),
            "banked-only mode remains selectable outside V86");
    m = find_mode(&p, 0x122);
    require(m && m->pitch == 3200 && (m->flags & DISP_MODE_BANKED) &&
            !(m->flags & DISP_MODE_LINEAR),
            "invalid linear masks fall back to the valid banked descriptor");
    puts("display_probe mode access model: PASS");
    return 0;
}
