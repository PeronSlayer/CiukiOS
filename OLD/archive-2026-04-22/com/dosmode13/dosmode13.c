/*
 * dosmode13.COM — CiukiOS SR-VIDEO-003: VGA mode 0x13 frame checkpoint.
 *
 * Sets mode 0x13 (320x200x8), draws a deterministic multi-region test
 * frame into the indexed plane, presents it through the GOP-backed
 * upscale path, emits serial markers proving the full pipeline, and
 * returns via INT 21h AH=4Ch.
 *
 * Frame layout (320x200):
 *   Row   0- 39: sky gradient (palette indices 32..67, horizontal sweep)
 *   Row  40- 79: four colored rectangles (red/green/blue/yellow bands)
 *   Row  80-139: 6x6x6 color-cube palette sweep (full indexed spectrum)
 *   Row 140-179: vertical gradient bar (greyscale ramp indices 16..31)
 *   Row 180-199: bottom border with checkerboard marker pattern
 */

#include "services.h"

static void print_line(ciuki_services_t *svc, const char *s) {
    svc->print(s);
}

/* Draw a filled rectangle using mode13_fill_rect if available, else
 * fall back to per-pixel writes. */
static void draw_rect(const ciuki_gfx_services_t *gfx,
                       unsigned x, unsigned y, unsigned w, unsigned h,
                       unsigned char idx) {
    if (gfx->mode13_fill_rect) {
        gfx->mode13_fill_rect(x, y, w, h, idx);
        return;
    }
    for (unsigned row = y; row < y + h && row < 200U; row++)
        for (unsigned col = x; col < x + w && col < 320U; col++)
            gfx->mode13_put_pixel(col, row, idx);
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;

    if (!gfx || !gfx->set_mode || !gfx->mode13_put_pixel || !gfx->present) {
        print_line(svc, "[dosmode13] FAIL: gfx services unavailable\n");
        goto exit;
    }

    /* ---- 1. Switch to mode 0x13 ---- */
    if (!gfx->set_mode(0x13U)) {
        print_line(svc, "[dosmode13] FAIL: set_mode 0x13\n");
        goto exit;
    }
    print_line(svc, "[dosmode13] mode 0x13 active\n");

    /* ---- 2. Region A: sky gradient (rows 0-39) ---- */
    for (unsigned y = 0; y < 40U; y++) {
        for (unsigned x = 0; x < 320U; x++) {
            /* Smooth horizontal sweep through color-cube blue range */
            unsigned idx = 32U + (x * 36U) / 320U;
            if (idx > 67U) idx = 67U;
            gfx->mode13_put_pixel(x, y, (unsigned char)idx);
        }
    }
    print_line(svc, "[dosmode13] region A drawn (sky gradient)\n");

    /* ---- 3. Region B: four colored rectangles (rows 40-79) ---- */
    /* Red band   */ draw_rect(gfx,   0, 40, 80, 40, 32U + 5*36U);       /* bright red */
    /* Green band */ draw_rect(gfx,  80, 40, 80, 40, 32U + 5*6U);        /* bright green */
    /* Blue band  */ draw_rect(gfx, 160, 40, 80, 40, 32U + 5U);          /* bright blue */
    /* Yellow band*/ draw_rect(gfx, 240, 40, 80, 40, 32U + 5*36U + 5*6U);/* yellow */
    print_line(svc, "[dosmode13] region B drawn (color bands)\n");

    /* ---- 4. Region C: color-cube palette sweep (rows 80-139) ---- */
    for (unsigned y = 80; y < 140U; y++) {
        for (unsigned x = 0; x < 320U; x++) {
            /* Walk linearly through the 216 color-cube entries */
            unsigned linear = ((y - 80U) * 320U + x);
            unsigned idx = 32U + (linear % 216U);
            gfx->mode13_put_pixel(x, y, (unsigned char)idx);
        }
    }
    print_line(svc, "[dosmode13] region C drawn (palette sweep)\n");

    /* ---- 5. Region D: greyscale ramp (rows 140-179) ---- */
    for (unsigned y = 140; y < 180U; y++) {
        for (unsigned x = 0; x < 320U; x++) {
            /* Greyscale palette indices 16..31 */
            unsigned idx = 16U + (x * 16U) / 320U;
            if (idx > 31U) idx = 31U;
            gfx->mode13_put_pixel(x, y, (unsigned char)idx);
        }
    }
    print_line(svc, "[dosmode13] region D drawn (greyscale ramp)\n");

    /* ---- 6. Region E: checkerboard marker (rows 180-199) ---- */
    for (unsigned y = 180; y < 200U; y++) {
        for (unsigned x = 0; x < 320U; x++) {
            unsigned char idx = ((x / 8U + y / 8U) & 1U) ? 15U : 0U;
            gfx->mode13_put_pixel(x, y, idx);
        }
    }
    print_line(svc, "[dosmode13] region E drawn (checkerboard)\n");

    /* ---- 7. Present the completed frame ---- */
    int ok = gfx->present();
    if (ok) {
        print_line(svc, "[dosmode13] frame checkpoint PASS\n");
    } else {
        print_line(svc, "[dosmode13] frame checkpoint FAIL (present error)\n");
    }

    /* ---- 8. Return to text mode if supported ---- */
    if (gfx->set_mode(0x03U)) {
        print_line(svc, "[dosmode13] restored text mode 0x03\n");
    }

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
