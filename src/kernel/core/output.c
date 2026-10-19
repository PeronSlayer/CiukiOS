/* Log lines, evidence records and the panic path.
 * Records use the f0-acceptance grammar; the panic path allocates nothing,
 * uses an optional bounded raw-sector sink on an idle channel.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/bootlog.h>
#include <ciuki/timing.h>
#include <ciuki/blackbox.h>
#include <ciuki/ledger.h>
#pragma weak blackbox_capture
#pragma weak blackbox_panic

static char run_id[9] = "00000000";
static uint32_t seq, premature_records;
static bool run_selected, run_begun, premature_noted;

uint32_t rec_premature_records(void) { return premature_records; }
bool rec_active(void) { return run_selected && run_begun; }

void blackbox_serial_replay(const char *s,size_t n) { serial_write(s,n); }

void kputs_raw(const char *s)
{
    size_t n = strlen(s);
    serial_write(s, n);
    console_write(s, n);
}

/* Periodic UART publications leave queue capacity reserved for evidence.
 * Disk priority is independent: klog_periodic summaries remain protected.
 * Boot/event logs wait in task context; IRQ/early publications never sleep
 * or poll the UART. Console work follows the bounded serial publication. */
static void log_line(const char *fmt, va_list ap, bool periodic, bool verbose)
{
    char buf[256];
    int n = kvsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    if (ledger_lifecycle) ledger_lifecycle(buf,(size_t)n);
    if (blackbox_capture) blackbox_capture(buf,(size_t)n+1);
    if (verbose) bootlog_capture_verbose(buf, (size_t)n + 1);
    else if (periodic) bootlog_capture_ring(buf, (size_t)n + 1);
    else bootlog_capture(buf, (size_t)n + 1);
    size_t wire_bytes = (size_t)n + 1;
    for (int i = 0; i <= n; i++) if (buf[i] == '\n') wire_bytes++;
    uint32_t flags = !periodic && serial_evidence_begin ? serial_evidence_begin(wire_bytes) : irq_save();
    serial_write(buf, (size_t)n + 1);
    irq_restore(flags);
    console_write(buf, (size_t)n + 1);
}

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); log_line(fmt, ap, false, false); va_end(ap);
}

void klog_periodic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); log_line(fmt, ap, true, false); va_end(ap);
}

/* High-volume details are opt-in and droppable on the disk sink. */
void klog_diag(const char *fmt, ...)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE)) return;
    va_list ap;
    va_start(ap,fmt);log_line(fmt,ap,true,true);va_end(ap);
}

void rec_set_run(const char *run8)
{
    for (int i = 0; i < 8; i++)
        run_id[i] = run8[i];
    run_id[8] = 0;
    run_selected = true;
    run_begun = false;
}

/* A record above 240 bytes is never truncated silently: an ERROR record
 * replaces it so the evidence fails loudly. */
static void emit_line(const char *probe, const char *event, const char *extra, bool ordinary, bool periodic, bool disk_only, bool evidence, bool capture_blackbox, bool verbose)
{
    char buf[256];
    /* Reserve room before assigning a sequence: a queue wait can dispatch
     * another evidence producer. Formatting/publication is bounded to this
     * buffer and atomic against IRQ producers. Console work follows IF restore. */
    uint32_t flags = evidence && serial_evidence_begin ? serial_evidence_begin(2 * sizeof(buf)) : irq_save();
    if ((flags & 0x200) && crit_label) crit_label("record_publish");
    uint32_t s = ++seq;
    int n = ksnprintf(buf, sizeof(buf), "CIUKI_TEST v=1 run=%s seq=%06u probe=%s event=%s%s%s",
                      run_id, s, probe, event, extra && *extra ? " " : "", extra ? extra : "");
    if (n > 240)
        n = ksnprintf(buf, sizeof(buf), "CIUKI_TEST v=1 run=%s seq=%06u probe=%s event=ERROR reason=record_overflow length=%d",
                      run_id, s, probe, n);
    buf[n] = '\n';
    buf[n + 1] = 0;
    if (!ordinary && blackbox_panic) blackbox_panic(buf,(size_t)n+1);
    if (ordinary) {
        if (verbose) bootlog_capture_verbose(buf, (size_t)n + 1);
        else if (periodic) bootlog_capture_ring(buf, (size_t)n + 1);
        else bootlog_capture(buf, (size_t)n + 1);
    }
    if (!disk_only) serial_write(buf, (size_t)n + 1);
    irq_restore(flags);
    /* The RAM recorder owns a separate <=256-byte publication. Do not
     * extend the serial/sequence critical section or tie capture to UART
     * admission: whole-record serial drops still reach the black box. */
    if (capture_blackbox && blackbox_capture) blackbox_capture(buf,(size_t)n+1);
    if (!disk_only) console_write(buf, (size_t)n + 1);
}

