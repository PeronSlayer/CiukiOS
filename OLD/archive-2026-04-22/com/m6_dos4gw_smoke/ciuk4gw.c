#include "services.h"

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
	ciuki_int21_regs_t regs;
	uint8_t exit_code = 0x48U;

	if (!svc || !ctx) {
		return;
	}

	if (svc->print) {
		svc->print("CIUK4GW: DPMI host-query smoke\n");
	}

	regs.ax = 0x1687U;
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

	if (svc->int2f) {
		svc->int2f(ctx, &regs);
	}

	if (regs.carry == 0U && regs.ax == 0x0000U && regs.cx == 0x0090U) {
		exit_code = 0x47U;
	}

	regs.ax = (uint16_t)(0x4C00U | exit_code);
	regs.bx = 0U;
	regs.cx = 0U;
	regs.dx = 0U;
	regs.si = 0U;
	regs.di = 0U;
	regs.ds = 0U;
	regs.es = 0U;
	regs.carry = 0U;

	if (svc->int21) {
		svc->int21(ctx, &regs);
		return;
	}

	if (svc->int21_4c) {
		svc->int21_4c(ctx, exit_code);
	}
}