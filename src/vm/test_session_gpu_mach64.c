/* Independent posted-register / stale CPU-cache fixture. This is a protocol
 * test, not an ATI silicon emulator or physical qualification. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "session_gpu_mach64.h"
#define VRAM (8U * 1024U * 1024U)
#define FB 0x40000000U
#define MMIO 0x41000000U
#define BYTES (3072U * 768U * 2U)
static uint8_t vram[VRAM], cache[VRAM], original[VRAM];
static uint32_t reg[1024], savedreg[1024], address, mode_bpp, pitch;
static uint32_t writes, triggers, map_count, unmaps, fifo_reads, gui_reads;
static uint32_t hanging, bad_pixel, bad_copy, absent, chip, bad_bar, bad_crtc;
static uint32_t pending_reg[32], pending_value[32], pending_count;
static uint32_t cache_flushes, writes_without_fifo;

static uint32_t cfg(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    if (fn) return 0xffffffffU;
    if (bus == 0U && dev == 1U) {
        if (!off) return 0x71918086U;
        if (off == 8U) return 0x06040000U;
        if (off == 0x18U) return 0x00010100U;
        return 0U;
    }
    if (bus == 1U && dev == 0U) {
        if (!off) return chip;
        if (off == 8U) return 0x03000000U;
        if (off == 4U) return absent ? 0U : 2U;
        if (off == 0x10U) return FB | 8U;
        if (off == 0x18U) return bad_bar ? MMIO | 4U : MMIO;
        return 0U;
    }
    return 0xffffffffU;
}
uint32_t cvdev_in(uint32_t port, uint32_t size)
{
    assert(port == 0xcfcU && size == 4U);
    return cfg((uint8_t)(address >> 16), (uint8_t)((address >> 11) & 31U),
               (uint8_t)((address >> 8) & 7U), (uint8_t)(address & 0xfcU));
}
void cvdev_out(uint32_t port, uint32_t value, uint32_t size)
{ assert(port == 0xcf8U && size == 4U); address = value; }
uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes)
{
    assert(bytes == 4096U); ++map_count;
    if (physical == MMIO) return 1U;
    assert(physical >= FB && physical - FB < VRAM - 4096U);
    return physical;
}
void cvgpu_unmap_mmio(uint32_t linear) { assert(linear); ++unmaps; }

static uint32_t r(uint32_t off) { return reg[(0x400U + off) / 4U]; }
static void draw(void)
{
    uint32_t unit = (r(0x2d0U) & 15U) == 2U ? 1U :
                    (r(0x2d0U) & 15U) == 4U ? 2U : 4U;
    uint32_t doff = (r(0x100U) & 0xfffffU) * 8U;
    uint32_t soff = (r(0x180U) & 0xfffffU) * 8U;
    uint32_t dpitch = (r(0x100U) >> 22) * 8U * unit;
    uint32_t spitch = (r(0x180U) >> 22) * 8U * unit;
    uint32_t dx = r(0x10cU) >> 16, dy = r(0x10cU) & 0xffffU;
    uint32_t sx = r(0x18cU) >> 16, sy = r(0x18cU) & 0xffffU;
    uint32_t width = r(0x118U) >> 16, height = r(0x118U) & 0xffffU;
    uint32_t ctl = r(0x130U), x, y, byte;
    int xdir = ctl & 1U ? 1 : -1, ydir = ctl & 2U ? 1 : -1;
    ++triggers;
    assert(dpitch && width && height);
    assert((r(0x2d4U) >> 16 & 31U) == 7U);
    for (y = 0U; y < height; ++y) for (x = 0U; x < width; ++x) {
        uint32_t xd = (uint32_t)((int)dx + xdir * (int)x);
        uint32_t yd = (uint32_t)((int)dy + ydir * (int)y);
        uint32_t dst = doff + yd * dpitch + xd * unit;
        assert(dst + unit <= VRAM);
        if (r(0x2d8U) == 0x300U) {
            uint32_t xs = (uint32_t)((int)sx + xdir * (int)x);
            uint32_t ys = (uint32_t)((int)sy + ydir * (int)y);
            uint32_t src = soff + ys * spitch + xs * unit;
            assert(src + unit <= VRAM);
            for (byte = 0U; byte < unit; ++byte)
                vram[dst + byte] = bad_copy ? 0U : vram[src + byte];
        } else {
            assert(r(0x2d8U) == 0x100U);
            for (byte = 0U; byte < unit; ++byte) {
                uint32_t component = byte;
                if (mode_bpp == 24U) {
                    assert(ctl & 0x80U);
                    /* Independent DWORD rotation: no direct x%3 shortcut. */
                    component = ((((ctl >> 8) & 7U) * 4U) +
                                  (dx % 4U) + x) % 3U;
                }
                vram[dst + byte] = bad_pixel ? 0U :
                    (uint8_t)(r(0x2c4U) >> (component * 8U));
            }
        }
    }
}
static void drain(void)
{
    uint32_t i;
    for (i = 0U; i < pending_count; ++i) {
        reg[pending_reg[i] / 4U] = pending_value[i];
        if (pending_reg[i] == 0x518U) draw();
    }
    pending_count = 0U;
}
uint32_t cvmach64_test_read(uint32_t linear, uint32_t offset)
{
    assert(linear == 1U && offset < 4096U);
    if (offset == 0x710U) { ++fifo_reads; if (!hanging) drain();
                            return hanging ? 0xffffU : 0U; }
    if (offset == 0x738U) { ++gui_reads; if (!hanging) drain();
                            return hanging ? 1U : 0U; }
    assert(!pending_count); /* FIFOed state must be read only after idle. */
    return reg[offset / 4U];
}
void cvmach64_test_write(uint32_t linear, uint32_t offset, uint32_t value)
{
    assert(linear == 1U && offset < 4096U); ++writes;
    /* Firmware scanout, clocks and PCI are never written. */
    assert(offset == 0x42cU || offset == 0x4d0U || offset >= 0x500U);
    if (offset < 0x500U) {
        reg[offset / 4U] = value;
        if (offset == 0x42cU && (value & 0x00800000U)) {
            memcpy(cache, vram, VRAM); ++cache_flushes;
        }
    } else {
        if (pending_count >= 16U) ++writes_without_fifo;
        assert(pending_count < 32U);
        pending_reg[pending_count] = offset;
        pending_value[pending_count++] = value;
        if (offset == 0x6a8U) { reg[0x6a0U / 4U] = value & 0xffffU;
                               reg[0x6a4U / 4U] = value >> 16; }
        if (offset == 0x6b4U) { reg[0x6acU / 4U] = value & 0xffffU;
                               reg[0x6b0U / 4U] = value >> 16; }
    }
}
uint8_t cvmach64_test_read8(uint32_t linear, uint32_t offset)
{ assert(linear >= FB && offset < 4096U); return cache[linear - FB + offset]; }
void cvmach64_test_write8(uint32_t linear, uint32_t offset, uint8_t value)
{ assert(linear >= FB && offset < 4096U);
  cache[linear - FB + offset] = vram[linear - FB + offset] = value; }

