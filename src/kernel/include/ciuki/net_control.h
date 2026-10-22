/* Network configuration, private kernel boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_CONTROL_H
#define CIUKI_NET_CONTROL_H
#include <ciuki/abi.h>
#include <stddef.h>
#include <stdbool.h>
#define NET_SETTINGS_BYTES 2048u
struct vfs;
struct net_config_identity { uint32_t vendor, device; uint8_t mac[6], reserved[2]; };
void net_config_default(struct ciuki_net_config *);
bool net_config_valid(const struct ciuki_net_config *);
int net_config_parse(const char *, size_t, struct ciuki_net_config [2], struct net_config_identity *);
size_t net_config_format(const struct ciuki_net_config [2], const struct net_config_identity *, char *, size_t);
void net_settings_load(struct vfs *);
void net_settings_get(unsigned, struct ciuki_net_config *);
int net_settings_save(unsigned, const struct ciuki_net_config *);
int net_control(unsigned, struct ciuki_net_control *);
int net_control_driver(unsigned, struct ciuki_net_control *, bool);
int net_stack_config(unsigned, unsigned, struct ciuki_net_control *);
#endif
