/*
 * gfxsmoke.COM — CiukiOS M-V2.3 graphics services ABI smoke test.
 *
 * Exercises ciuki_gfx_services_t from a COM binary: queries fb info,
 * wraps a frame, draws mixed primitives, prints a result line to serial,
 * and cleanly returns via INT 21h AH=4Ch.
 */

#include "services.h"

static void print_line(ciuki_services_t *svc, const char *s) {
    svc->print(s);
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;
    ciuki_fb_info_t info;

    if (!gfx) {
        print_line(svc, "[gfxsmoke] FAIL: gfx services not available\n");
        goto exit;
    }

    gfx->get_fb_info(&info);
    if (info.width == 0U || info.height == 0U) {
        print_line(svc, "[gfxsmoke] FAIL: fb size zero\n");
        goto exit;
    }

    gfx->begin_frame();

    /* Background quadrants */
    gfx->fill_rect(0, 0, info.width / 2U, info.height / 2U, 0x00101040U);
    gfx->fill_rect(info.width / 2U, 0,
                   info.width - info.width / 2U, info.height / 2U,
                   0x00401010U);
    gfx->fill_rect(0, info.height / 2U,
                   info.width / 2U, info.height - info.height / 2U,
                   0x00104010U);
    gfx->fill_rect(info.width / 2U, info.height / 2U,
                   info.width - info.width / 2U,
                   info.height - info.height / 2U,
                   0x00404010U);

    /* Outline rect */
    gfx->rect(info.width / 4U, info.height / 4U,
              info.width / 2U, info.height / 2U,
              0x00FFFFFFU);

    /* Diagonals */
    gfx->line(0, 0, (int)info.width - 1, (int)info.height - 1, 0x0000FFFFU);
    gfx->line((int)info.width - 1, 0, 0, (int)info.height - 1, 0x00FFFF00U);

    /* Centered filled circle */
    unsigned r = (info.width < info.height ? info.width : info.height) / 12U;
    gfx->fill_circle((int)(info.width / 2U), (int)(info.height / 2U),
                     r, 0x00FF6020U);
    gfx->circle((int)(info.width / 2U), (int)(info.height / 2U),
                r + 6U, 0x00FFFFFFU);

    /* Big triangle */
    gfx->fill_tri(
        (int)(info.width / 10U), (int)(info.height * 8U / 10U),
        (int)(info.width / 2U),  (int)(info.height / 10U),
        (int)(info.width * 9U / 10U), (int)(info.height * 8U / 10U),
        0x0060C0FFU);

    gfx->end_frame();

    print_line(svc, "[gfxsmoke] OK\n");

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
