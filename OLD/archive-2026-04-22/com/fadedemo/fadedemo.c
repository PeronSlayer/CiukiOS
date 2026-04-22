/*
 * fadedmo.COM — CiukiOS palette-fade demo (SR-VIDEO-002 DOOM-prep, v0.8.2).
 *
 * Enters mode 0x13, draws a simple palette-indexed background (concentric
 * bands), then performs two palette fades via the services ABI:
 *   1. Fade current palette -> bright red (0x00FF0000) in 16 steps.
 *   2. Fade red back -> black (0x00000000) in 16 steps.
 * Each step re-commits the plane via gfx->present().
 * Emits [fadedmo] OK on serial when done and returns via INT 21h AH=4Ch.
 */

#include "services.h"

static void print_line(ciuki_services_t *svc, const char *s) {
    svc->print(s);
}

static void busy_delay(unsigned cycles) {
    /* Compiler barrier; no sleep primitive is needed — each present call
     * already does an 8 MiB blit so the fade paces itself visually. */
    for (volatile unsigned i = 0; i < cycles; i++) { }
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;

    if (!gfx || !gfx->set_mode || !gfx->mode13_fill_rect ||
        !gfx->palette_fade || !gfx->present) {
        print_line(svc, "[fadedmo] FAIL: gfx services incomplete\n");
        goto exit;
    }

    if (!gfx->set_mode(0x13U)) {
        print_line(svc, "[fadedmo] FAIL: set_mode 0x13\n");
        goto exit;
    }

    /* Concentric bands using the 6x6x6 color-cube region (indices 32..247). */
    gfx->mode13_fill(0);
    for (unsigned band = 0; band < 10U; band++) {
        unsigned idx = 32U + band * 22U;
        if (idx > 247U) idx = 247U;
        unsigned inset = band * 10U;
        if (inset * 2U >= 320U || inset * 2U >= 200U) break;
        gfx->mode13_fill_rect(inset, inset,
                              320U - inset * 2U,
                              200U - inset * 2U,
                              (unsigned char)idx);
    }
    gfx->present();

    /* Fade -> red (blood flash). */
    for (unsigned s = 0; s <= 16U; s++) {
        gfx->palette_fade(0x00FF0000U, s, 16U);
        gfx->present();
        busy_delay(2000000U);
    }

    /* Fade -> black (screen wipe). */
    for (unsigned s = 0; s <= 16U; s++) {
        gfx->palette_fade(0x00000000U, s, 16U);
        gfx->present();
        busy_delay(2000000U);
    }

    print_line(svc, "[fadedmo] OK\n");

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
