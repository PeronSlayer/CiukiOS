/* The shell script has six left-button down/up pairs. Stage 6 and the A
 * edges precede the last pair; END must wait for its consumed release.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SHELL_PROBE_H
#define CIUKI_SHELL_PROBE_H
#include <stdint.h>
#include <stdbool.h>
#define SHELL_PROBE_BUTTON_EDGES 12u
static inline bool shell_probe_pending(uint32_t stage, uint32_t keys, uint32_t buttons)
{
    return stage!=6 || keys<4 || buttons<SHELL_PROBE_BUTTON_EDGES;
}
#endif
