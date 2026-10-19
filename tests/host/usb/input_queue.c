/* Production merged queue; only privileged port/IRQ operations are replaced.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <assert.h>
#define CIUKI_CPU_H
static uint32_t flags = 0x200;
static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void) { uint32_t f = flags; flags &= ~0x200u; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static uint8_t inb(uint16_t port) { (void)port; return 0xff; }
static void outb(uint16_t port, uint8_t value) { (void)port; (void)value; }
int udelay(uint32_t us) { (void)us; assert(!"unexpected physical i8042 access"); return -EINVAL; }
uint64_t ktime_cycles(void) { assert(!"unexpected physical i8042 clock access"); return 0; }
#include "../../../src/kernel/drivers/i8042.c"
bool kwork_queue(kwork_fn fn, void *arg)
{ (void)fn; (void)arg; assert(!"unexpected native IRQ work in merged-queue fixture"); return false; }
