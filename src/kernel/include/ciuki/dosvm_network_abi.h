/* f6-11 proposed CREATE launch.network word at offset 84. Frozen command
 * size/old offsets remain unchanged; zero means off. The existing kernel
 * validator must explicitly allow this word before the UI enables it.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSVM_NETWORK_ABI_H
#define CIUKI_DOSVM_NETWORK_ABI_H
#include <ciuki/abi.h>
#define CIUKI_DOSVM_NETWORK_OFF 0u
#define CIUKI_DOSVM_NETWORK_IPX 1u
static inline uint32_t dosvm_launch_network(const struct ciuki_dosvm_command *c)
{ return c->launch.reserved[0]; }
static inline void dosvm_launch_network_set(struct ciuki_dosvm_command *c, uint32_t mode)
{ c->launch.reserved[0] = mode; }
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, launch.reserved[0]) == 84, "launch.network offset");
#endif
