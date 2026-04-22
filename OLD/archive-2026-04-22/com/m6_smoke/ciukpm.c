#include "services.h"

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
	ciuki_int21_regs_t regs;

	if (!svc || !ctx) {
		return;
	}

	if (svc->print) {
		svc->print("CIUKPM: m6 readiness smoke payload\n");
	}

	regs.ax = 0x4C36U;
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
		svc->int21_4c(ctx, 0x36U);
	}
}