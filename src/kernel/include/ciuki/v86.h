/* F1 V86 monitor; F3 extends this state, not a second execution path.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_V86_H
#define CIUKI_V86_H
#include <ciuki/arch.h>
#include <stddef.h>

#define V86_IF 0x200u
#define V86_TF 0x100u
#define V86_VM 0x20000u
#define V86_CF 1u
#define V86_ZF 0x40u
#define V86_EPERM 1
#define V86_EIO 5
#define V86_ETIMEDOUT 110

/* Intel SDM Vol. 3B fig. 23-4: these four words follow SS:ESP.
 * The protected-mode segment saves in tf are NOT the real-mode segments. */
struct v86_frame {
    struct trap_frame tf;
    uint32_t es, ds, fs, gs;
};
_Static_assert(offsetof(struct v86_frame, es) == 76, "V86 extended frame");
_Static_assert(sizeof(struct v86_frame) == 92, "V86 frame size");

enum v86_port_class { V86_CONTROLLER, V86_PIC, V86_PIT, V86_RTC, V86_PORT_CLASSES };
struct v86_port_rule { uint16_t first, last; enum v86_port_class kind; };
struct v86_pic {
    uint8_t irr, isr, imr, base, priority, init, icw1;
    bool read_isr, auto_eoi, rotate_auto, poll, special_mask;
};
enum v86_insn { V86_INT, V86_IRET, V86_PUSHF, V86_POPF, V86_CLI,
                V86_STI, V86_IN, V86_OUT, V86_INS, V86_OUTS, V86_HLT, V86_INSNS };
struct v86_stats {
    uint64_t calls, io[V86_PORT_CLASSES], reflected_irqs, disallowed_io, timeouts;
    uint64_t insn[V86_INSNS];
};
enum v86_state { V86_IDLE, V86_RUNNING, V86_HALTED, V86_DONE, V86_DISABLED };
struct v86_ops {
    /* Return a host pointer only after validating the WHOLE linear range.
     * No allocations, page faults or pointer into an unrelated address space. */
    void *(*memory)(void *arg, uint32_t linear, unsigned bytes, bool write);
    uint8_t (*in)(void *arg, uint16_t port);
    void (*out)(void *arg, uint16_t port, uint8_t value);
    void *arg;
};
struct v86 {
    struct v86_ops ops;
    const struct v86_port_rule *ports;
    unsigned nports;
    struct v86_pic pic[2];
    struct v86_stats stats;
    enum v86_state state;
    uint64_t deadline, ticks;
    bool vif, shadow;
    uint16_t sentinel_cs, sentinel_ip, sentinel_ss, sentinel_sp;
    uint16_t pit_latch[3];
    uint8_t pit_phase[3], pit_latched[3], rtc_index, rtc[128];
    bool rtc_valid;
    int result;
    struct { uint32_t cs, ip, vector, address; uint8_t bytes[15], count; } fault;
};
void v86_init(struct v86 *v, const struct v86_ops *ops);
int v86_begin(struct v86 *v, uint64_t now, uint32_t ticks);
int v86_check_deadline(struct v86 *v, uint64_t now);
int v86_abort(struct v86 *v, const struct v86_frame *f, int error);
int v86_io(struct v86 *v, uint16_t port, unsigned width, bool write, uint32_t *value);
void v86_irq_raise(struct v86 *v, unsigned irq);
int v86_irq_deliver(struct v86 *v, struct v86_frame *f);
int v86_reflect(struct v86 *v, struct v86_frame *f, uint8_t vector);
/* 0: resume, 1: sentinel complete, 2: halted, negative: disabled. */
int v86_emulate(struct v86 *v, struct v86_frame *f);
int v86_debug_step(struct v86 *v, struct v86_frame *f);

#endif
