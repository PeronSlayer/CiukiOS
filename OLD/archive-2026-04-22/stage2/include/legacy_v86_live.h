/*
 * OPENGEM-044 Stage 3C — first-v86-entry live scaffold.
 *
 * Opt-in wrapper around Task A's mode-switch engine that executes a
 * body which actually performs IRETL into virtual-8086 mode from
 * legacy 32-bit protected mode (paging off), and survives a #GP trap
 * back to the host.
 *
 * Contract: runtime-inerte by default. Two arm flags must be set
 * (mode_switch + mode_switch_trampoline) in addition to this file's
 * own arm flag before legacy_v86_live_enter can do anything.
 *
 * Magic:    0xC1D39470u (stage-3C arm)
 * Sentinel: 0x0470u
 * Marker:   "V86!" written to port 0xE9 by the #GP handler on first v86 trap.
 */

#ifndef CIUKIOS_STAGE2_LEGACY_V86_LIVE_H
#define CIUKIOS_STAGE2_LEGACY_V86_LIVE_H

#include <stdint.h>

#define LEGACY_V86_LIVE_ARM_MAGIC   0xC1D39470u
#define LEGACY_V86_LIVE_SENTINEL    0x0470u

#define LEGACY_V86_LIVE_OK                      0
#define LEGACY_V86_LIVE_ERR_NOT_ARMED          -1
#define LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF    -2
#define LEGACY_V86_LIVE_ERR_BAD_INPUT          -3

int  legacy_v86_live_arm(uint32_t magic);
void legacy_v86_live_disarm(void);
int  legacy_v86_live_is_armed(void);

/*
 * Runs the v86-entry body through Task A's mode-switch trampoline.
 * Caller must have already armed:
 *   - mode_switch_arm(MODE_SWITCH_ARM_MAGIC)
 *   - mode_switch_trampoline_arm(0xC1D3944Au)
 *   - legacy_v86_live_arm(LEGACY_V86_LIVE_ARM_MAGIC)
 * Returns the rc from mode_switch_run_legacy_pm (0 on nominal round-trip).
 */
int legacy_v86_live_enter(void);

int legacy_v86_live_probe(void);

#endif
