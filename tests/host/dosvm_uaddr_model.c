/* F3-52: real user mappings for command transfers, sharing the VM harness's
 * fake physical-memory allocator and CPU/wait-word transport.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#define ua_pin(...) dosvm_model_pin(__VA_ARGS__)
#define ua_unpin dosvm_model_unpin
#define ua_read dosvm_model_read
#define ua_write dosvm_model_write
#define ua_user_read dosvm_model_user_read
#define ua_user_write dosvm_model_user_write
#define copy_from_user dosvm_model_copy_from_user
#define copy_to_user dosvm_model_copy_to_user
#define copy_user dosvm_model_copy_user
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#define CIUKI_CPU_H
extern void *dosvm_host_physical(uint32_t);
#define P2V(p) dosvm_host_physical(p)
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t flags) { (void)flags; }
static uint32_t read_cr3(void) { return 0; }
static void invlpg(uint32_t address) { (void)address; }
void *(kzalloc)(size_t bytes) { if (bytes > KHEAP_MAX_REQUEST) return 0; void *p=kmalloc(bytes); if (p) memset(p,0,bytes); return p; }
/* No user wait-word is installed by these mapping/command fixtures. */
void ww_cancel(void *process, uint64_t generation, uint32_t base, uint32_t end)
{ (void)process; (void)generation; (void)base; (void)end; }
/* The legacy VM boundary fixtures keep their small byte-array seams. Real
 * mappings are selected explicitly for the protection regression below. */
#include "../../src/kernel/proc/uaddr.c"

/* Instrumented replays retain the copy helpers too. As in proc_test.c,
 * translate simulated user VAs through the model's physical backing;
 * production copy_from/to_user above perform the permission/pin checks. */
int copy_user(void *dst, const void *src, uint32_t bytes)
{
    struct process *p = proc_current();
    if (!p || !p->memory) return -EFAULT;
    if ((uintptr_t)src <= UINT32_MAX)
        return ua_read(p->memory, dst, (uint32_t)(uintptr_t)src, bytes);
    if ((uintptr_t)dst <= UINT32_MAX)
        return ua_write(p->memory, (uint32_t)(uintptr_t)dst, src, bytes);
    return -EFAULT;
}
