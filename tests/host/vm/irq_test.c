/* Real V86/PIC arbitration around a pending virtual NE2000 cause.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include <ciuki/v86.h>
#include "../../../src/kernel/vm/dosvm_ne2000.h"
static struct v86 monitor;
static uint8_t memory[65536];
volatile uint64_t g_ticks;
void klog(const char *format,...) { (void)format; }
static void *guest_memory(void *arg,uint32_t at,unsigned n,bool write)
{ (void)arg; (void)write; return at<sizeof(memory) && n<=sizeof(memory)-at ? memory+at : 0; }
static void irq(void *arg,bool level)
{ (void)arg; if(level) v86_irq_raise(&monitor,3); }
static void nic(struct dosvm_ne2000 *d,unsigned reg,uint8_t value)
{ dosvm_ne2000_write(d,0x300+reg,1,value); }
int main(void)
{
    struct v86_ops ops={.memory=guest_memory}; v86_init(&monitor,&ops); CHECK(!v86_begin(&monitor,0,500));
    struct v86_frame f={0}; f.tf.cs=0x100; f.tf.user_ss=0x300; f.tf.user_esp=0x100;
    memory[0x1000]=0xfa; memory[0x1001]=0xfb; memory[0x1002]=0x90;
    memory[0x2000]=0xcf; memory[11*4+2]=0x00; memory[11*4+3]=0x02;
    const uint8_t mac[6]={2,1,2,3,4,5}; struct dosvm_ne2000 d;
    struct dosvm_ne2000_ops ne_ops={.irq=irq}; dosvm_ne2000_init(&d,mac,&ne_ops);
    CHECK(!v86_emulate(&monitor,&f) && !monitor.vif); /* actual CLI */
    dosvm_ne2000_overflow(&d); CHECK(d.isr&0x10); CHECK(!(monitor.pic[0].irr&8));
    nic(&d,15,0x10); CHECK(monitor.pic[0].irr&8); CHECK(v86_irq_pending(&monitor)<0); /* PIC mask */
    dosvm_pic_register(&monitor.pic[0],true,true,monitor.pic[0].imr&~8);
    CHECK(v86_irq_pending(&monitor)==3 && !v86_irq_deliver(&monitor,&f)); /* CLI */
    CHECK(!v86_emulate(&monitor,&f) && monitor.vif && monitor.shadow); /* STI shadow */
    CHECK(!v86_irq_deliver(&monitor,&f));
    f.tf.eip++; CHECK(!v86_debug_step(&monitor,&f) && !monitor.shadow); /* NOP retired */
    CHECK(v86_irq_deliver(&monitor,&f)==1 && f.tf.cs==0x200 && f.tf.eip==0);
    CHECK(monitor.pic[0].isr&8); CHECK(d.isr&0x10);
    nic(&d,7,0x10); CHECK(!(d.isr&0x10) && !d.irq_level);
    dosvm_ne2000_overflow(&d); CHECK(monitor.pic[0].irr&8); /* new cause held behind EOI */
    CHECK(!v86_emulate(&monitor,&f) && monitor.vif); /* actual IRET */
    CHECK(!v86_irq_deliver(&monitor,&f));
    dosvm_pic_register(&monitor.pic[0],false,true,0x20); /* nonspecific EOI */
    CHECK(v86_irq_deliver(&monitor,&f)==1 && monitor.stats.delivered[3]==2);
    nic(&d,15,0); CHECK(!d.irq_level && (d.isr&0x10));
    nic(&d,15,0x10); CHECK(d.irq_level); nic(&d,7,0x10); CHECK(!d.irq_level);
    dosvm_ne2000_reset(&d); CHECK(d.isr==0x80 && !d.imr && !d.irq_level);
    printf("virtual IRQ: %u checks passed\n",checks); return 0;
}
