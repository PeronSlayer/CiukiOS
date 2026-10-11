/* f3-02: one DOS window VM per process, delegated by the desktop launch of
 * /bin/dosbox. Record and operations: ciuki/abi.h, execution-abi.md.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_DOSVM_H
#define CIUKI_USER_DOSVM_H
#include <ciuki/abi.h>
#define __CIUKI_DOSVM_VERSION__ 1
/* Returns 0, or -1 with errno set. The record's version and size must be set. */
int ciuki_dosvm(uint32_t operation, struct ciuki_dosvm_command *command);
#endif
