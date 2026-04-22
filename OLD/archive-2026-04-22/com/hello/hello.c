#include "bootinfo.h"
#include "handoff.h"
#include "services.h"

static void com_int21_print(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *s) {
    ciuki_int21_regs_t regs;
    unsigned long long base = ctx->image_linear;
    unsigned long long ptr = (unsigned long long)(const void *)s;
    unsigned long long off64;

    if (!svc->int21 || ptr < base) {
        svc->print(s);
        return;
    }

    off64 = ptr - base;
    if (off64 > 0xFFFFULL) {
        svc->print(s);
        return;
    }

    regs.ax = 0x0900U;
    regs.bx = 0U;
    regs.cx = 0U;
    regs.dx = (unsigned short)off64;
    regs.si = 0U;
    regs.di = 0U;
    regs.ds = 0U;
    regs.es = 0U;
    regs.carry = 0U;
    regs.reserved[0] = 0U;
    regs.reserved[1] = 0U;
    regs.reserved[2] = 0U;

    svc->int21(ctx, &regs);
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    static const char line0[] = "================================$\n";
    static const char line1[] = "  Hello from INIT.COM!$\n";
    static const char line2[] = "  CiukiOS M1 - DOS-like .COM$\n";
    static const char line3[] = "================================$\n";
    static const char line4[] = "\nCOM executed successfully.$\n";
    static const char line5[] = "PSP segment: 0x$";
    static const char line6[] = "\n";
    static const char line7[] = "Tail: $";
    static const char line8[] = "Returning to shell...$\n";
    ciuki_int21_regs_t regs;

    svc->cls();
    com_int21_print(ctx, svc, line0);
    com_int21_print(ctx, svc, line1);
    com_int21_print(ctx, svc, line2);
    com_int21_print(ctx, svc, line3);
    com_int21_print(ctx, svc, line4);
    com_int21_print(ctx, svc, line5);
    svc->print_hex64((unsigned long long)ctx->psp_segment);
    com_int21_print(ctx, svc, line6);
    if (ctx->command_tail_len > 0) {
        com_int21_print(ctx, svc, line7);
        svc->print(ctx->command_tail);
        com_int21_print(ctx, svc, line6);
    }
    com_int21_print(ctx, svc, line8);

    if (svc->int21) {
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
        svc->int21(ctx, &regs);
    } else {
        svc->int21_4c(ctx, 0x00);
    }
}
