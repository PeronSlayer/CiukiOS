/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_DOSVM_H
#define CIUKI_USER_DOSVM_H
#include <ciuki/abi.h>
int ciuki_dosvm(uint32_t operation, struct ciuki_dosvm_command *command);
#endif
