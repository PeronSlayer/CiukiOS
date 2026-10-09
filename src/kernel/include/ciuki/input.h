/* Ordered kernel input, F1. No user ABI is published here.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_INPUT_H
#define CIUKI_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

#define INPUT_CAPACITY 256u
enum input_type { INPUT_KEY = 1, INPUT_REL, INPUT_BTN };
enum input_axis { INPUT_X, INPUT_Y };
enum input_button { INPUT_LEFT, INPUT_RIGHT, INPUT_MIDDLE };
enum input_source { INPUT_NATIVE = 1, INPUT_FIRMWARE };
#define INPUT_F_RESYNC 1u
/* Stable raw set-2 positions: ordinary scan byte, E0 positions = 0x100 |
 * scan byte. Pause = 0x1FF (a synthetic down/up, no hardware break code).
 * This encoding is independent of the character layout. */
#define INPUT_KEY_A 0x1Cu
#define INPUT_KEY_PAUSE 0x1FFu

struct input_event {
    uint16_t type, code;
    int32_t value;
    uint64_t tick, sequence;
    gen_t generation;
    uint16_t source, flags;
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

/* One consumer in thread context; never reads the controller. Overflow
 * drops the new event, counts loss, and marks subsequent events RESYNC.
 * state_lost stays latched until a new activation; consumers must treat
 * their reconstructed state as incomplete after loss. */
bool input_read(struct input_event *out);
void input_snapshot(struct input_stats *out);
char input_unshifted(uint16_t code);
void input_digest_init(struct input_digest *d);
void input_digest_add(struct input_digest *d, const struct input_event *e);

#endif
