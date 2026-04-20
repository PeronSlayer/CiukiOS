/*
 * OPENGEM-044 Stage 3C — v86 live wrapper.
 *
 * Arm-gated host-side glue around the legacy_v86_v86_body trampoline.
 * Does not perform any mode transition itself; delegates to Task A's
 * mode_switch_run_legacy_pm after verifying all required arm flags.
 */

#include "legacy_v86_live.h"
#include "mode_switch.h"

static const char opengem_044_stage3c_sentinel[] = "OPENGEM-044-STAGE3C";

static int s_legacy_v86_live_armed = 0;

/* Exposed by legacy_v86_v86_body.S. Runs in legacy 32-bit PM, paging off. */
extern void legacy_v86_v86_body(void *user);
extern const char legacy_v86_v86_body_sentinel[];

int legacy_v86_live_arm(uint32_t magic)
{
    if (magic != LEGACY_V86_LIVE_ARM_MAGIC) {
        return LEGACY_V86_LIVE_ERR_BAD_INPUT;
    }
    s_legacy_v86_live_armed = 1;
    return LEGACY_V86_LIVE_OK;
}

void legacy_v86_live_disarm(void)
{
    s_legacy_v86_live_armed = 0;
}

int legacy_v86_live_is_armed(void)
{
    return s_legacy_v86_live_armed;
}

int legacy_v86_live_enter(void)
{
    int rc;

    if (!s_legacy_v86_live_armed) {
        return LEGACY_V86_LIVE_ERR_NOT_ARMED;
    }
    if (!mode_switch_is_armed()) {
        return LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF;
    }

    rc = mode_switch_run_legacy_pm(legacy_v86_v86_body, (void *)0);
    return rc;
}

int legacy_v86_live_probe(void)
{
    if (legacy_v86_live_is_armed() != 0) {
        return -1;
    }
    if (legacy_v86_live_enter() != LEGACY_V86_LIVE_ERR_NOT_ARMED) {
        return -2;
    }
    if (legacy_v86_live_arm(0xDEADBEEFu) != LEGACY_V86_LIVE_ERR_BAD_INPUT) {
        return -3;
    }
    if (legacy_v86_live_is_armed() != 0) {
        return -4;
    }
    if (legacy_v86_live_arm(LEGACY_V86_LIVE_ARM_MAGIC) != LEGACY_V86_LIVE_OK) {
        return -5;
    }
    if (legacy_v86_live_is_armed() != 1) {
        return -6;
    }
    mode_switch_disarm();
    if (legacy_v86_live_enter() != LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF) {
        return -7;
    }
    legacy_v86_live_disarm();
    if (opengem_044_stage3c_sentinel[0] != 'O') {
        return -8;
    }
    if (legacy_v86_v86_body_sentinel[0] != 'V') {
        return -9;
    }
    return 0;
}
