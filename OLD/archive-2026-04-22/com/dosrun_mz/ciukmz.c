#include "services.h"

/*
 * CIUKMZ.EXE payload.
 *
 * Wrapped by tools/mkciukmz_exe into a deterministic MZ (.EXE) file that
 * carries the CIUKEX64 native-dispatch marker. The stage2 MZ launch path
 * parses the MZ header, locates the marker, and calls this entry point.
 *
 * Avoid referencing any .rodata/.data to stay VA-independent inside the
 * single-page MZ wrapper (no absolute address fixups needed at runtime).
 */
void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    ciuki_int21_regs_t regs;

    if (!svc || !ctx) {
        return;
    }

    regs.ax = 0x4C2BU;
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
        return;
    }

    if (svc->int21_4c) {
        svc->int21_4c(ctx, 0x2BU);
    }
}
