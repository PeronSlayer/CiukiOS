/* Independent fake register/packet boundary, shared by lifecycle and engine tests.
 * Only the pixel interpreter writes scanout. SPDX-License-Identifier: GPL-2.0-only */
static struct {
    uint32_t regs[NV2D_MMIO_BYTES/4];
    uint8_t cursor[256], index;
    uint8_t pixels[32768], golden[32768];
    _Alignas(4) uint8_t tail[NV2D_NV40_TAIL_BYTES];
    struct { uint32_t reg, value; } trace[32768];
    unsigned traced, methods, blits, delays, pauses, reads, drains;
    unsigned fault_after, stop_after;
    bool hold_get, ignore_put, malformed_get;
    unsigned wraps, packets, padding_puts;
    bool valid, frozen, stuck, dead, ignore_cursor, ignore_stop;
    bool hold_fifo, fail_cursor_read, partial, invalidate_on_pause, quarantine_on_pause, drop_draw;
    bool loading_cursor;
    uint64_t time;
    unsigned bytes, pitch, width, height;
    uint32_t scratch_offset, tail_offset;
    int pause_error;
} m;
#ifndef NV2D_ADAPTER_FIXTURE
static struct nv2d engine;
#endif
static uint32_t reg(uint32_t r) { return m.regs[r/4]; }
static uint8_t *address(uint32_t offset, unsigned n)
{
    if (offset>=m.scratch_offset) {
        assert((uint64_t)offset+n<=m.tail_offset+engine.tail_bytes);
        return m.tail+(offset-m.tail_offset);
    }
    assert((uint64_t)offset+n<=sizeof(m.pixels));
    return m.pixels+offset;
}
static uint32_t load_pixel(const uint8_t *p)
{
    uint32_t value=0;
    for (unsigned b=0;b<m.bytes;b++) value|=(uint32_t)p[b]<<(8*b);
    return value;
}
static void put_pixel(uint8_t *p, uint32_t v)
{ for (unsigned b=0;b<m.bytes;b++) p[b]=(uint8_t)(v>>(8*b)); }
static void draw(void)
{
    if (m.drop_draw) return;
    unsigned x=reg(NV2D_FIFO+0xc400)>>16, y=reg(NV2D_FIFO+0xc400)&65535;
    unsigned w=reg(NV2D_FIFO+0xc404)>>16, h=reg(NV2D_FIFO+0xc404)&65535;
    assert(x+w<=m.width && y+h<=m.height);
    uint32_t rop=reg(NV2D_FIFO+0x2300);
    for (unsigned yy=y;yy<y+h;yy++) for (unsigned xx=x;xx<x+w;xx++) {
        uint32_t value=reg(NV2D_FIFO+0xc3fc);
        if (rop==0xf0) {
            uint32_t bits=reg(NV2D_FIFO+0x4318+((yy%8)/4)*4);
            unsigned bit=((bits>>(8*(yy%4)))>>(xx%8))&1;
            value=reg(NV2D_FIFO+0x4310+4*bit);
            /* Color format A1/XRGB uses alpha only for pattern opacity;
             * the modeled surface stores the RGB channels. */
            value&=m.bytes==4 ? 0xffffffu : engine.fb.green_size==5 ? 0x7fffu : 0xffffu;
        } else assert(rop==0xcc);
        put_pixel(m.pixels+yy*m.pitch+xx*m.bytes,value);
    }
}
static void blit(void)
{
    unsigned sx=reg(NV2D_FIFO+0xa300)&65535, sy=reg(NV2D_FIFO+0xa300)>>16;
    unsigned dx=reg(NV2D_FIFO+0xa304)&65535, dy=reg(NV2D_FIFO+0xa304)>>16;
    unsigned w=reg(NV2D_FIFO+0xa308)&65535, h=reg(NV2D_FIFO+0xa308)>>16;
    unsigned sp=reg(NV2D_FIFO+0x304)&65535, dp=reg(NV2D_FIFO+0x304)>>16;
    uint32_t source=reg(NV2D_FIFO+0x308);
    assert(dx+w<=m.width && dy+h<=m.height && dp==m.pitch);
    /* The port must never depend on a hidden overlap-direction property. */
    assert(source || sx+w<=dx || dx+w<=sx || sy+h<=dy || dy+h<=sy);
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        uint8_t *p=address(source+(sy+y)*sp+(sx+x)*m.bytes,m.bytes);
        put_pixel(m.pixels+(dy+y)*dp+(dx+x)*m.bytes,load_pixel(p));
    }
    m.blits++;
    if (m.fault_after && m.blits>=m.fault_after) { m.stuck=true; m.partial=true; }
}
static uint32_t model_read32(void *ctx, uint32_t r)
{
    (void)ctx; assert(r<NV2D_MMIO_BYTES && !(r&3)); m.reads++;
    if (m.dead) return UINT32_MAX;
    if (r==NV2D_FREE) { assert(!engine.dma); return m.hold_fifo ? 0 : 0x7c00; }
    if (r==NV2D_GET && m.malformed_get) return 3;
    if (r==NV2D_GRAPH_BUSY) return m.stuck ? 1 : 0;
    return reg(r);
}
static void method_write(uint32_t r, uint32_t v)
{
    assert(m.traced<ARRAY_SIZE(m.trace));
    m.trace[m.traced].reg=r; m.trace[m.traced++].value=v;
    m.regs[r/4]=v;
    m.methods++;
    if (r==NV2D_FIFO+0xc404) draw();
    if (r==NV2D_FIFO+0xa308) blit();
    if (m.stop_after && m.methods>=m.stop_after) m.hold_fifo=true;
}
static void consume(void)
{
    if (m.hold_get) return;
    uint32_t get=reg(NV2D_GET)/4, put=reg(NV2D_PUT)/4;
    assert(get<NV2D_RING_WORDS && put<NV2D_RING_WORDS);
    for (unsigned n=0;get!=put;n++) {
        assert(n<NV2D_RING_WORDS);
        uint32_t header=engine.ring[get++];
        if (header==0x20000000) { get=0; m.wraps++; }
        else if (header) {
            unsigned count=header>>18, method=header&0xffff;
            assert(count && count<=32 && get+count<NV2D_RING_WORDS);
            assert((header&0x00030000)==0);
            m.packets++;
            for (unsigned i=0;i<count;i++) method_write(NV2D_FIFO+method+4*i,engine.ring[get++]);
        }
        m.regs[NV2D_GET/4]=get*4;
        if (m.partial) { m.hold_get=true; return; }
    }
}
static void model_write32(void *ctx, uint32_t r, uint32_t v)
{
    (void)ctx; assert(r<NV2D_MMIO_BYTES && !(r&3));
    if (r>=NV2D_FIFO && r!=NV2D_PUT && r!=NV2D_GET) {
        assert(!engine.dma); method_write(r,v); return;
    }
    assert(m.traced<ARRAY_SIZE(m.trace));
    m.trace[m.traced].reg=r; m.trace[m.traced++].value=v;
    if (r==NV2D_FIFO_INTR || r==NV2D_GRAPH_INTR) { m.regs[r/4]&=~v; return; }
    if (m.ignore_stop && (r==0x3200 || r==0x3250 || r==0x2500 || r==0x3220)) return;
    if (r==NV2D_PUT && m.ignore_put) return;
    m.regs[r/4]=v;
    if (r==NV2D_PUT && v) {
        if (v==NV2D_SKIPS*4) m.padding_puts++;
        consume();
    }
    if (r==NV2D_GET) assert(!v); /* GET is written only during stopped setup */
}
static uint8_t model_read8(void *ctx, uint32_t r)
{
    (void)ctx; assert(r==NV2D_PCIO+0x3d5 || r==NV2D_PCIO+0x2000+0x3d5);
    return m.fail_cursor_read ? 255 : m.cursor[m.index];
}
static void model_write8(void *ctx, uint32_t r, uint8_t v)
{
    (void)ctx;
    if (r==NV2D_PCIO+0x3d4 || r==NV2D_PCIO+0x2000+0x3d4) m.index=v;
    else { assert(r==NV2D_PCIO+0x3d5 || r==NV2D_PCIO+0x2000+0x3d5); if (!m.ignore_cursor) m.cursor[m.index]=v; }
}
static uint64_t model_now(void *ctx) { (void)ctx; return m.time; }
static void model_delay(void *ctx, unsigned us) { (void)ctx; m.delays++; if (!m.frozen) m.time+=us; }
static void model_drain(void *ctx)
{ (void)ctx; m.drains++; if (m.loading_cursor) assert(!(m.cursor[0x31]&1)); }
static bool model_valid(void *ctx) { (void)ctx; return m.valid
#ifdef NV2D_ADAPTER_FIXTURE
        && io_valid(NULL)
#endif
        ; }
static const struct nv2d_io model_io={.read=model_read32,.write=model_write32,.read8=model_read8,.write8=model_write8,
    .now=model_now,.delay=model_delay,.drain=model_drain,.valid=model_valid};
static int pause_wait(void *ctx)
{
    assert(ctx==&m); m.pauses++;
    if (m.invalidate_on_pause) m.valid=false;
    if (m.quarantine_on_pause) { nv2d_quarantine(&engine,"pause_quarantine");m.hold_fifo=false; }
    return m.pause_error;
}
