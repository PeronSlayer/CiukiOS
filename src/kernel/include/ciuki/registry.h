/* Resource registry (docs/design/device-firmware-ownership.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_REGISTRY_H
#define CIUKI_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

enum res_type { RES_PORT, RES_IRQ, RES_DMA, RES_MMIO };
enum res_state { RS_FIRMWARE, RS_CLAIMED, RS_ACTIVE, RS_QUIESCING, RS_RELEASED, RS_QUARANTINED };

struct resource {
    enum res_type type;
    uint32_t start, end;            /* inclusive start, exclusive end */
    const char *owner;
    uint32_t generation;
    enum res_state state;
    bool shareable;
};

struct pci_func {
    uint8_t bus, dev, fn;
    uint16_t vendor, device, sub_vendor, sub_device;
    uint8_t class_code, subclass, prog_if, revision, header_type;
    uint8_t irq_line, irq_pin;
    uint16_t command;
    uint32_t bar[6];
};

void registry_init(void);
int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share);
int registry_release(int handle, uint32_t generation);
unsigned registry_count(void);
const struct resource *registry_get(unsigned i);
unsigned pci_count(void);
const struct pci_func *pci_get(unsigned i);

#endif
