/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_FENCE_H
#define CIUKI_NDIS_FENCE_H
#include "callback.h"
struct ndis_pci_fence {
    struct pci_func function; /* pinned inventory copy, not driver storage */
    struct ndis_callback *callback;
    uint16_t command_before, command_after;
    bool prepared, command_verified;
    /* This step supplies no per-revision engine-idle proof. Pins stay held. */
};
int ndis_fence_prepare(struct ndis_pci_fence *,struct ndis_callback *,unsigned pci_index);
bool ndis_fence_emergency(void *,const char *reason);
/* Native backend serializes CF8/CFC and writes only the command WORD,
 * never the adjacent W1C PCI status. Synthetic tests replace this backend. */
bool ndis_pci_disable(const struct pci_func *,uint16_t *before,uint16_t *after);
#endif
