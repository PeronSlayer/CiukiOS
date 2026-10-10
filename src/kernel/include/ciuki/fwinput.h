/* Firmware event adapter. Codes are translated set-1 positions; E0 adds 0x100.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FWINPUT_H
#define CIUKI_FWINPUT_H
#include <stdint.h>
#include <stdbool.h>
#define FWINPUT_CAPACITY 128u
enum fwinput_type { FWINPUT_KEY = 1, FWINPUT_TEXT, FWINPUT_REL, FWINPUT_BUTTON, FWINPUT_RESYNC };
enum fwinput_axis { FWINPUT_X, FWINPUT_Y };
struct fwinput_event { uint16_t type, code; int32_t value; uint64_t tick; };
struct fwinput_stats {
    uint64_t loss, resyncs, keys, text, packets, malformed;
    uint64_t observed_bytes, aux_bytes, scan_bytes, makes;
    uint64_t text_matched, text_unmatched, agreement_overflow;
    uint32_t mouse_functions; /* bit AL records successful INT15/C2 subfunctions */
    uint64_t service_budget_violations, max_service_us;
};
struct fwinput_backend_state {
    bool keyboard, mouse, disabled, key_releases;
    int setup_error;
};
/* Instance form is also used by the sanitizer tests. No controller reads. */
struct fwinput_decoder {
    struct fwinput_event events[FWINPUT_CAPACITY];
    unsigned head, count, mouse_count, pause;
    uint8_t mouse_bytes[3], buttons;
    bool e0, held[256], extended_held[128], resync_pending;
    uint64_t last_tick, resync_tick;
    /* Pending make observations, including typematic, indexed by set-1
     * position. INT16 text consumes a match even if the key was released.
     * Cleared on resync and after draining the BIOS queue. This compares
     * positions, not a second ASCII/modifier translation implementation. */
    uint16_t pending_makes[128];
    struct fwinput_stats stats;
};
void fwinput_decode_scan(struct fwinput_decoder *d, uint8_t byte, uint64_t tick);
void fwinput_decode_bios(struct fwinput_decoder *d, uint16_t ax, uint64_t tick);
void fwinput_decode_mouse(struct fwinput_decoder *d, uint8_t byte, uint64_t tick);
void fwinput_decoder_loss(struct fwinput_decoder *d, unsigned lost);
unsigned fwinput_decode_poll(struct fwinput_decoder *d, struct fwinput_event *out, unsigned max);
int fwinput_init(void);
unsigned fwinput_poll(struct fwinput_event *out, unsigned max);
/* Decoded work or a backend-disable transition. Safe as a kwait condition. */
bool fwinput_pending(void);
void fwinput_stats(struct fwinput_stats *out);
void fwinput_backend_state(struct fwinput_backend_state *out);
#endif
