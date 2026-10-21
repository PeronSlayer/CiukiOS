/* VM owner adapter, explicit opt-in; default is off.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_NETWORK_H
#define CIUKI_DOSVM_NETWORK_H
#include <ciuki/dosvm.h>
#include <ciuki/net_bridge_port.h>
struct dosvm_network;
/* Lead-owned network service publishes/revokes one prepared physical port.
 * Never substitutes a port while a VM is attached. No syscall exposes this. */
int dosvm_network_bind_port(struct net_bridge_port *);
int dosvm_network_launch(struct dosvm *, unsigned mode);
/* The lead-owned boot/configuration service supplies one selected bridge with
 * a shared packet pool, a persisted random identity and working RX-mode ops.
 * No second physical port, implicit identity, or fallback network is selected. */
int dosvm_network_attach(struct dosvm *, struct net_bridge_port *);
/* May return EBUSY: retry before guest address space release. */
int dosvm_network_detach(struct dosvm *);
void dosvm_network_service(struct dosvm *, uint64_t host_ms); /* owner thread, IF=1 */
int dosvm_network_interrupt(struct dosvm *, struct v86_frame *, uint8_t);
int dosvm_network_dispatch(struct dosvm *, struct v86_frame *); /* safe VM callback boundary */
void dosvm_network_rom_init(struct dosvm *);
bool dosvm_network_irq_available(const struct dosvm *, unsigned);
void dosvm_network_process_exit(struct dosvm *, uint16_t psp);
#endif
