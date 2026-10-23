/* Per-subscription RC model. No physical I/O. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <ciuki/joystick.h>
uint8_t joystick_virtual_io(struct joystick_virtual *v, uint64_t clocks, bool write)
{
    /* One virtual ISA I/O cycle advances at least one PIT input clock, also
     * when DOS disables its virtual IRQ0. Wall-clock PIT catchup can advance
     * this clock further. Never reprogram or latch the native PIT here. */
    if (clocks>v->now) v->now=clocks;
    else if (v->now<UINT64_MAX) v->now++;
    int32_t axes[4]; uint8_t mask, buttons;
    joystick_snapshot(&v->route,axes,&mask,&buttons);
    if (write) {
        v->measured=mask;
        for (unsigned a=0;a<4;a++) {
            int32_t x=axes[a]; if (x < -32767) x=-32767; if (x>32767) x=32767;
            uint64_t duration=24+(uint64_t)(x+32767)*1408/65534;
            v->deadline[a]=UINT64_MAX-v->now<duration ? UINT64_MAX : v->now+duration;
        }
    }
    uint8_t value=(uint8_t)(~(unsigned)buttons<<4)&0xf0;
    for (unsigned a=0;a<4;a++) if (!(mask&v->measured&(1u<<a)) || v->now<v->deadline[a]) value|=1u<<a;
    return value;
}
int joystick_virtual_bios(struct joystick_virtual *v, uint16_t subfunction, uint16_t registers[4])
{
    if (subfunction>1) return -EINVAL;
    int32_t axes[4]; uint8_t mask, buttons;
    joystick_snapshot(&v->route,axes,&mask,&buttons);
    if (!subfunction) { registers[0]=(uint16_t)(~(unsigned)buttons<<4)&0xf0; return mask ? 0 : -ENODEV; }
    for (unsigned a=0;a<4;a++) {
        int32_t x=axes[a]; if (x < -32767) x=-32767; if (x>32767) x=32767;
        registers[a]=mask&(1u<<a) ? (uint32_t)(x+32767)*416/65534 : 0;
    }
    return mask ? 0 : -ENODEV;
}
