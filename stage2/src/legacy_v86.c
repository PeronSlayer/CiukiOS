/*
 * OPENGEM-044 Task B — Legacy-PM v86 host scaffold.
 *
 * Stage 1 intentionally does not enter v86. It only publishes the
 * arm-gated host wrapper around Task A's mode-switch engine so Task B
 * can be built and validated before the legacy PM trampoline lands.
 */

#include "legacy_v86.h"
#include "mode_switch.h"

typedef struct {
    const legacy_v86_frame_t *entry;
    legacy_v86_exit_t *out;
} legacy_v86_context_t;

extern void legacy_v86_pm32_body(void *user);

static const char opengem_044_b_sentinel[] = "OPENGEM-044-B";

static int s_legacy_v86_armed = 0;

static void legacy_v86_copy_frame(legacy_v86_frame_t *dst, const legacy_v86_frame_t *src)
{
    int index;

    if ((dst == (legacy_v86_frame_t *)0) || (src == (const legacy_v86_frame_t *)0)) {
        return;
    }

    dst->cs = src->cs;
    dst->ip = src->ip;
    dst->ss = src->ss;
    dst->sp = src->sp;
    dst->ds = src->ds;
    dst->es = src->es;
    dst->fs = src->fs;
    dst->gs = src->gs;
    dst->eflags = src->eflags;
    for (index = 0; index < 4; ++index) {
        dst->reserved[index] = src->reserved[index];
    }
}

static void legacy_v86_clear_frame(legacy_v86_frame_t *frame)
{
    int index;

    if (frame == (legacy_v86_frame_t *)0) {
        return;
    }

    frame->cs = 0;
    frame->ip = 0;
    frame->ss = 0;
    frame->sp = 0;
    frame->ds = 0;
    frame->es = 0;
    frame->fs = 0;
    frame->gs = 0;
    frame->eflags = 0;
    for (index = 0; index < 4; ++index) {
        frame->reserved[index] = 0;
    }
}

static void legacy_v86_fill_fault(legacy_v86_exit_t *out,
                                  const legacy_v86_frame_t *entry,
                                  uint32_t fault_code)
{
    if (out == (legacy_v86_exit_t *)0) {
        return;
    }

    out->reason = LEGACY_V86_EXIT_FAULT;
    out->int_vector = 0;
    if (entry != (const legacy_v86_frame_t *)0) {
        legacy_v86_copy_frame(&out->frame, entry);
    } else {
        legacy_v86_clear_frame(&out->frame);
    }
    out->fault_code = fault_code;
}

int legacy_v86_arm(uint32_t magic)
{
    if (magic != LEGACY_V86_ARM_MAGIC) {
        return LEGACY_V86_ERR_BAD_INPUT;
    }
    s_legacy_v86_armed = 1;
    return LEGACY_V86_OK;
}

void legacy_v86_disarm(void)
{
    s_legacy_v86_armed = 0;
}

int legacy_v86_is_armed(void)
{
    return s_legacy_v86_armed;
}

int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out)
{
    legacy_v86_context_t context;
    int rc;

    /* Arm-gate FIRST, before any state inspection. */
    if (!s_legacy_v86_armed) {
        return LEGACY_V86_ERR_NOT_ARMED;
    }
    if ((entry == (const legacy_v86_frame_t *)0) || (out == (legacy_v86_exit_t *)0)) {
        return LEGACY_V86_ERR_BAD_INPUT;
    }

    context.entry = entry;
    context.out = out;

    rc = mode_switch_run_legacy_pm(legacy_v86_pm32_body, &context);
    if (rc == MODE_SWITCH_OK) {
        legacy_v86_fill_fault(out, entry, LEGACY_V86_FAULT_PM32_BODY_RETURNED);
        return LEGACY_V86_OK;
    }
    if (rc == MODE_SWITCH_ERR_NOT_ARMED) {
        legacy_v86_fill_fault(out, entry, LEGACY_V86_FAULT_MODE_SWITCH_NOT_ARMED);
        return LEGACY_V86_OK;
    }
    if (rc == MODE_SWITCH_ERR_NOT_IMPLEMENTED) {
        legacy_v86_fill_fault(out, entry, LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED);
        return LEGACY_V86_OK;
    }

    legacy_v86_fill_fault(out, entry, LEGACY_V86_FAULT_MODE_SWITCH_ERROR);
    return LEGACY_V86_OK;
}

