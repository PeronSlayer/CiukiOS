/* CiukiOS SVGA II platform I/O; no donor implementation code.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "engine.h"
#include <ciuki/cpu.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
static uint32_t read_reg(void *ctx,unsigned reg)
{
    struct vmsvga_ports *p=ctx;
    outl(p->base+SVGA_INDEX_PORT,reg);
    return inl(p->base+SVGA_VALUE_PORT);
}
static void write_reg(void *ctx,unsigned reg,uint32_t value)
{
    struct vmsvga_ports *p=ctx;
    outl(p->base+SVGA_INDEX_PORT,reg); outl(p->base+SVGA_VALUE_PORT,value);
}
static uint32_t read_fifo(void *ctx,unsigned off)
{ return ((struct vmsvga_ports *)ctx)->fifo[off/4]; }
static void write_fifo(void *ctx,unsigned off,uint32_t value)
{ ((struct vmsvga_ports *)ctx)->fifo[off/4]=value; }
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static uint64_t now(void *ctx)
{
    (void)ctx;
    uint64_t cycles=g_tsc_per_ms ? read_tsc_unserialized() : 0;
    return g_tsc_per_ms ? cycles/g_tsc_per_ms*1000 + cycles%g_tsc_per_ms*1000/g_tsc_per_ms : g_ticks*1000;
}
static int delay(void *ctx,unsigned us) { (void)ctx; return udelay(us); }
struct vmsvga_io vmsvga_port_io(struct vmsvga_ports *ports)
{
    return (struct vmsvga_io){.ctx=ports,.read=read_reg,.write=write_reg,
        .fifo_read=read_fifo,.fifo_write=write_fifo,.drain=drain,.now=now,.delay=delay};
}
