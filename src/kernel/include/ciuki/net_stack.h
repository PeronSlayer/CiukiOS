/* F6 stack boundary: no lwIP or physical-driver types escape.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_STACK_H
#define CIUKI_NET_STACK_H
#include <ciuki/process.h>
int net_stack_init(void);
void net_stack_input(unsigned, const void *, unsigned);
void net_stack_link(unsigned, bool);
int net_stack_attach(unsigned, const uint8_t mac[6], bool link);
void net_stack_address(unsigned, uint32_t *address);
int net_stack_transmit(unsigned, const void *, unsigned);
void net_stack_entropy(uint64_t);
void net_fd_changed(struct process *, unsigned);
uint32_t net_socket_count(void);
uint32_t *net_socket_flags(struct proc_object *);
bool net_socket_object(struct proc_object *);
int32_t net_socket_call(struct trap_frame *);
int32_t net_socket_rw(struct process *, int, uint32_t, uint32_t, bool);
int net_socket_create(struct process *, int, int, int);
int net_socket_bind(struct process *, int, const struct ciuki_sockaddr_in *);
int net_socket_connect(struct process *, int, const struct ciuki_sockaddr_in *);
int net_socket_listen(struct process *, int, int);
int net_socket_accept(struct process *, int, struct ciuki_sockaddr_in *);
int net_socket_io(struct process *, int, void *, unsigned, unsigned,
                  struct ciuki_sockaddr_in *, bool);
int net_socket_shutdown(struct process *, int, unsigned);
int net_socket_name(struct process *, int, unsigned, struct ciuki_sockaddr_in *);
int net_socket_option(struct process *, int, unsigned, int, int, void *, unsigned *);
int net_poll(struct process *, struct ciuki_pollfd *, unsigned, int);
int net_resolve(struct process *, const char *, uint32_t *, int *);
#endif
