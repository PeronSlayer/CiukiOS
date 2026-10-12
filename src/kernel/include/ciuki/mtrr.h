/* LFB cache policy (UP only).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MTRR_H
#define CIUKI_MTRR_H
#include <stdint.h>
#include <stdbool.h>

/* Once, before the first LFB mapping; further calls reuse the boot decision.
 * False means keep UC. Does not change PAT or any valid BIOS range. */
bool mtrr_framebuffer_enable(uint32_t phys, uint32_t bytes);
/* Runtime only: caller has quiesced/revoked ALL LFB aliases after firmware.
 * Replan for confirmed geometry, reusing only verified OS-owned pairs. */
bool mtrr_framebuffer_reconfigure(uint32_t phys, uint32_t bytes);
void framebuffer_write_drain(void);
uint64_t framebuffer_cycles(void); /* caller checks TSC support/calibration */
#endif