void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    if (!run_selected || (!run_begun && strncmp(event, "BEGIN", sizeof("BEGIN")))) {
        premature_records++;
        if (!premature_noted) {
            premature_noted = true;
            klog("[records] dropped premature evidence record");
        }
        return;
    }
    run_begun = true;
    char extra[240];
    extra[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(extra, sizeof(extra), fmt, ap);
        va_end(ap);
    }
    emit_line(probe, event, extra, true, false, false, true, true, false);
}

void rec_emit_panicsafe(const char *probe, const char *event, const char *extra)
{
    emit_line(probe, event, extra, false, false, false, false, true, false);
}

void rec_diag(const char *probe, const char *fmt, ...)
{
    if (!(g_boot.flags & (CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE)) || (run_selected && !run_begun)) return;
    char extra[240];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    /* The VM heartbeat is a recurring runtime snapshot, even when queued
     * and later emitted with fmt="%s". Evidence rec_emit stays protected. */
    bool periodic = !strncmp(extra, "group=vm_heartbeat ", sizeof("group=vm_heartbeat ") - 1) ||
                    !strncmp(extra, "group=vm_perf_", sizeof("group=vm_perf_") - 1);
    emit_line(probe, "DATA", extra, true, periodic, false, !periodic, true, true);
}

static void diag_disk(const char *probe, const char *fmt, va_list ap, bool capture_blackbox)
{
    /* Always capture low-rate summaries in RAM, including quiet hardware
     * boots. No UART/console/VFS wait. Preserve selected-run BEGIN ordering. */
    if (run_selected && !run_begun) return;
    char extra[240];
    kvsnprintf(extra,sizeof(extra),fmt,ap);
    emit_line(probe,"DATA",extra,true,true,true,false,capture_blackbox,false);
}

void rec_diag_disk(const char *probe, const char *fmt, ...)
{
    va_list ap;
    va_start(ap,fmt); diag_disk(probe,fmt,ap,true); va_end(ap);
}

void rec_diag_disk_bootlog(const char *probe, const char *fmt, ...)
{
    /* Bounded five-second performance families are protected on disk; keep
     * their independent exclusion from the persistent black-box queue. */
    va_list ap;
    va_start(ap,fmt); diag_disk(probe,fmt,ap,false); va_end(ap);
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
    serial_panic();
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
    console_reclaim();
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
    serial_panic();
    uint32_t cr2 = read_cr2();
    char extra[220];
    ksnprintf(extra, sizeof(extra),
              "vector=%u error=%08x eip=%08x cr2=%08x storage_delta=0 allocations=0 locks=0 task=%s why=%s",
              tf->vector, tf->err, tf->eip, cr2,
              g_current ? g_current->name : "boot", why);
    console_reclaim();
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
    serial_panic();
    char extra[200];
    ksnprintf(extra, sizeof(extra), "vector=8 error=00000000 eip=%08x esp=%08x cr2=%08x why=double_fault",
              g_tss.eip, g_tss.esp, read_cr2());
    console_reclaim();
    console_set_color(0xFFFFFF, 0x800000);
    kputs_raw("Ciuki VMM double fault: halting.\n");
    rec_emit_panicsafe("panic", "PANIC", extra);   /* last output */
    halt_forever();
}