static void init(uint32_t bpp)
{
    uint32_t i, depth = bpp == 16U ? 4U : bpp == 24U ? 5U : 6U;
    assert(!cvmach64_owned());
    mode_bpp = bpp; pitch = 1024U * (bpp / 8U);
    memset(reg, 0, sizeof(reg));
    /* Distinct prior context values catch zero-filled or incomplete restore. */
    for (i = 0x500U / 4U; i < 0x710U / 4U; ++i)
        reg[i] = (i * 0x197U) ^ 0x25173U;
    for (i = 0U; i < VRAM; ++i) vram[i] = cache[i] = (uint8_t)(i * 17U + 93U);
    memcpy(original, vram, VRAM);
    reg[0x400U / 4U] = 127U << 16; reg[0x408U / 4U] = 767U << 16;
    reg[0x414U / 4U] = 128U << 22;
    reg[0x41cU / 4U] = 0x03000000U | depth << 8;
    reg[0x4b0U / 4U] = 11U; reg[0x4e0U / 4U] = 0x4c4dU;
    reg[0x4d0U / 4U] = 0x80U; reg[0x42cU / 4U] = 0x0200U;
    memcpy(savedreg, reg, sizeof(reg));
    writes = triggers = map_count = unmaps = fifo_reads = gui_reads = 0U;
    hanging = bad_pixel = bad_copy = absent = bad_bar = bad_crtc = 0U;
    cache_flushes = pending_count = writes_without_fifo = 0U; chip = 0x4c4d1002U;
}
static uint32_t bind(uint32_t step)
{ return cvmach64_bind_step(FB, pitch * 768U * 2U, 1024U, 768U,
                            pitch, mode_bpp, VRAM, step); }
