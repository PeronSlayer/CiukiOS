/* Native-port hardware boundary. SPDX-License-Identifier: GPL-2.0-only */
#include "native.h"
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
#include <ciuki/mtrr.h>
static uint32_t hw_read(void *ctx, unsigned bits, uint32_t reg)
{
    volatile uint8_t *p=(volatile uint8_t *)ctx+reg;
    if (bits==8) return *p;
    if (bits==16) return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void hw_write(void *ctx, unsigned bits, uint32_t reg, uint32_t value)
{
    volatile uint8_t *p=(volatile uint8_t *)ctx+reg;
    if (bits==8) *p=(uint8_t)value;
    else if (bits==16) *(volatile uint16_t *)p=(uint16_t)value;
    else *(volatile uint32_t *)p=value;
}
static uint8_t hw_index_read(void *ctx, uint16_t port, uint8_t index)
{ (void)ctx; uint8_t old=inb(port); outb(port,index); uint8_t v=inb(port+1); outb(port,old); return v; }
static void hw_index_write(void *ctx, uint16_t port, uint8_t index, uint8_t v)
{ (void)ctx; uint8_t old=inb(port); outb(port,index); outb(port+1,v); outb(port,old); }
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t c=ktime_cycles();
    return c/g_tsc_per_ms*1000+c%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static void hw_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
void video_hardware_io(struct video_native *s, void *mmio)
{ s->io=(struct video_io){mmio,hw_read,hw_write,hw_index_read,hw_index_write,hw_now,hw_drain}; }

