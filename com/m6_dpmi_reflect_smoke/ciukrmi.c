#include "services.h"

static void zero_regs(ciuki_int21_regs_t *regs) {
    uint8_t *p = (uint8_t *)regs;
    for (unsigned i = 0; i < sizeof(*regs); i++) {
        p[i] = 0U;
    }
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    ciuki_int21_regs_t regs;
    ciuki_int21_regs_t frame;
    uint8_t exit_code = 0x57U;

    if (!svc || !ctx) {
        return;
    }

    zero_regs(&regs);
    regs.ax = 0x1687U;
    if (svc->int2f) { svc->int2f(ctx, &regs); }
    if (regs.carry != 0U || regs.ax != 0x0000U) { goto done; }

    zero_regs(&regs);
    regs.ax = 0x0400U;
    if (svc->int31) { svc->int31(ctx, &regs); }
    if (regs.carry != 0U || regs.ax != 0x005AU) { goto done; }

    zero_regs(&frame);
    frame.ax = 0x3000U;

    zero_regs(&regs);
    regs.ax = 0x0300U;
    regs.bx = 0x0021U;
    regs.cx = 0x0000U;
    regs.es = ctx->psp_segment;
    regs.di = (uint16_t)((uint8_t *)&frame - (uint8_t *)ctx->image_linear);
    if (svc->int31) { svc->int31(ctx, &regs); }

    if (regs.carry != 0U) {
        goto done;
    }

    exit_code = 0x58U;
    if (frame.carry == 0U && frame.ax == 0x1606U && frame.bx == 0x0000U && frame.cx == 0x0000U) {
        exit_code = 0x59U;
    }

done:
    if (svc->int21_4c) {
        svc->int21_4c(ctx, exit_code);
    }
}