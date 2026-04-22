#include "services.h"

static void clear_regs(ciuki_int21_regs_t *regs) {
	regs->ax = 0U;
	regs->bx = 0U;
	regs->cx = 0U;
	regs->dx = 0U;
	regs->si = 0U;
	regs->di = 0U;
	regs->ds = 0U;
	regs->es = 0U;
	regs->carry = 0U;
	regs->reserved[0] = 0U;
	regs->reserved[1] = 0U;
	regs->reserved[2] = 0U;
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
	ciuki_int21_regs_t regs;
	uint8_t exit_code = 0x4FU;

	if (!svc || !ctx) {
		return;
	}

	clear_regs(&regs);
	regs.ax = 0x1687U;
	if (svc->int2f) {
		svc->int2f(ctx, &regs);
	}

	if (regs.carry == 0U && regs.ax == 0x0000U && (regs.es != 0U || regs.di != 0U)) {
		clear_regs(&regs);
		regs.ax = 0x0306U;
		if (svc->int31) {
			svc->int31(ctx, &regs);
		}
		if (regs.carry == 0U &&
			(regs.bx != 0U || regs.cx != 0U) &&
			(regs.si != 0U || regs.di != 0U)) {
			exit_code = 0x4EU;
		}
	}

	if (svc->int21_4c) {
		svc->int21_4c(ctx, exit_code);
	}
}