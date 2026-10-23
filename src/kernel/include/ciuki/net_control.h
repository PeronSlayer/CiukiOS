/* Network configuration, private kernel boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_CONTROL_H
#define CIUKI_NET_CONTROL_H
#include <ciuki/abi.h>
#include <ciuki/net_config.h>
#include <stddef.h>
#include <stdbool.h>
struct vfs;
void net_settings_load(struct vfs *);
void net_settings_get(unsigned, struct ciuki_net_config *);
int net_settings_save(unsigned, const struct ciuki_net_config *);
int net_control(unsigned, struct ciuki_net_control *);
int net_control_driver(unsigned, struct ciuki_net_control *, bool);
int net_stack_config(unsigned, unsigned, struct ciuki_net_control *);
#endif
