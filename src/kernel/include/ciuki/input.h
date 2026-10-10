/* Ordered kernel input, F1. No user ABI is published here.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_INPUT_H
#define CIUKI_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

#define INPUT_CAPACITY 256u
enum input_type { INPUT_KEY = 1, INPUT_REL, INPUT_BTN, INPUT_TEXT, INPUT_RESYNC };
enum input_axis { INPUT_X, INPUT_Y };
enum input_button { INPUT_LEFT, INPUT_RIGHT, INPUT_MIDDLE };
enum input_source { INPUT_NATIVE = 1, INPUT_FIRMWARE };
#define INPUT_F_RESYNC 1u
/* Public set-1 positions: ordinary scan byte, E0 positions = 0x100 |
 * scan byte. Pause = 0x200 (a synthetic down/up, no hardware break code).
 * This encoding is independent of the character layout. */
#define INPUT_KEY_A 0x1Eu
#define INPUT_KEY_PAUSE 0x200u

struct input_event {
    uint16_t type, code;
    int32_t value;
    uint64_t tick, sequence;
    gen_t generation;
    uint16_t source, flags;
    uint64_t lost_count; /* RESYNC only; cumulative discarded events */
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
 * 0-2); consumers release remembered keys. state_lost is latched telemetry,
 * not a flag on subsequent events. */
bool input_read(struct input_event *out);
void input_snapshot(struct input_stats *out);
char input_unshifted(uint16_t code);
void input_digest_init(struct input_digest *d);
void input_digest_add(struct input_digest *d, const struct input_event *e);

#endif
