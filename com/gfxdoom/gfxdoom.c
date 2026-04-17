/*
 * gfxdoom.COM — CiukiOS clipped patch + sampled column demo (SR-VIDEO-002).
 *
 * Validates the DOS-universality additions:
 *   - gfx->mode13_blit_scaled_clip      (signed/clipped patch placement)
 *   - gfx->mode13_draw_column_sampled_masked (sampled masked columns)
 *   - gfx->frame_counter       (monotonic counter bumped by present)
 *
 * Scene: one off-screen scaled patch clipped at the top-left, one centered
 * patch, then sampled masked columns across the bottom half.
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
        !gfx->mode13_blit_scaled_clip ||
        !gfx->mode13_draw_column_sampled_masked ||
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

    /* Off-screen patch: validates signed clipping at top-left. */
    gfx->mode13_blit_scaled_clip(g_src, 16U, 16U, 16U,
                                 -24, -12, 112U, 84U, 0U, 0U);
    /* Center patch: stable reference. */
    gfx->mode13_blit_scaled_clip(g_src, 16U, 16U, 16U,
                                 104, 40, 112U, 112U, 0U, 0U);
    gfx->present();

    /* Sampled columns: stretch a 64-row source to 96 rows, including a few
     * partially off-screen left columns to validate signed X clipping. */
    for (unsigned i = 0; i < 42U; i++) {
        int x = -8 + (int)i * 8;
        gfx->mode13_draw_column_sampled_masked(x, 104, 96U,
                                               g_col, 64U,
                                               0U,
                                               (64U << 16) / 96U,
                                               0U);
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
