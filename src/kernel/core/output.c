/* Log lines, evidence records and the panic path.
 * Records use the f0-acceptance grammar; the panic path allocates nothing,
 * takes no lock except a re-entry guard and never touches storage.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/bootlog.h>

static char run_id[9] = "00000000";
static uint32_t seq;

void kputs_raw(const char *s)
{
    size_t n = strlen(s);
    serial_write(s, n);
    console_write(s, n);
}

/* Ordinary output runs with interrupts enabled: the kernel is not
 * preemptible, interrupt handlers never print, and the panic path is the
 * only other writer (with interrupts off). Holding IF clear while polling
 * the UART would lose timer ticks on real hardware. */
void klog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    bootlog_capture(buf, (size_t)n + 1);
    kputs_raw(buf);
}

void rec_set_run(const char *run8)
{
    for (int i = 0; i < 8; i++)
        run_id[i] = run8[i];
    run_id[8] = 0;
}

static uint32_t next_seq(void)
{
    uint32_t f = irq_save();
    uint32_t s = ++seq;
    irq_restore(f);
    return s;
}

/* A record above 240 bytes is never truncated silently: an ERROR record
 * replaces it so the evidence fails loudly. */
static void emit_line(const char *probe, const char *event, const char *extra, bool ordinary)
{
    char buf[256];
    uint32_t s = next_seq();
    int n = ksnprintf(buf, sizeof(buf), "CIUKI_TEST v=1 run=%s seq=%06u probe=%s event=%s%s%s",
                      run_id, s, probe, event, extra && *extra ? " " : "", extra ? extra : "");
    if (n > 240)
        n = ksnprintf(buf, sizeof(buf), "CIUKI_TEST v=1 run=%s seq=%06u probe=%s event=ERROR reason=record_overflow length=%d",
                      run_id, s, probe, n);
    buf[n] = '\n';
    buf[n + 1] = 0;
    if (ordinary) bootlog_capture(buf, (size_t)n + 1);
    kputs_raw(buf);
}

void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[240];
    extra[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(extra, sizeof(extra), fmt, ap);
        va_end(ap);
    }
    emit_line(probe, event, extra, true);
}

void rec_emit_panicsafe(const char *probe, const char *event, const char *extra)
{
    emit_line(probe, event, extra, false);
}

static volatile int in_panic;

static __attribute__((noreturn)) void halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

void panic(const char *fmt, ...)
{
    cli();
    if (in_panic++)
        halt_forever();
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    for (char *p = msg; *p; p++)
        if (*p == ' ')
            *p = '_';
    char extra[200];
    ksnprintf(extra, sizeof(extra), "vector=none reason=%s", msg);
    console_set_color(0xFFFFFF, 0x800000);
    kputs_raw("Ciuki VMM panic: halting.\n");
    rec_emit_panicsafe("panic", "PANIC", extra);   /* last output */
    halt_forever();
}

void panic_frame(struct trap_frame *tf, const char *why)
{
    cli();
    if (in_panic++)
        halt_forever();
    uint32_t cr2 = read_cr2();
    char extra[220];
    ksnprintf(extra, sizeof(extra),
              "vector=%u error=%08x eip=%08x cr2=%08x storage_delta=0 allocations=0 locks=0 task=%s why=%s",
              tf->vector, tf->err, tf->eip, cr2,
              g_current ? g_current->name : "boot", why);
    console_set_color(0xFFFFFF, 0x800000);
    char regs[200];
    ksnprintf(regs, sizeof(regs),
              "group=registers eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x",
              tf->eax, tf->ebx, tf->ecx, tf->edx, tf->esi, tf->edi, tf->ebp);
    rec_emit_panicsafe("panic", "DATA", regs);
    kputs_raw("Ciuki VMM panic: halting.\n");
    rec_emit_panicsafe("panic", "PANIC", extra);   /* last output */
    halt_forever();
}

/* Double fault: entered through the task gate on a private TSS. */
void df_handler(void)
{
    if (in_panic++)
        halt_forever();
    char extra[200];
    ksnprintf(extra, sizeof(extra), "vector=8 error=00000000 eip=%08x esp=%08x cr2=%08x why=double_fault",
              g_tss.eip, g_tss.esp, read_cr2());
    console_set_color(0xFFFFFF, 0x800000);
    kputs_raw("Ciuki VMM double fault: halting.\n");
    rec_emit_panicsafe("panic", "PANIC", extra);   /* last output */
    halt_forever();
}
