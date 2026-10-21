/* Syscall 91. Small kernel scratch, transactional permission-checked copy.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#include <ciuki/entropy.h>
#include <ciuki/entropy_pool.h>
int32_t entropy_syscall(struct trap_frame *tf)
{
    if (tf->ecx > 256 || tf->edx || tf->esi || tf->edi || tf->ebp) return -EINVAL;
    if (!tf->ecx) return 0;
    struct process *p = proc_current();
    if (!p || !tf->ebx || tf->ebx > UINT32_MAX - tf->ecx ||
        !ua_range(p->memory, tf->ebx, tf->ecx, PROT_WRITE)) return -EFAULT;
    uint8_t buffer[256];
    int e = entropy_get(buffer, tf->ecx);
    if (!e && ua_user_write(p->memory, tf->ebx, buffer, tf->ecx)) e = -EFAULT;
    entropy_erase(buffer, sizeof(buffer));
    return e;
}
