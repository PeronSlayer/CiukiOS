/* F1 boot activation and firmware queue bridge.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_INIT_H
#define CIUKI_INIT_H
#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

struct drivers_state {
    uint32_t boot_flags, flag_sequence, fb_sequence, input_sequence, ata_sequence;
    unsigned optional_activations;
    int fb_error, input_error, ata_error;
    bool initialized, safe, fb_called, ata_called, firmware;
};
void drivers_init(void);
void drivers_snapshot(struct drivers_state *out);
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
