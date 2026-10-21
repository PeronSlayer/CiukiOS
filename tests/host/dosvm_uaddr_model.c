/* F3-52: real user mappings for command transfers, sharing the VM harness's
 * fake physical-memory allocator and CPU/wait-word transport.
 * SPDX-License-Identifier: GPL-2.0-only */
#define ua_pin(...) dosvm_model_pin(__VA_ARGS__)
#define ua_unpin dosvm_model_unpin
#define ua_read dosvm_model_read
#define ua_write dosvm_model_write
#define ua_user_read dosvm_model_user_read
#define ua_user_write dosvm_model_user_write
#define copy_from_user dosvm_model_copy_from_user
#define copy_to_user dosvm_model_copy_to_user
/* Native pointer sizes inflate uaddr's metadata in this 64-bit host build. */
#define kmalloc dosvm_host_alloc
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#define CIUKI_CPU_H
extern void *dosvm_host_physical(uint32_t);
#define P2V(p) dosvm_host_physical(p)
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t flags) { (void)flags; }
static uint32_t read_cr3(void) { return 0; }
static void invlpg(uint32_t address) { (void)address; }
void *kzalloc(size_t bytes)
{ void *p=kmalloc(bytes); if (p) memset(p,0,bytes); return p; }
/* No user wait-word is installed by these mapping/command fixtures. */
void ww_cancel(void *process, uint64_t generation, uint32_t base, uint32_t end)
{ (void)process; (void)generation; (void)base; (void)end; }
/* The legacy VM boundary fixtures keep their small byte-array seams. Real
 * mappings are selected explicitly for the protection regression below. */
#include "../../src/kernel/proc/uaddr.c"