int legacy_v86_probe(void)
{
    legacy_v86_frame_t entry;
    legacy_v86_frame_t before;
    legacy_v86_exit_t out;

    entry.cs = 0x1000u;
    entry.ip = 0x0000u;
    entry.ss = 0x2000u;
    entry.sp = 0x0100u;
    entry.ds = 0x3000u;
    entry.es = 0x3000u;
    entry.fs = 0x0000u;
    entry.gs = 0x0000u;
    entry.eflags = 0x00000202u;
    entry.reserved[0] = 0;
    entry.reserved[1] = 0;
    entry.reserved[2] = 0;
    entry.reserved[3] = 0;
    legacy_v86_copy_frame(&before, &entry);

    legacy_v86_disarm();
    mode_switch_disarm();

    if (legacy_v86_enter(&entry, &out) != LEGACY_V86_ERR_NOT_ARMED) {
        return -1;
    }

    if (legacy_v86_arm(0xDEADBEEFu) != LEGACY_V86_ERR_BAD_INPUT) {
        return -2;
    }
    if (legacy_v86_is_armed() != 0) {
        return -3;
    }

    if (legacy_v86_arm(LEGACY_V86_ARM_MAGIC) != LEGACY_V86_OK) {
        return -4;
    }
    if (legacy_v86_is_armed() != 1) {
        return -5;
    }

    if (legacy_v86_enter((const legacy_v86_frame_t *)0, &out) != LEGACY_V86_ERR_BAD_INPUT) {
        return -6;
    }
    if (legacy_v86_enter(&entry, (legacy_v86_exit_t *)0) != LEGACY_V86_ERR_BAD_INPUT) {
        return -7;
    }

    if (legacy_v86_enter(&entry, &out) != LEGACY_V86_OK) {
        return -8;
    }
    if (out.reason != LEGACY_V86_EXIT_FAULT) {
        return -9;
    }
    if (out.fault_code != LEGACY_V86_FAULT_MODE_SWITCH_NOT_ARMED) {
        return -10;
    }
    if ((out.frame.cs != before.cs) ||
        (out.frame.ip != before.ip) ||
        (out.frame.ss != before.ss) ||
        (out.frame.sp != before.sp) ||
        (out.frame.eflags != before.eflags)) {
        return -11;
    }

    if (mode_switch_arm(MODE_SWITCH_ARM_MAGIC) != MODE_SWITCH_OK) {
        return -12;
    }
    if (legacy_v86_enter(&entry, &out) != LEGACY_V86_OK) {
        return -13;
    }
    if (out.reason != LEGACY_V86_EXIT_FAULT) {
        return -14;
    }
    if (out.fault_code != LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED) {
        return -15;
    }
    if ((out.frame.cs != before.cs) ||
        (out.frame.ip != before.ip) ||
        (out.frame.ss != before.ss) ||
        (out.frame.sp != before.sp) ||
        (out.frame.ds != before.ds) ||
        (out.frame.es != before.es) ||
        (out.frame.fs != before.fs) ||
        (out.frame.gs != before.gs) ||
        (out.frame.eflags != before.eflags)) {
        return -16;
    }

    legacy_v86_disarm();
    mode_switch_disarm();
    if (legacy_v86_is_armed() != 0) {
        return -17;
    }
    if (opengem_044_b_sentinel[0] != 'O') {
        return -18;
    }
    return 0;
}