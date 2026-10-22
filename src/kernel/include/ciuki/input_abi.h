/* Versioned native HID input extension. SPDX-License-Identifier: MIT */
#ifndef CIUKI_INPUT_ABI_H
#define CIUKI_INPUT_ABI_H
#include <stdint.h>
#define CIUKI_HID_VERSION 1u
#define INPUT_HID_GET_CAPS 0x4901u
#define INPUT_HID_GET_STATE 0x4902u
#define INPUT_HID_CALIBRATE 0x4903u
#define CIUKI_INPUT_USAGE 7u
#define CIUKI_INPUT_CONSUMER 8u
#define CIUKI_INPUT_JOY_AXIS 9u
#define CIUKI_INPUT_JOY_HAT 10u
#define CIUKI_INPUT_JOY_BUTTON 11u
#define CIUKI_INPUT_DEVICE 12u
#define CIUKI_CONSUMER_VOLUME_UP 1u
#define CIUKI_CONSUMER_VOLUME_DOWN 2u
#define CIUKI_CONSUMER_MUTE 3u
#define CIUKI_CONSUMER_PLAY_PAUSE 4u
#define CIUKI_CONSUMER_NEXT 5u
#define CIUKI_CONSUMER_PREVIOUS 6u
#define CIUKI_HID_KEYBOARD 1u
#define CIUKI_HID_MOUSE 2u
#define CIUKI_HID_CONSUMER 4u
#define CIUKI_HID_JOYSTICK 8u
#define CIUKI_HID_AXES 9u
#define CIUKI_HID_HATS 4u
#define CIUKI_HID_DEVICES 256u
#define CIUKI_JOY_X 0u
#define CIUKI_JOY_Y 1u
#define CIUKI_JOY_Z 2u
#define CIUKI_JOY_RX 3u
#define CIUKI_JOY_RY 4u
#define CIUKI_JOY_RZ 5u
#define CIUKI_JOY_SLIDER 6u
#define CIUKI_JOY_DIAL 7u
#define CIUKI_JOY_WHEEL 8u
/* New events reuse ciuki_input_event: value2=device, lost_count=version.
 * code is a usage/action/axis/hat/button index; value is 0/1 for transitions,
 * -32767..32767 for axes, or -1 (neutral)/0..7 (north, clockwise) for hats.
 * Device events use code=0, value=attached. Existing event types are unchanged.
 * Bitmaps use 1<<index; button 1 uses bit 0. Keyboard bit u is page-07 usage u.
 * Get caps permits generation=0 for discovery. State/calibration require the
 * exact generation. Only the input-grant owner may issue these devctls. */
/* All wire fields are fixed width. No pointers, host longs or implicit enum. */
#pragma pack(push, 4)
struct ciuki_hid_axis {
    /* Descriptor logical limits/default centre; physical metadata is retained.
     * Calibration affects normalized state, not these discovery facts. */
    int32_t minimum, centre, maximum;
    int32_t physical_minimum, physical_maximum;
    uint32_t unit;
    int32_t unit_exponent;
};
struct ciuki_hid_caps {
    uint32_t version, size, device, generation;
    uint32_t attached, classes, axes, hats, buttons, consumers;
    uint32_t host, address, interface, reserved;
    uint8_t keyboard[32];
    struct ciuki_hid_axis axis[CIUKI_HID_AXES];
};
struct ciuki_hid_state {
    uint32_t version, size, device, generation;
    uint32_t attached, consumers, buttons, reserved;
    uint8_t keyboard[32];
    int32_t axes[CIUKI_HID_AXES], hats[CIUKI_HID_HATS];
};
struct ciuki_hid_calibration {
    uint32_t version, size, device, generation, axis;
    int32_t minimum, centre, maximum;
};
#pragma pack(pop)
_Static_assert(sizeof(struct ciuki_hid_axis)==28, "HID axis ABI");
_Static_assert(sizeof(struct ciuki_hid_caps)==340, "HID caps ABI");
_Static_assert(sizeof(struct ciuki_hid_state)==116, "HID state ABI");
_Static_assert(sizeof(struct ciuki_hid_calibration)==32, "HID calibration ABI");
#endif
