/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SPAWN_H
#define CIUKI_SPAWN_H
#include <ciuki/abi.h>
ciuki_pid_t ciuki_spawn(const char *,char *const [],char *const [],const struct ciuki_spawn_fd *,uint32_t,uint32_t);
#endif
