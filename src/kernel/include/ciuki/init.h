/* F1 boot activation and firmware queue bridge.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_INIT_H
#define CIUKI_INIT_H
#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

#define ACTIVATION_LEDGER_MAX 32u
struct activation_entry {
    uint32_t seq;
    const char *device, *result, *reason;
    uint64_t tick;
    bool safe_flag_before;
    int error;
    bool required, present, quarantined;
};

struct drivers_state {
    uint32_t boot_flags, flag_sequence, fb_sequence, input_sequence, ata_sequence;
    unsigned optional_activations;
    int fb_error, input_error, ata_error;
    bool initialized, safe, fb_called, ata_called, firmware;
};
void drivers_init(void);
void drivers_snapshot(struct drivers_state *out);
/* Boot-task-owned, append-only ledger; immutable after drivers_init returns.
 * The flag snapshot precedes its driver entries in activation sequence. */
unsigned drivers_activation_count(void);
const struct activation_entry *drivers_activation_get(unsigned index);
bool drivers_activation_ordered(void);
/* Called before the scheduler creates any resumable protected frames. */
void stackprot_init(void);

struct fwinput_event;
int fwinput_adapter_init(void);
gen_t fwinput_adapter_generation(void);
unsigned fwinput_adapter_step(void); /* bounded thread-context poll */
bool input_firmware_begin(gen_t generation);
void input_firmware_event(const struct fwinput_event *event, gen_t generation);
void input_firmware_loss(uint64_t lost);
void input_firmware_disable(gen_t generation);
#endif
