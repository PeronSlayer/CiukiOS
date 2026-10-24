/* F6 stack boundary: no lwIP or physical-driver types escape.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_STACK_H
#define CIUKI_NET_STACK_H
#include <ciuki/process.h>
int net_stack_init(void);
void net_stack_input(unsigned, const void *, unsigned);
void net_stack_link(unsigned, bool);
int net_stack_attach(unsigned, const uint8_t mac[6], bool link);
/* Index of the successfully attached default physical netif; no fallback to
 * a different running adapter when the selected port loses carrier. */
int net_stack_primary(void);
void net_stack_search_domain(char out[128]);
int net_stack_random_id(uint8_t[16]);
void net_stack_address(unsigned, uint32_t *address);
int net_stack_dhcp_error(unsigned);
/* f6-22: driver counters for the `[net] stack` and DHCP timeout records,
 * filled by the kernel NIC layer (lwip_handoff.c; absent on host harnesses,
 * which read zeros unless they define it). frames_in/frames_out are the
 * stack's own copied-frame counts. */
struct net_stack_counters {
    uint64_t rx, tx, rx_errors, tx_errors, dropped, frames_in, frames_out;
};
void net_stack_driver_counters(unsigned, struct net_stack_counters *);
/* Any task, cheap: posts one preallocated core watch pass at most every
 * 250 ms (DHCP state records, timeout record, periodic stack line). */
void net_stack_watch(void);
int net_stack_transmit(unsigned, const void *, unsigned);
void net_stack_entropy(uint64_t);
void net_fd_changed(struct process *, unsigned);
/* Input publication, including IRQ producers: defer the poll waiter walk. */
void net_input_changed(void);
uint32_t net_socket_count(void);
void net_socket_snapshot(struct ciuki_net_socket_list *);
uint32_t *net_socket_flags(struct proc_object *);
bool net_socket_object(struct proc_object *);
int32_t net_socket_call(struct trap_frame *);
int32_t net_socket_rw(struct process *, int, uint32_t, uint32_t, bool);
int net_socket_create(struct process *, int, int, int);
/* Retained at initial validation, before any potentially sleeping user access.
 * Kernel-buffer callers may pass NULL to capture at entry instead. */
struct net_socket_ref {
    struct proc_object *object;
    unsigned generation;
};
int net_socket_capture(struct process *, int, struct net_socket_ref *);
void net_socket_drop(struct net_socket_ref *);
int net_socket_bind(struct process *, int, const struct ciuki_sockaddr_in *, const struct net_socket_ref *);
int net_socket_connect(struct process *, int, const struct ciuki_sockaddr_in *, const struct net_socket_ref *);
int net_socket_listen(struct process *, int, int, const struct net_socket_ref *);
int net_socket_accept(struct process *, int, struct ciuki_sockaddr_in *, const struct net_socket_ref *);
int net_socket_io(struct process *, int, void *, unsigned, unsigned,
                  struct ciuki_sockaddr_in *, bool, const struct net_socket_ref *);
int net_socket_shutdown(struct process *, int, unsigned, const struct net_socket_ref *);
int net_socket_name(struct process *, int, unsigned, struct ciuki_sockaddr_in *, const struct net_socket_ref *);
int net_socket_option(struct process *, int, unsigned, int, int, void *, unsigned *, const struct net_socket_ref *);
int net_poll_capture(struct process *, const struct ciuki_pollfd *, unsigned, void **);
void net_poll_drop(void *);
int net_poll(struct process *, struct ciuki_pollfd *, unsigned, int, void *);
int net_resolve(struct process *, const char *, uint32_t *, int *);
/* f6-18 test-profile static host entries (fixture.test -> 10.0.2.100): armed
 * only by the f6:browser probe, consulted before any DNS query, never set on
 * a normal boot. address is in network byte order; address 0 clears the slot.
 * Returns -EINVAL for a name the resolver grammar refuses, -ENOSPC when both
 * slots hold other names. */
#define NET_RESOLVE_OVERRIDES 2
int net_resolve_override(const char *name, uint32_t address);
#endif
