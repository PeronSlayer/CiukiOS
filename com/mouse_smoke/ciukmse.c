/*
 * SR-MOUSE-001 — INT 33h smoke test COM.
 *
 * Exercises the mandatory DOS mouse subset wired into stage2:
 *   AX=0000h reset+status
 *   AX=0001h show cursor
 *   AX=0002h hide cursor
 *   AX=0003h get position + buttons
 *   AX=0004h set position (with clipping)
 *   AX=0007h set horizontal range
 *   AX=0008h set vertical range
 *
 * Emits deterministic serial markers so the external gate can verify
 * each step without framebuffer inspection. All markers start with
 * "[mouse] " for easy grep-based filtering.
 */

#include "bootinfo.h"
#include "handoff.h"
#include "services.h"

static void m_call(ciuki_dos_context_t *ctx, ciuki_services_t *svc,
                   ciuki_int21_regs_t *regs) {
    (void)ctx;
    if (svc && svc->int33) {
        svc->int33(ctx, regs);
    }
}

static void m_zero(ciuki_int21_regs_t *regs) {
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

static void say(ciuki_services_t *svc, const char *s) {
    if (svc && svc->serial_print) {
        svc->serial_print(s);
    }
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    ciuki_int21_regs_t r;
    int ok_all = 1;

    say(svc, "[mouse] smoke begin\n");

    if (!svc || !svc->int33) {
        say(svc, "[mouse] abi_missing int33=NULL\n");
        goto done;
    }

    /* 1) Reset — expect AX=0xFFFF, BX=0x0002 (2 buttons). */
    m_zero(&r);
    r.ax = 0x0000U;
    m_call(ctx, svc, &r);
    if (r.ax == 0xFFFFU && r.bx == 0x0002U) {
        say(svc, "[mouse] reset ok\n");
    } else {
        say(svc, "[mouse] reset fail\n");
        ok_all = 0;
    }

    /* 2) Show/Hide contract: show then hide should be symmetric, but we
     *    only verify that both calls complete without carry and that a
     *    subsequent get-position still works. */
    m_zero(&r);
    r.ax = 0x0001U;
    m_call(ctx, svc, &r);
    say(svc, r.carry ? "[mouse] show fail\n" : "[mouse] show ok\n");
    if (r.carry) ok_all = 0;

    m_zero(&r);
    r.ax = 0x0002U;
    m_call(ctx, svc, &r);
    say(svc, r.carry ? "[mouse] hide fail\n" : "[mouse] hide ok\n");
    if (r.carry) ok_all = 0;

    /* 3) Set position 100,80 then read it back. Default post-reset range
     *    is 0..639 / 0..199, so 100/80 must round-trip verbatim. */
    m_zero(&r);
    r.ax = 0x0004U;
    r.cx = 100U;
    r.dx = 80U;
    m_call(ctx, svc, &r);

    m_zero(&r);
    r.ax = 0x0003U;
    m_call(ctx, svc, &r);
    if (r.cx == 100U && r.dx == 80U && r.bx == 0U) {
        say(svc, "[mouse] setpos ok x=100 y=80 b=0\n");
    } else {
        say(svc, "[mouse] setpos fail\n");
        ok_all = 0;
    }

    /* 4) Range clipping. Tighten X to 0..319 and Y to 0..199, then try
     *    to park the cursor at (500,300). Expect clipped (319,199). */
    m_zero(&r);
    r.ax = 0x0007U;
    r.cx = 0U;
    r.dx = 319U;
    m_call(ctx, svc, &r);

    m_zero(&r);
    r.ax = 0x0008U;
    r.cx = 0U;
    r.dx = 199U;
    m_call(ctx, svc, &r);

    m_zero(&r);
    r.ax = 0x0004U;
    r.cx = 500U;
    r.dx = 300U;
    m_call(ctx, svc, &r);

    m_zero(&r);
    r.ax = 0x0003U;
    m_call(ctx, svc, &r);
    if (r.cx == 319U && r.dx == 199U) {
        say(svc, "[mouse] range ok clip_x=319 clip_y=199\n");
    } else {
        say(svc, "[mouse] range fail\n");
        ok_all = 0;
    }

    /* 5) Swapped-range normalization: calling AX=0007h with CX>DX must
     *    normalize internally. After this, setpos(50,0) should leave
     *    x=50 (inside the normalized 0..200 range). */
    m_zero(&r);
    r.ax = 0x0007U;
    r.cx = 200U;
    r.dx = 0U;   /* swapped on purpose */
    m_call(ctx, svc, &r);

    m_zero(&r);
    r.ax = 0x0004U;
    r.cx = 50U;
    r.dx = 0U;
    m_call(ctx, svc, &r);

    m_zero(&r);
    r.ax = 0x0003U;
    m_call(ctx, svc, &r);
    if (r.cx == 50U) {
        say(svc, "[mouse] swap_range ok\n");
    } else {
        say(svc, "[mouse] swap_range fail\n");
        ok_all = 0;
    }

done:
    if (ok_all) {
        say(svc, "[mouse] smoke done result=ok\n");
    } else {
        say(svc, "[mouse] smoke done result=fail\n");
    }

    /* Terminate via INT 21h AH=4Ch with the aggregated result code. */
    if (svc && svc->int21) {
        m_zero(&r);
        r.ax = (unsigned short)(0x4C00U | (ok_all ? 0U : 1U));
        svc->int21(ctx, &r);
    }
    /* Hard fallback in case int21 is missing. */
    if (svc && svc->terminate) {
        svc->terminate(ctx, (unsigned char)(ok_all ? 0U : 1U));
    }
    for (;;) {
        __asm__ volatile("hlt");
    }
}
