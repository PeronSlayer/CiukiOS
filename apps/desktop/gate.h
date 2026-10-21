/* Private f2-12 test transport; not a desktop protocol/SDK extension.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_GATE_H
#define CIUKI_DESKTOP_GATE_H
#include <stdint.h>
#define GATE_MAGIC 0x32475443u
enum { GATE_SPAWN = 1, GATE_RELEASE, GATE_REAP, GATE_RUN, GATE_SNAPSHOT, GATE_INTERACT, GATE_V3,
       GATE_DIALOGS, /* f5-10C2: Win32 services plus the w32-dialogs fixture */
       GATE_UPPER /* f5-20D: Win32 services plus the w32-upper-dlls fixture */,
       GATE_MEDIA /* f5-12B2: Win32 services plus the w32-media fixture */,
       GATE_INSTALLER /* f5-15A2: pinned NSIS and independent manifests */ };
/* f2-33 tasks scenario: Tasks pings with this serial once the PID it ended
 * has left the kernel process list (runtime test mode only). */
#define DESK_TASKS_CHECKPOINT 0xf2330007u
struct gate_control { volatile uint32_t command, generation; };
enum { GATE_SETUP_SIGPIPE=1, GATE_SETUP_DISPLAY, GATE_SETUP_GEOMETRY,
    GATE_SETUP_MEMORY, GATE_SETUP_PORTRAIT, GATE_SETUP_OUTPUT,
    GATE_SETUP_INPUT, GATE_SETUP_CONTROL, GATE_SETUP_SURVIVOR };
static const char *const gate_setup_checks[] = {
    "", "sigpipe", "display", "geometry", "memory", "portrait", "output",
    "input", "control", "survivor"
};
static const char *const gate_faults[] = {
    "bad-pointer", "closed-peer", "forged-fd", "grant-fd", "handler-fault"
};
#endif
