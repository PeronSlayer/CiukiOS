/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_USERHOST_KERNEL_H
#define CIUKI_NDIS_USERHOST_KERNEL_H
#include "userhost.h"
#include <ciuki/task.h>
/* No ordinary/native process may attach or inherit a host. The dedicated task
 * has a private raw address space, one thread and no native syscall surface. */
int ndis_user_attach(struct task *,struct ndis_user_host *);
void ndis_user_switch(const struct task *);
bool ndis_user_trap(struct trap_frame *);
void ndis_user_timer(void);
void ndis_user_task_stopping(struct task *,int);
void ndis_user_reaped(struct task *);
void ndis_user_return(struct trap_frame *);
/* Private host syscall ABI. Arguments are scalar generation/token values.
 * Unsupported calls quarantine; native mmap/clone/exec are never delegated. */
enum ndis_user_call { NDIS_USER_YIELD=0x4e440000u, NDIS_USER_TAKE,
    NDIS_USER_COMPLETE, NDIS_USER_REARM, NDIS_USER_FLAGS, NDIS_USER_RESTORE_FLAGS };
#endif
