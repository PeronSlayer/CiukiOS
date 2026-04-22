#include "services.h"

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    ciuki_int21_regs_t regs = {0};
    uint16_t handle_hi;
    uint16_t handle_lo;
    uint8_t exit_code = 0x50U;

    if (!svc || !ctx) {
        return;
    }

    regs.ax = 0x1687U;
    if (svc->int2f) { svc->int2f(ctx, &regs); }
    if (regs.carry != 0U || regs.ax != 0x0000U) { goto done; }

    regs = (ciuki_int21_regs_t){0};
    regs.ax = 0x0400U;
    if (svc->int31) { svc->int31(ctx, &regs); }
    if (regs.carry != 0U || regs.ax != 0x005AU) { goto done; }

    regs = (ciuki_int21_regs_t){0};
    regs.ax = 0x0501U;
    regs.bx = 0x0001U;
    regs.cx = 0x0000U;
    if (svc->int31) { svc->int31(ctx, &regs); }
    if (regs.carry != 0U || (regs.si == 0U && regs.di == 0U)) {
        exit_code = 0x55U;
        goto done;
    }

    handle_hi = regs.si;
    handle_lo = regs.di;

    regs = (ciuki_int21_regs_t){0};
    regs.ax = 0x0502U;
    regs.si = handle_hi;
    regs.di = handle_lo;
    if (svc->int31) { svc->int31(ctx, &regs); }
    if (regs.carry != 0U) {
        exit_code = 0x55U;
        goto done;
    }

    regs = (ciuki_int21_regs_t){0};
    regs.ax = 0x0502U;
    regs.si = handle_hi;
    regs.di = handle_lo;
    if (svc->int31) { svc->int31(ctx, &regs); }

    exit_code = 0x55U;
    if (regs.carry != 0U && regs.ax == 0x8023U) {
        exit_code = 0x56U;
    }

done:
    if (svc->int21_4c) {
        svc->int21_4c(ctx, exit_code);
    }
}