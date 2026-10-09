/* Resource registry (docs/design/device-firmware-ownership.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_REGISTRY_H
#define CIUKI_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

enum res_type { RES_PORT, RES_IRQ, RES_DMA, RES_MMIO };
enum res_state { RS_FIRMWARE, RS_CLAIMED, RS_ACTIVE, RS_QUIESCING, RS_RELEASED, RS_QUARANTINED,
                 RS_DISCOVERED };

struct resource {
    enum res_type type;
    uint32_t start, end;            /* inclusive start, exclusive end */
    const char *owner;
    gen_t generation;
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

/* Counts are cumulative except live (includes discovery, reservations and
 * quarantine). registry_count remains the occupied slot high-water mark;
 * released slots are reused with fresh generations. Owners have static or
 * otherwise pinned names. valid excludes released/quarantined handles. */
struct registry_stats {
    uint64_t claims, releases, conflicts, quarantines;
    unsigned live;
};
bool registry_valid(int handle, gen_t generation);
int registry_discover(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share);
int registry_claim_reserved(int handle, gen_t generation, const char *owner);
int registry_activate(int handle, gen_t generation);
typedef bool (*registry_idle_proof_fn)(int handle, gen_t generation);
/* Thread context, IF=1. Publishes QUIESCING before calling proof outside the
 * registry irq lock. Proof must stop sources, drain callbacks, establish
 * idle and revoke mappings before success. Release is barred while it runs
 * or while the claim still has an attached IRQ handler.
 * A false proof quarantines permanently; no release/reclaim/reset bypass. */
int registry_quiesce(int handle, gen_t generation, registry_idle_proof_fn proof);
int registry_quarantine(int handle, gen_t generation);
void registry_snapshot(struct registry_stats *out);

#define IRQ_CHAIN_MAX 8u
#define IRQ_CHAIN_STUCK_PASSES 1000u
typedef bool (*irq_chain_handler_t)(unsigned irq, const char *owner);
struct irq_chain_stats {
    uint64_t passes, handled, unclaimed, budget_violations;
    uint64_t max_service_us;
    unsigned participants, consecutive_unclaimed, quarantines;
    bool quarantined, deferred;
};
/* Qualified PCI level routing only: caller validates chipset/ELCR and owns
 * an ACTIVE shareable registry IRQ claim. Fixed edge/cascade lines reject.
 * Handlers test/ack only their own source, return handled/not-mine, never
 * sleep or alter PIC/IF, and may run in the worker when draining is deferred.
 * Every callback must fit the remaining hard-IRQ budget (250 us aggregate).
 * add/remove run in thread context; removal retains peers and invalidates
 * pending callbacks before returning. Removal during a callback returns
 * -EINVAL (including self-removal); retry after it finishes. Driver context
 * stays pinned until successful removal.
 *
 * trap.c retains spurious detection and exactly one EOI after this chain's
 * handler returns. The line stays masked until a worker continuation, which
 * necessarily runs after EOI in the non-preemptible kernel. Ring overflow
 * fails closed, retaining the masked line and quarantining all its claims. */
int irq_chain_add(unsigned irq, irq_chain_handler_t handler, const char *owner);
int irq_chain_remove(unsigned irq, irq_chain_handler_t handler, const char *owner);
bool irq_chain_snapshot(unsigned irq, struct irq_chain_stats *out);

#endif
