#include "services.h"

/*
 * CIUKLDT.EXE - DPMI allocate-LDT-descriptors callable smoke.
 *
 * Chain: INT 2Fh AX=1687h -> INT 31h AX=0400h -> AX=0000h.
 * Exit 0x52 only when the full chain succeeds and the allocated LDT
 * selector has the RPL=3 + TI=1 low-3-bits shape real extenders expect.
 */

static void zero_regs(ciuki_int21_regs_t *regs) {
    uint8_t *p = (uint8_t *)regs;
    for (unsigned i = 0; i < sizeof(*regs); i++) {
        p[i] = 0U;
    }
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    ciuki_int21_regs_t regs;
    uint8_t exit_code = 0x50U;

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

    zero_regs(&regs);
    regs.ax = 0x0000U;
    regs.cx = 0x0001U;
    if (svc->int31) { svc->int31(ctx, &regs); }

    exit_code = 0x51U;
    if (regs.carry == 0U && (regs.ax & 0x0007U) == 0x0007U) {
        exit_code = 0x52U;
    }

done:
    if (svc->int21_4c) {
        svc->int21_4c(ctx, exit_code);
    }
}
