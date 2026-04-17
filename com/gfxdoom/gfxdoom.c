/*
 * gfxdoom.COM — CiukiOS scaled-blit + masked-column demo (SR-VIDEO-002 v0.8.4).
 *
 * Validates the DOS-universality additions:
 *   - gfx->mode13_blit_scaled  (nearest-neighbor HUD/title-style upscale)
 *   - gfx->mode13_draw_column_masked (DOOM-style masked column, chroma-key)
 *   - gfx->frame_counter       (monotonic counter bumped by present)
 *
 * Scene: a 16x16 checkerboard source is scaled to 160x100 centered, then
 * 40 masked columns are drawn on top with alternating transparent holes.
 * Emits [gfxdoom] OK + frame count on serial. Exits via INT 21h AH=4Ch.
 */

#include "services.h"

/* 16x16 indexed source: quadrant-split colors. */
static unsigned char g_src[16 * 16];

/* One 64-row column with alternating stripes + transparent (0) holes. */
static unsigned char g_col[64];

static void build_assets(void) {
    for (unsigned y = 0; y < 16U; y++) {
        for (unsigned x = 0; x < 16U; x++) {
            unsigned idx;
            if (x < 8U && y < 8U) idx = 40U;         /* light red   */
            else if (x >= 8U && y < 8U) idx = 120U;   /* green       */
            else if (x < 8U && y >= 8U) idx = 200U;   /* light blue  */
            else idx = 247U;                          /* near-white  */
            /* Checker sub-pattern for scaling visibility. */
            if (((x / 2U) ^ (y / 2U)) & 1U) idx += 4U;
            g_src[y * 16U + x] = (unsigned char)idx;
        }
    }
    for (unsigned i = 0; i < 64U; i++) {
        /* Alternate stripes; every 4th pixel = 0 = transparent. */
        g_col[i] = (unsigned char)((i & 3U) == 3U ? 0U : (64U + (i * 3U) % 150U));
    }
}

static void u32_to_dec(unsigned int v, char *out) {
    char buf[16];
    int n = 0;
    if (v == 0U) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0U && n < 15) {
        buf[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    int j = 0;
    while (n > 0) out[j++] = buf[--n];
    out[j] = 0;
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;
    char num[16];

    if (!gfx || !gfx->set_mode || !gfx->mode13_fill ||
        !gfx->mode13_blit_scaled || !gfx->mode13_draw_column_masked ||
        !gfx->frame_counter || !gfx->present) {
        svc->print("[gfxdoom] FAIL: gfx services incomplete\n");
        goto exit;
    }

    if (!gfx->set_mode(0x13U)) {
        svc->print("[gfxdoom] FAIL: set_mode 0x13\n");
        goto exit;
    }

    build_assets();

    unsigned int frames_before = gfx->frame_counter();

    /* Background. */
    gfx->mode13_fill(16U); /* dark blue-ish */
    gfx->present();

    /* Scaled blit: 16x16 -> 160x100 centered at (80, 50). */
    gfx->mode13_blit_scaled(g_src, 16U, 16U, 16U,
                            80U, 50U, 160U, 100U, 0U, 0U);
    gfx->present();

    /* 40 masked columns at y=130, h=64, x stride of 8 starting at x=0.
     * Transparent index 0 means background shows through. */
    for (unsigned i = 0; i < 40U; i++) {
        gfx->mode13_draw_column_masked(i * 8U, 130U, 64U, g_col, 0U);
    }
    gfx->present();

    unsigned int frames_after = gfx->frame_counter();
    unsigned int delta = frames_after - frames_before;

    svc->print("[gfxdoom] frames=");
    u32_to_dec(delta, num);
    svc->print(num);
    svc->print("\n[gfxdoom] OK\n");

exit:
    regs.ax = 0x4C00U;
    regs.bx = 0U;
    regs.cx = 0U;
    regs.dx = 0U;
    regs.si = 0U;
    regs.di = 0U;
    regs.ds = 0U;
    regs.es = 0U;
    regs.carry = 0U;
    regs.reserved[0] = 0U;
    regs.reserved[1] = 0U;
    regs.reserved[2] = 0U;
    if (svc->int21) {
        svc->int21(ctx, &regs);
    } else if (svc->int21_4c) {
        svc->int21_4c(ctx, 0x00);
    } else {
        svc->terminate(ctx, 0x00);
    }
}
