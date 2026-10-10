/* Private f2-12 test transport; not a desktop protocol/SDK extension.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_GATE_H
#define CIUKI_DESKTOP_GATE_H
#include <stdint.h>
#define GATE_MAGIC 0x32475443u
enum { GATE_SPAWN = 1, GATE_RELEASE, GATE_REAP, GATE_RUN, GATE_SNAPSHOT, GATE_INTERACT };
struct gate_control { volatile uint32_t command, generation; };
static const char *const gate_faults[] = {
    "bad-pointer", "closed-peer", "forged-fd", "grant-fd", "handler-fault"
};
#endif
