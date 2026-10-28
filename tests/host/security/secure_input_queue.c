/* H09: the production merged input queue (drivers/i8042.c) in its own unit.
 * Only port/IRQ/clock primitives are replaced; the PS/2 set-2 decoder, the
 * USB/firmware producers, the secure attention filter and the domain switch
 * are the kernel's. Same boundary as tests/host/usb/input_queue.c.
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
{ (void)fn; (void)arg; assert(!"unexpected native IRQ work in the secure gate fixture"); return false; }
void pic_mask(unsigned irq) { (void)irq; assert(!"unexpected native PIC access"); }
void irq_set_handler(unsigned irq, irq_handler_t handler)
{ (void)irq; (void)handler; assert(!"unexpected native IRQ registration"); }
void task_yield(void) { assert(!"unexpected native command wait"); }

/* Accepting native controller, as after f1-04 activation, without ports. */
void secure_test_native(bool firmware, gen_t generation)
{
    uint32_t f = irq_save();
    native.accepting = native.stats.active = true;
    native.stats.firmware = firmware;
    native.stats.generation = generation;
    irq_restore(f);
}
/* One raw set-2 keyboard byte through the thread decoder (input_work's body). */
void secure_test_ps2(uint8_t byte, uint64_t tick) { capture_at(&native, OBF, byte, tick); }
bool secure_test_held(uint16_t code) { return code < ARRAY_SIZE(native.queue.down) && native.queue.down[code]; }
unsigned secure_test_pending(void) { return native.queue.stats.pending; }
