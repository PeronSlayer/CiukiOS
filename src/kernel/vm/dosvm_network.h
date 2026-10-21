/* VM owner adapter, explicit opt-in; default is off.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_NETWORK_H
#define CIUKI_DOSVM_NETWORK_H
#include <ciuki/dosvm.h>
#include <ciuki/net_bridge.h>
struct dosvm_network;
/* The lead-owned boot/configuration service supplies one selected bridge with
 * a shared packet pool, a persisted random identity and working RX-mode ops.
 * No second physical port, implicit identity, or fallback network is selected. */
int dosvm_network_attach(struct dosvm *, struct net_bridge *);
void dosvm_network_detach(struct dosvm *); /* before guest address space release */
void dosvm_network_service(struct dosvm *, uint64_t host_ms); /* owner thread, IF=1 */
int dosvm_network_interrupt(struct dosvm *, struct v86_frame *, uint8_t);
int dosvm_network_dispatch(struct dosvm *, struct v86_frame *); /* safe VM callback boundary */
void dosvm_network_rom_init(struct dosvm *);
bool dosvm_network_irq_available(const struct dosvm *, unsigned);
#endif
