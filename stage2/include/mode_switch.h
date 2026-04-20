/*
 * OPENGEM-044 Task A — Long ↔ Legacy 32-bit PM mode-switch engine.
 *
 * Contract: see docs/opengem-044-mode-switch-split.md §3.1
 *
 * This API is the reversible mode-exit / mode-reentry primitive:
 *   caller (long mode)
 *       -> mode_switch_run_legacy_pm(body, user)
 *           -> trampoline exits IA-32e, enters 32-bit legacy PM
 *           -> body(user) runs in legacy PM
 *           -> trampoline restores long mode
 *       -> returns to caller (long mode, state preserved)
 *
 * Arm-gate default disarmed. Boot path must never reach it.
 * No v86 involvement here — that is Task B.
 */

#ifndef CIUKIOS_STAGE2_MODE_SWITCH_H
#define CIUKIOS_STAGE2_MODE_SWITCH_H

#include <stdint.h>

#define MODE_SWITCH_ARM_MAGIC   0xC1D39440u
#define MODE_SWITCH_SENTINEL    0x0440u
#define MODE_SWITCH_TRAMPOLINE_ARM_MAGIC 0xC1D3944Au

/* Body function executed in legacy 32-bit PM.
 * Must return cleanly. If it faults, engine halts deterministically;
 * recovery is Task B's domain (not yet present). */
typedef void (*mode_switch_legacy_pm_body_fn)(void *user);

/* Return codes.
 * Negative values are runtime errors; positive/zero are success. */
#define MODE_SWITCH_OK                    0
#define MODE_SWITCH_ERR_NOT_ARMED        -1
#define MODE_SWITCH_ERR_BAD_INPUT        -2
#define MODE_SWITCH_ERR_NOT_IMPLEMENTED  -3  /* engine asm not yet landed */

int  mode_switch_run_legacy_pm(mode_switch_legacy_pm_body_fn body, void *user);

int  mode_switch_arm(uint32_t magic);
void mode_switch_disarm(void);
int  mode_switch_is_armed(void);
int  mode_switch_trampoline_arm(uint32_t magic);
void mode_switch_trampoline_disarm(void);
int  mode_switch_trampoline_is_live(void);

/* Host-driven probe — no v86, no guest.
 * Returns 0 when all disarmed-path cases pass.
 * Does NOT execute the engine while the asm trampoline is still pending. */
int  mode_switch_probe(void);

#endif /* CIUKIOS_STAGE2_MODE_SWITCH_H */
