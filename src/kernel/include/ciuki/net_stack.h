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
int net_stack_random_id(uint8_t[16]);
void net_stack_address(unsigned, uint32_t *address);
int net_stack_dhcp_error(unsigned);
int net_stack_transmit(unsigned, const void *, unsigned);
void net_stack_entropy(uint64_t);
void net_fd_changed(struct process *, unsigned);
uint32_t net_socket_count(void);
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
