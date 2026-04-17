/*
 * dosmode13.COM — CiukiOS M-V2.4 sample: VGA mode 0x13 via services ABI.
 *
 * Sets mode 0x13 (320x200x8), writes a palette-indexed gradient into the
 * mode 0x13 plane through gfx->mode13_put_pixel, calls gfx->present to
 * commit (upscale + letterbox onto the real GOP fb), logs a marker to
 * serial, then returns via INT 21h AH=4Ch.
 */

#include "services.h"

static void print_line(ciuki_services_t *svc, const char *s) {
    svc->print(s);
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;

    if (!gfx || !gfx->set_mode || !gfx->mode13_put_pixel || !gfx->present) {
        print_line(svc, "[dosmode13] FAIL: gfx mode services unavailable\n");
        goto exit;
    }

    if (!gfx->set_mode(0x13U)) {
        print_line(svc, "[dosmode13] FAIL: set_mode 0x13\n");
        goto exit;
    }

    /* Gradient: x drives hue through the color-cube region, y shifts band. */
    for (unsigned y = 0; y < 200U; y++) {
        for (unsigned x = 0; x < 320U; x++) {
            unsigned r = (x * 6U) / 320U;
            unsigned g = (y * 6U) / 200U;
            unsigned b = ((x + y) * 6U) / 520U;
            unsigned idx = 32U + r * 36U + g * 6U + (b % 6U);
            if (idx > 255U) idx = 255U;
            gfx->mode13_put_pixel(x, y, (unsigned char)idx);
        }
    }

    /* Optional: custom palette entry 255 → bright white. */
    if (gfx->set_palette) {
        unsigned char white[3] = { 0x3F, 0x3F, 0x3F };
        gfx->set_palette(255U, 1U, white);
    }

    gfx->present();
    print_line(svc, "[dosmode13] OK\n");

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
