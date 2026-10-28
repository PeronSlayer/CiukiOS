/* Ordered kernel input, F1. No user ABI is published here.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_INPUT_H
#define CIUKI_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "sync.h"
#include <ciuki/input_abi.h>

#define INPUT_CAPACITY 256u
enum input_type { INPUT_KEY = 1, INPUT_REL, INPUT_BTN, INPUT_TEXT, INPUT_RESYNC, INPUT_ABS };
enum input_hid_type { INPUT_USAGE = 7, INPUT_CONSUMER, INPUT_JOY_AXIS,
    INPUT_JOY_HAT, INPUT_JOY_BUTTON, INPUT_DEVICE };
enum input_axis { INPUT_X, INPUT_Y, INPUT_WHEEL };
enum input_button { INPUT_LEFT, INPUT_RIGHT, INPUT_MIDDLE, INPUT_SIDE, INPUT_EXTRA };
enum input_source { INPUT_NATIVE = 1, INPUT_FIRMWARE, INPUT_USB };
/* Live producers, independent of the boot-time i8042 activation result.
 * PS/2 and firmware activate a keyboard/mouse pair; USB counts HID interfaces.
 * An input grant remains valid even when every count is zero. */
struct input_devices { unsigned ps2, firmware, usb; };
void input_devices_snapshot(struct input_devices *out);
void input_devices_changed(enum input_source source, bool attached);
#define INPUT_F_RESYNC 1u
/* Public set-1 positions: ordinary scan byte, E0 positions = 0x100 |
 * scan byte. Pause = 0x200 (a synthetic down/up, no hardware break code).
 * This encoding is independent of the character layout. */
#define INPUT_REPEAT_DELAY_MS 500u
#define INPUT_REPEAT_INTERVAL_MS 33u
unsigned input_repeat_delay(void);
unsigned input_repeat_interval(void);
unsigned input_lock_leds(void);
struct ciuki_keyboard_settings;
int input_keyboard_settings(const struct ciuki_keyboard_settings *);
/* USB producer: balanced transitions per HID interface, thread context.
 * Merges with native PS/2 held state, even when i8042 activation failed. */
void input_usb_event(uint16_t type, uint16_t code, int32_t value, gen_t generation, uint64_t tick);
/* Balanced serial-mouse transitions merge with the existing pointer state. */
void input_serial_event(uint16_t type, uint16_t code, int32_t value, gen_t generation, uint64_t tick);
void input_hid_event(uint16_t type, uint16_t code, int32_t value, uint32_t device,
                     gen_t generation, uint64_t tick);

/* --- sec-k07 (begin): trusted console input domain ---
 * INPUT_ATTENTION is queued only by the kernel's physical secure attention
 * filter and only while the queue feeds the trusted domain; code is the
 * CIUKI_SECURITY_SECURE_* reason. Grants deliver it to LOGON_UI alone. */
enum { INPUT_ATTENTION = 13 };
bool input_secure_trusted(void);
void input_secure_domain(bool trusted, uint32_t attention);
void input_secure_stats(uint64_t *discarded, uint64_t *attentions);
/* sec-user-06: the T boot's security.lock_latency only (drivers/i8042.c);
 * -EPERM outside a self-test boot, -ENOENT before the LOGON role. */
int input_selftest_attention(void);
struct input_event;
bool input_read_domain(struct input_event *out, bool trusted);
/* --- sec-k07 (end) --- */

#define INPUT_KEY_A 0x1Eu
#define INPUT_KEY_PAUSE 0x200u

struct input_event {
    uint16_t type, code;
    int32_t value;
    uint64_t tick, sequence;
    gen_t generation;
    uint16_t source, flags;
    uint64_t lost_count; /* RESYNC only; cumulative discarded events */
    uint32_t device; /* HID extension only; stable logical interface locator */
};
struct input_stats {
    uint64_t overflow, resync, duplicates, repeats, bytes, errors;
    unsigned pending, keys_down, buttons;
    bool state_lost;
};
struct input_digest {
    uint32_t hash, text_hash;
    uint64_t events, characters, key_transitions, button_transitions;
    int64_t x, y;
};

/* One consumer in thread context; never reads the controller. KEY value is
 * 0=up, 1=down, 2=repeat. Overflow discards stale queued events, counts them,
 * and queues one RESYNC before the triggering event and fresh transitions.
 * RESYNC carries lost_count and the current button bitmap in value (bits
 * 0-4); consumers release remembered keys. state_lost is latched telemetry,
 * not a flag on subsequent events. */
bool input_read(struct input_event *out);
void input_snapshot(struct input_stats *out);
char input_unshifted(uint16_t code);
void input_digest_init(struct input_digest *d);
void input_digest_add(struct input_digest *d, const struct input_event *e);

#endif
