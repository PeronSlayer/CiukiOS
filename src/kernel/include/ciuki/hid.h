/* Bounded HID 1.11 layouts; pure parser/decoder.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_HID_H
#define CIUKI_HID_H
#include <stdbool.h>
#include <stdint.h>
#include <ciuki/input_abi.h>
#define HID_DESCRIPTOR_MAX 512u
#define HID_REPORT_MAX 8u
#define HID_FIELD_MAX 32u
#define HID_USAGE_MAX 64u
#define HID_PACKET_MAX 64u /* wire bytes, including an optional report ID */
#define HID_EPROTO 71
enum hid_kind { HID_INPUT, HID_OUTPUT, HID_FEATURE };
struct hid_usage { uint32_t low, high; };
struct hid_field {
    int32_t minimum, maximum, physical_minimum, physical_maximum;
    uint32_t unit, application;
    int32_t unit_exponent;
    uint16_t bit, count, flags;
    uint8_t size, report, kind, first_usage, usages, hat;
};
struct hid_report { uint16_t bits[3]; uint8_t id, unsafe_output; };
struct hid_layout {
    struct hid_field field[HID_FIELD_MAX];
    struct hid_usage usage[HID_USAGE_MAX];
    struct hid_report report[HID_REPORT_MAX];
    uint8_t fields, usages, reports, hats;
    bool numbered;
    uint32_t classes;
};
struct hid_values {
    uint8_t keyboard[32], consumers;
    uint32_t classes, buttons, axes, hats;
    int32_t axis[CIUKI_HID_AXES], hat[CIUKI_HID_HATS];
};
struct usb_hid_reports {
    struct hid_layout *layout;
    struct ciuki_hid_caps caps;
    struct ciuki_hid_state state;
    uint8_t keys[HID_REPORT_MAX][32], consumers[HID_REPORT_MAX];
    uint32_t buttons[HID_REPORT_MAX];
    int32_t calibration[CIUKI_HID_AXES][3], raw[CIUKI_HID_AXES];
};
/* kheap payload limit is 2040, including neither its 8-byte header nor a
 * foreign large-allocation fallback. Keep both independent objects within it. */
_Static_assert(sizeof(struct usb_hid_reports)<=2040, "HID state exceeds kheap payload");
_Static_assert(sizeof(struct hid_layout)<=2040, "HID layout exceeds kheap payload");
/* All failures leave out byte-for-byte unchanged. Unknown IDs return zero;
 * rollover returns 2 and also leaves out unchanged. Valid reports return 1. */
bool hid_parse(struct hid_layout *out, const uint8_t *, unsigned);
int hid_decode(const struct hid_layout *, const uint8_t *, unsigned, struct hid_values *out);
uint32_t hid_usage_at(const struct hid_layout *, const struct hid_field *, unsigned);
int32_t hid_value(const uint8_t *, unsigned bit, unsigned size, bool sign);
unsigned hid_consumer_action(uint32_t usage);
unsigned hid_axis_index(uint32_t usage);
#endif
