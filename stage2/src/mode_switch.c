/*
 * OPENGEM-044 Task A — Mode-switch engine (long ↔ legacy 32-bit PM).
 *
 * This translation unit provides the arm-gate, state, and probe shell.
 * The actual asm trampoline (mode_switch_asm.S) is pending in a later
 * commit inside this same branch. Until then, mode_switch_run_legacy_pm
 * returns MODE_SWITCH_ERR_NOT_IMPLEMENTED. Probe validates disarmed-path
 * behaviour only, so the subsystem can land in CI without runtime risk.
 *
 * Boot-path isolation: no symbol in this file is called outside of its
 * own gate probe. Gate `test_mode_switch.sh` enforces that.
 *
 * Sentinel string below is inspected by the gate to verify provenance.
 */

#include "mode_switch.h"

static const char opengem_044_a_sentinel[] = "OPENGEM-044-A";

static int s_mode_switch_armed = 0;

int mode_switch_arm(uint32_t magic)
{
    if (magic != MODE_SWITCH_ARM_MAGIC) {
        return MODE_SWITCH_ERR_BAD_INPUT;
    }
    s_mode_switch_armed = 1;
    return MODE_SWITCH_OK;
}

void mode_switch_disarm(void)
{
    s_mode_switch_armed = 0;
}

int mode_switch_is_armed(void)
{
    return s_mode_switch_armed;
}

int mode_switch_run_legacy_pm(mode_switch_legacy_pm_body_fn body, void *user)
{
    /* Arm-gate FIRST, before any state inspection. */
    if (!s_mode_switch_armed) {
        return MODE_SWITCH_ERR_NOT_ARMED;
    }
    if (body == (mode_switch_legacy_pm_body_fn)0) {
        return MODE_SWITCH_ERR_BAD_INPUT;
    }
    (void)user;
    /* The asm trampoline is not yet landed. Returning a distinguishable
     * error preserves boot safety: no CR/MSR is touched until the real
     * engine ships. Task B may stub against this contract meanwhile. */
    return MODE_SWITCH_ERR_NOT_IMPLEMENTED;
}

int mode_switch_probe(void)
{
    /* All cases below exercise the disarmed and input-validation paths
     * only. No mode register, no CR3, no EFER write is performed. */

    /* Case 1: disarmed refusal. */
    mode_switch_disarm();
    if (mode_switch_run_legacy_pm((mode_switch_legacy_pm_body_fn)(uintptr_t)0x1, (void *)0) != MODE_SWITCH_ERR_NOT_ARMED) {
        return -1;
    }

    /* Case 2: bad magic. */
    if (mode_switch_arm(0xDEADBEEFu) != MODE_SWITCH_ERR_BAD_INPUT) {
        return -2;
    }
    if (mode_switch_is_armed() != 0) {
        return -3;
    }

    /* Case 3: correct magic arms. */
    if (mode_switch_arm(MODE_SWITCH_ARM_MAGIC) != MODE_SWITCH_OK) {
        return -4;
    }
    if (mode_switch_is_armed() != 1) {
        return -5;
    }

    /* Case 4: armed + NULL body → BAD_INPUT, engine NOT executed. */
    if (mode_switch_run_legacy_pm((mode_switch_legacy_pm_body_fn)0, (void *)0) != MODE_SWITCH_ERR_BAD_INPUT) {
        return -6;
    }

    /* Case 5: armed + non-NULL body → NOT_IMPLEMENTED (pending asm).
     * This transitions to OK once mode_switch_asm.S lands in this branch. */
    if (mode_switch_run_legacy_pm((mode_switch_legacy_pm_body_fn)(uintptr_t)0x1, (void *)0) != MODE_SWITCH_ERR_NOT_IMPLEMENTED) {
        return -7;
    }

    /* Sentinel referenced to prevent dead-strip. */
    if (opengem_044_a_sentinel[0] != 'O') {
        return -8;
    }

    mode_switch_disarm();
    return 0;
}