static void restored(void)
{
    static const uint32_t fields[] = {0x42cU,0x4d0U,0x500U,0x504U,0x508U,
        0x510U,0x530U,0x580U,0x584U,0x588U,0x590U,0x5b4U,0x5fcU,
        0x640U,0x688U,0x6a0U,0x6a4U,0x6acU,0x6b0U,0x6c4U,0x6c8U,
        0x6ccU,0x6d0U,0x6d4U,0x6d8U,0x708U};
    uint32_t i;
    assert(!cvmach64_owned() && !cvmach64_status.ready && !cvmach64_status.caps);
    for (i = 0U; i < sizeof(fields) / sizeof(fields[0]); ++i)
        assert(reg[fields[i]/4U] == savedreg[fields[i]/4U]);
    assert(!writes_without_fifo && !pending_count);
}
int main(void)
{
    uint32_t bpp, step, x, before;
    assert(sizeof(cvmach64_status_info) == 128U);
    for (bpp = 16U; bpp <= 32U; bpp += 8U) {
        for (step = 1U; step <= 3U; ++step) {
            init(bpp); assert(bind(step) == 1U); restored();
            assert(!memcmp(original, vram, VRAM));
            if (step == 1U) assert(!writes && !triggers);
            if (step == 3U) assert(triggers == 3U && cache_flushes >= 3U &&
                                    cvmach64_status.selftests == 1U &&
                                    cvmach64_status.copy_selftests == 1U);
        }
        init(bpp); assert(bind(4U) == 1U);
        assert(cvmach64_status.ready && cvmach64_status.caps == 5U);
        for (x = 0U; x < 12U; ++x) {
            uint32_t off = pitch * 10U + x * (bpp / 8U), byte;
            assert(cvmach64_fill(off, 3U, 0x00abcdefU, bpp / 8U) == 0U);
            for (byte = 0U; byte < 3U * bpp / 8U; ++byte)
                assert(cache[off + byte] == (uint8_t)(0x00abcdefU >>
                                        ((byte % (bpp / 8U)) * 8U)));
        }
        before = triggers;
        assert(cvmach64_fill(1U, 7U, 0U, bpp / 8U) == 1U);
        assert(cvmach64_page_copy(0U, 0U, 1U, 1U, pitch) == 1U);
        assert(cvmach64_fill(0xffffffffU, 0xffffffffU, 0U,bpp / 8U) == 1U);
        assert(triggers == before);
        memcpy(original, vram, VRAM);
        /* Overlapping two-row reverse copy and shifted byte/pixel origins. */
        assert(cvmach64_page_copy(pitch * 10U, pitch * 11U + 2U * (bpp/8U),
                                  16U * (bpp/8U), 2U, pitch) == 0U);
        for (x = 0U; x < 2U; ++x)
            assert(!memcmp(vram + pitch * (11U+x) + 2U*(bpp/8U),
                           original + pitch * (10U+x), 16U*(bpp/8U)));
        before = triggers; assert(cvmach64_release() == 0U); restored();
        assert(triggers == before); /* Restoration must not launch a draw. */
    }
    init(24U); bad_pixel = 1U; assert(bind(4U) == 0U); restored();
    assert(!memcmp(original,vram,VRAM) && cvmach64_status.error_stage == 4U);
    init(24U); bad_copy = 1U; assert(bind(4U) == 0U); restored();
    assert(!memcmp(original,vram,VRAM) && cvmach64_status.copy_selftests == 0U);
    init(24U); hanging = 1U; assert(bind(4U) == 0U); restored();
    assert(cvmach64_status.fifo_timeouts == 1U);
    init(24U); assert(bind(4U) == 1U); hanging = 1U;
    assert(cvmach64_fill(0U,1U,0U,3U) == 2U);
    assert(cvmach64_owned() && !cvmach64_status.ready);
    assert(cvmach64_release() == 1U && cvmach64_owned());
    hanging = 0U; assert(cvmach64_release() == 0U); restored();
    init(24U); chip = 0x4c4e1002U; assert(bind(4U) == 0U && !writes);
    init(24U); bad_bar = 1U; assert(bind(4U) == 0U && !writes);
    init(24U); absent = 1U; assert(bind(4U) == 0U && !writes);
    init(24U); reg[0x414U/4U] = 127U << 22; assert(bind(4U) == 0U && !writes);
    puts("Mach64 posted-MMIO, readback-cache,16/24/32bpp, qualification, overlap, rollback tests passed");
    return 0;
}
