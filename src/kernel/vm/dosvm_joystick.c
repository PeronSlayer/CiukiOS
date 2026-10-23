/* V86/DPMI joystick services, common native state only.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include "dosvm_internal.h"
int dosvm_joystick_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (port!=0x201 || width!=1) return -V86_EPERM;
    uint32_t flags=irq_save();
    uint8_t b=joystick_virtual_io(&v->joystick,v->pit.clocks,write);
    irq_restore(flags);
    if (!write) *value=b;
    return 0;
}
int dosvm_joystick_bios(struct dosvm *v, struct v86_frame *f)
{
    uint16_t registers[4]={(uint16_t)f->tf.eax,(uint16_t)f->tf.ebx,(uint16_t)f->tf.ecx,(uint16_t)f->tf.edx};
    uint32_t flags=irq_save();
    int e=joystick_virtual_bios(&v->joystick,(uint16_t)f->tf.edx,registers);
    irq_restore(flags);
    f->tf.eflags=(f->tf.eflags&~V86_CF)|(e ? V86_CF : 0);
    if (e && e!=-ENODEV) { f->tf.eax=(f->tf.eax&~0xff00u)|0x8600; return 0; }
    uint32_t *r[]={&f->tf.eax,&f->tf.ebx,&f->tf.ecx,&f->tf.edx};
    for (unsigned a=0;a<4;a++) *r[a]=(*r[a]&0xffff0000u)|registers[a];
    return 0;
}
