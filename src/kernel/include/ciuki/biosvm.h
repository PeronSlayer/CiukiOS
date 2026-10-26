/* Serialized firmware input VM. Initialization is explicit, in thread context.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BIOSVM_H
#define CIUKI_BIOSVM_H
#include <ciuki/v86.h>
struct task;
struct kwait;

#define BIOSVM_SCRATCH 0x10000u
#define BIOSVM_STACK 0x11000u
#define BIOSVM_MOUSE_OFFSET 0x100u
#define BIOSVM_MOUSE_RING 0x400u
#define BIOSVM_MOUSE_CAP 32u
struct biosvm_mouse_ring {
    uint16_t head, tail, lost;
    uint8_t packets[BIOSVM_MOUSE_CAP][3];
} __attribute__((packed));
struct biosvm_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint16_t ds, es, fs, gs, flags;
    uint8_t interrupt;
};
enum biosvm_backend { BIOSVM_OFF, BIOSVM_READY, BIOSVM_DISABLED_BACKEND };
/* Sole controller lease. Uses PMM's existing fixed BIOS reservation; page
 * zero/EBDA are borrowed and must never reach as_destroy/pmm_free. */
int biosvm_init(void);
/* Boot-only handoff after adapter failure. Stop input producers and release
 * clean, idle leases; never reclaim quarantine or an executing firmware VM.
 * No physical register writes. The VM remains available for video. */
int biosvm_input_fallback(void);
int biosvm_call(struct biosvm_regs *regs, uint32_t deadline_ms);
/* Kernel-only real-mode APM broker. INT15/5300..530F and 5380/BH=7F query;
 * 5302/5303 PM connections are refused. Serialized V86, 1..2000 ms budget
 * including mutex/queue admission. Faults/timeouts quarantine APM for this
 * boot, including disconnect; input and video keep separate fault latches.
 * Native-owned ports remain denied. No DOS entry. */
int biosvm_apm_call(struct biosvm_regs *regs, uint32_t deadline_ms);
/* Kernel-only, fixed INT15/5300 installation check, 100 ms. Does not
 * connect, accept caller registers, or widen input/DOS firmware access. */
int biosvm_apm_probe(struct biosvm_regs *out);
/* fx-18: the chipset power-management window the APM BIOS may use during
 * APM calls only (ciuki/apm_ports.h), set by the APM worker before its
 * first call. Each distinct admitted port and the first denials are
 * logged; the counters below are cumulative. */
struct apm_port_window;
int biosvm_apm_set_window(const struct apm_port_window *w);
struct biosvm_apm_io { uint64_t allowed, writes, absorbed, denied; };
void biosvm_apm_io_snapshot(struct biosvm_apm_io *out);
/* fx-25: why the last BIOS VM call failed (zeroed after a successful one).
 * reason: port_denied, halted, halted_at_sentinel, timeout, page_fault,
 * emulation (an instruction the monitor refused), cpu_fault or error; budget_ms is the caller's, deadline_ms what remained
 * after queueing. Also logged as one `[biosvm] step=failure` record. */
struct biosvm_failure {
    int error;
    uint16_t function, cs, ip, ss, sp, port;
    uint8_t interrupt, vector, width;
    bool write;
    const char *reason;
    uint32_t elapsed_ms, deadline_ms, budget_ms;
};
void biosvm_last_failure(struct biosvm_failure *out);
/* Only fbdev's quiesced transaction calls this kernel-only entry. 4F02
 * (LFB), 4F01 into private scratch, 4F03 readback; serialized as one request.
 * No allowlist widening for biosvm_call or DOS guests. */
/* Initialize VM dependencies before LFB revocation; switch claims video
 * resources after acceleration stops, then releases on completed calls. */
int biosvm_video_prepare(void);
int biosvm_video_switch(uint16_t mode, uint8_t info[256]);
int biosvm_video_dpms(unsigned state); /* VBE states 0, 1, 4 */
/* Full-screen transaction, only after LFB revocation. Retains VGA leases;
 * save/restore VBE state mask 000Fh, bounded private scratch, no guest API. */
int biosvm_video_fullscreen_begin(uint8_t mode);
int biosvm_video_fullscreen_end(uint16_t desktop_mode, uint8_t info[256]);
enum biosvm_backend biosvm_backend_state(void);
void biosvm_stats(struct v86_stats *out);
/* RTC service is outside f1-07: supply its validated cache before first use.
 * No native CMOS reads, NMI-mask changes or fabricated time in this VM. */
int biosvm_set_rtc_cache(const uint8_t values[128]);
void biosvm_set_input_service(void (*service)(void));
/* Observes the single trapped physical read, never consumes the controller.
 * status=0 if no unconsumed status read exists. keyboard_irq identifies the
 * reflected IRQ1 handler, excluding ordinary command/reply polling. The
 * callback must not block, allocate or enter firmware. AUX stays callback-fed. */
void biosvm_set_input_observer(void (*observer)(uint8_t status, uint8_t byte, bool keyboard_irq));
unsigned biosvm_mouse_packets(uint8_t (*out)[3], unsigned max, unsigned *lost);
/* Cheap mapped-memory checks, no firmware entry or controller reads. */
bool biosvm_keyboard_pending(void);
bool biosvm_mouse_pending(void);
uint64_t biosvm_input_reflections(void);
/* Persistent queue, published before starting its consumer. Reflection and
 * completed input service wake it; conditions still decide whether to run. */
void biosvm_set_input_wait(struct kwait *wait);

/* Cumulative delivery evidence. Slots 0/1 are IRQ1/IRQ12. entries/done
 * count standalone ISR entries/IRETs to the sentinel; reflected also counts
 * IRQs nested in an INT16/C2 call. Snapshots never read controller ports. */
struct biosvm_input_diag {
    uint64_t arrivals[2], dispatched[2], eois[2], queued[2], reflected[2];
    uint64_t entries[2], done[2], port60, signals, budget_yields;
    uint16_t pending, bda_head, bda_tail, mouse_head, mouse_tail, mouse_lost;
    uint8_t physical_imr[2], imr[2], irr[2], isr[2], base[2];
    uint8_t last_status, last_byte;
    enum v86_state state;
    bool active, disabled;
};
/* Dispatcher accounting only: called with IF clear, without port access. */
void biosvm_account_irq(unsigned irq, bool eoi, bool handled);
void biosvm_input_snapshot(struct biosvm_input_diag *out);

/* V86-only arch/scheduler hooks. Defaults preserve every F0 frame/path. */
void biosvm_trap(struct trap_frame *tf);
uint32_t biosvm_task_cr3(const struct task *t, uint32_t normal);
uint32_t biosvm_task_esp0(const struct task *t, uint32_t normal);

#define BIOSVM_MAP_COUNT 6u
struct biosvm_mapping { uint32_t start, end, flags; }; /* page-aligned, end exclusive */
struct biosvm_selftest_report {
    uint8_t pic_before[2], pic_after[2], pit_before, pit_after;
    uint32_t disallowed, timeouts;
    int policy_result, denied_result, timeout_result, later_result;
    bool pic_unchanged, pit_unchanged, disabled;
    struct biosvm_mapping mappings[BIOSVM_MAP_COUNT];
    bool mappings_ok;
};
int biosvm_selftest(struct biosvm_selftest_report *r);
/* Test-only reset of synthetic execution state; NEVER reclaims quarantined
 * hardware. Refuses after a live lease fault: registry quarantine is final.
 * The lead must use a fresh boot for a live firmware timeout. */
int biosvm_reset_for_test(void);
#endif
