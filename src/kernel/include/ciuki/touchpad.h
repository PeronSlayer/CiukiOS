/* Internal i8042 touchpad state; no syscall ABI additions.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TOUCHPAD_H
#define CIUKI_TOUCHPAD_H

#include "input.h"

enum touchpad_protocol { TOUCHPAD_NONE, TOUCHPAD_SYNAPTICS_OLD,
    TOUCHPAD_SYNAPTICS_NEW, TOUCHPAD_ALPS_V1, TOUCHPAD_ALPS_V2 };
/* INPUT_ABS codes. Coordinates are sensor units, y increasing downwards.
 * One complete packet emits X/Y/pressure/fingers/width in this order, followed
 * by the existing relative pointer projection. Pass-through emits only REL/BTN.
 * This is a kernel stream; the frozen desktop/DOS ABI receives the projection. */
enum touchpad_axis { TOUCHPAD_X, TOUCHPAD_Y, TOUCHPAD_PRESSURE,
    TOUCHPAD_FINGERS, TOUCHPAD_WIDTH };
struct touchpad_info {
    uint32_t protocol, identity, model, capabilities;
    int32_t x_min, x_max, y_min, y_max;
    int32_t fallback_error;
    uint8_t pass_through, two_finger, palm_detect, reserved;
};
/* Read-only; no probing, claims or controller I/O. */
void i8042_touchpad_snapshot(struct touchpad_info *out);

#endif
