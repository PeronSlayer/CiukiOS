/* Serialized firmware input VM. Initialization is explicit, in thread context.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BIOSVM_H
#define CIUKI_BIOSVM_H
#include <ciuki/v86.h>
struct task;

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
int biosvm_call(struct biosvm_regs *regs, uint32_t deadline_ms);
enum biosvm_backend biosvm_backend_state(void);
void biosvm_stats(struct v86_stats *out);
/* RTC service is outside f1-07: supply its validated cache before first use.
 * No native CMOS reads, NMI-mask changes or fabricated time in this VM. */
int biosvm_set_rtc_cache(const uint8_t values[128]);
void biosvm_set_input_service(void (*service)(void));
unsigned biosvm_mouse_packets(uint8_t (*out)[3], unsigned max, unsigned *lost);

/* V86-only arch/scheduler hooks. Defaults preserve every F0 frame/path. */
void biosvm_trap(struct trap_frame *tf);
uint32_t biosvm_task_cr3(const struct task *t, uint32_t normal);
uint32_t biosvm_task_esp0(const struct task *t, uint32_t normal);

struct biosvm_selftest_report {
    uint8_t pic_before[2], pic_after[2], pit_before, pit_after;
    uint32_t disallowed, timeouts;
    int policy_result, denied_result, timeout_result, later_result;
    bool pic_unchanged, pit_unchanged, disabled;
};
int biosvm_selftest(struct biosvm_selftest_report *r);
/* Test-only reset of synthetic execution state; NEVER reclaims quarantined
 * hardware. Refuses after a live lease fault: registry quarantine is final.
 * The lead must use a fresh boot for a live firmware timeout. */
int biosvm_reset_for_test(void);
#endif
