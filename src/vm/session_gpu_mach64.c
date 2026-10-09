/* Exact ATI1002:4c4d Mach64 Mobility 2D PIO backend. Independently implemented
 * from ATI RAGE Pro Programmer's Guide5.2/6.4 and pinned X.Org register facts.
 * Firmware mode/PLL/panel/scanout/PCI remain untouched. See native-mach64-2d.md. */
#include <stdint.h>
#include "session_gpu_mach64.h"

extern uint32_t cvdev_in(uint32_t port, uint32_t size);
extern void cvdev_out(uint32_t port, uint32_t value, uint32_t size);
extern uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes);
extern void cvgpu_unmap_mmio(uint32_t linear);

#define MC_ID 0x4c4d1002UL
#define MC_SPINS 65536UL
#define MC_BLOCK0 0x400U
#define MC_MEMBUF 0x02cU
#define MC_GEN_TEST 0x0d0U
#define MC_FIFO 0x310U
#define MC_GUI_STAT 0x338U
#define MC_DST_OFF 0x100U
#define MC_DST_X 0x104U
#define MC_DST_Y 0x108U
#define MC_DST_YX 0x10cU
#define MC_DST_WIDTH 0x110U
#define MC_DST_HW 0x118U
#define MC_DST_CNTL 0x130U
#define MC_SRC_OFF 0x180U
#define MC_SRC_X 0x184U
#define MC_SRC_Y 0x188U
#define MC_SRC_YX 0x18cU
#define MC_SRC_WIDTH 0x190U
#define MC_SRC_CNTL 0x1b4U
#define MC_SCALE 0x1fcU
#define MC_HOST 0x240U
#define MC_PAT 0x288U
#define MC_SC_L 0x2a0U
#define MC_SC_R 0x2a4U
#define MC_SC_LR 0x2a8U
#define MC_SC_T 0x2acU
#define MC_SC_B 0x2b0U
#define MC_SC_TB 0x2b4U
#define MC_COLOUR 0x2c4U
#define MC_MASK 0x2c8U
#define MC_CHAIN 0x2ccU
#define MC_PIX 0x2d0U
#define MC_MIX 0x2d4U
#define MC_SRC 0x2d8U
#define MC_CMP 0x308U
#define MC_RBCACHE 0x00800000UL
#define MC_GUI_ENABLE 0x00000100UL

volatile cvmach64_status_info cvmach64_status;
/* No triggering register is replayed during restoration. Packed scissor/YX
 * write aliases are saved through their individual readable fields. */
static const uint32_t context[] = {
    MC_DST_OFF, MC_DST_X, MC_DST_Y, MC_DST_WIDTH, MC_DST_CNTL,
    MC_SRC_OFF, MC_SRC_X, MC_SRC_Y, MC_SRC_WIDTH, MC_SRC_CNTL,
    MC_SCALE, MC_HOST, MC_PAT, MC_SC_L, MC_SC_R, MC_SC_T, MC_SC_B,
    MC_COLOUR, MC_MASK, MC_CHAIN, MC_PIX, MC_MIX, MC_SRC, MC_CMP
};
#define MC_CONTEXT (sizeof(context) / sizeof(context[0]))
typedef struct {
    uint32_t mmio, probe, bytes, pitch, bpp, pixelbytes, unitbytes;
    uint32_t scratch_page, probe_offset, probe_delta;
    uint32_t saved[MC_CONTEXT], gen_test, mem_buf;
    uint8_t saved_valid, probe_saved;
} mach64_state;
static mach64_state g;
static uint8_t probe_bytes[4096], bus_seen[256], bus_queue[256];

#ifdef CVMACH64_HOST_TEST
extern uint32_t cvmach64_test_read(uint32_t linear, uint32_t offset);
extern void cvmach64_test_write(uint32_t linear, uint32_t offset, uint32_t value);
extern uint8_t cvmach64_test_read8(uint32_t linear, uint32_t offset);
extern void cvmach64_test_write8(uint32_t linear, uint32_t offset, uint8_t value);
#define READ(o) cvmach64_test_read(g.mmio, MC_BLOCK0 + (o))
#define WRITE(o,v) cvmach64_test_write(g.mmio, MC_BLOCK0 + (o), (v))
#define PROBE_READ(o) cvmach64_test_read8(g.probe, (o))
#define PROBE_WRITE(o,v) cvmach64_test_write8(g.probe, (o), (v))
#else
#define READ(o) (*(volatile uint32_t *)(g.mmio + MC_BLOCK0 + (o)))
#define WRITE(o,v) (*(volatile uint32_t *)(g.mmio + MC_BLOCK0 + (o)) = (v))
#define PROBE_READ(o) (*(volatile uint8_t *)(g.probe + (o)))
#define PROBE_WRITE(o,v) (*(volatile uint8_t *)(g.probe + (o)) = (v))
#endif

static void zero(void *p, uint32_t bytes)
{
    uint8_t *b = (uint8_t *)p;
    while (bytes--) *b++ = 0U;
}

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    cvdev_out(0xcf8U, 0x80000000UL | ((uint32_t)bus << 16) |
              ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xfcU), 4U);
    return cvdev_in(0xcfcU, 4U);
}

static uint32_t find_mmio(uint32_t physical)
{
    uint16_t head, tail = 1U;
    zero(bus_seen, sizeof(bus_seen)); bus_queue[0] = 0U; bus_seen[0] = 1U;
    for (head = 0U; head < tail; ++head) {
        uint8_t bus = bus_queue[head], dev;
        for (dev = 0U; dev < 32U; ++dev) {
            uint32_t id = pci_read(bus, dev, 0U, 0U), header;
            uint8_t fn, functions;
            if ((id & 0xffffU) == 0xffffU) continue;
            header = pci_read(bus, dev, 0U, 0x0cU);
            functions = ((header >> 23) & 1U) ? 8U : 1U;
            for (fn = 0U; fn < functions; ++fn) {
                uint32_t cls = pci_read(bus, dev, fn, 8U) >> 16;
                if (cls == 0x0604U) {
                    uint32_t buses = pci_read(bus, dev, fn, 0x18U);
                    uint8_t secondary = (uint8_t)(buses >> 8);
                    if (secondary && secondary <= (uint8_t)(buses >> 16) &&
                        !bus_seen[secondary] && tail < 256U) {
                        bus_seen[secondary] = 1U; bus_queue[tail++] = secondary;
                    }
                }
                if (cls == 0x0300U && pci_read(bus, dev, fn, 0U) == MC_ID) {
                    uint32_t fb = pci_read(bus, dev, fn, 0x10U);
                    uint32_t mmio = pci_read(bus, dev, fn, 0x18U);
                    cvmach64_status.device_id = MC_ID;
                    cvmach64_status.framebuffer_physical = fb & ~15UL;
                    cvmach64_status.mmio_physical = (mmio & ~15UL) + MC_BLOCK0;
                    if (!(pci_read(bus, dev, fn, 4U) & 2U) ||
                        (fb & 7U) || (mmio & 15U) ||
                        (fb & ~15UL) != physical || mmio < 0x100000UL ||
                        mmio > 0xfffff000UL || (mmio & 0xfffU)) return 0U;
                    return mmio;
                }
            }
        }
    }
    return 0U;
}

static uint32_t fifo(uint32_t entries)
{
    uint32_t spin, status;
    for (spin = 0U; spin < MC_SPINS; ++spin) {
        status = READ(MC_FIFO); cvmach64_status.last_status = status;
        if (!(status & 0x80000000UL) &&
            (status & 0xffffU) <= (0x8000U >> entries)) return 0U;
    }
    ++cvmach64_status.fifo_timeouts; return 1U;
}

static uint32_t idle(void)
{
    uint32_t spin, status;
    if (fifo(16U)) return 1U;
    for (spin = 0U; spin < MC_SPINS; ++spin) {
        status = READ(MC_GUI_STAT); cvmach64_status.last_status = status;
        if (status != 0xffffffffUL && !(status & 1U)) return 0U;
    }
    ++cvmach64_status.idle_timeouts; return 1U;
}

uint32_t cvmach64_sync(void)
{
    if (!g.saved_valid) return 0U;
    if (idle()) { cvmach64_status.ready = 0U; cvmach64_status.caps = 0U;
                 return 1U; }
    WRITE(MC_MEMBUF, g.mem_buf | MC_RBCACHE);
    (void)READ(MC_MEMBUF); /* Complete posted register write before CPU VRAM. */
    return 0U;
}

uint32_t cvmach64_owned(void) { return g.saved_valid ? 1U : 0U; }

uint32_t cvmach64_release(void)
{
    uint32_t i;
    cvmach64_status.ready = cvmach64_status.caps = 0U;
    if (g.saved_valid) {
        if (cvmach64_sync()) goto unsafe;
        if (g.probe_saved) {
            for (i = 0U; i < sizeof(probe_bytes); ++i) PROBE_WRITE(i, probe_bytes[i]);
            g.probe_saved = 0U;
        }
        for (i = 0U; i < MC_CONTEXT; ++i) {
            if (fifo(1U)) goto unsafe;
            WRITE(context[i], g.saved[i]);
        }
        if (idle()) goto unsafe;
        WRITE(MC_MEMBUF, g.mem_buf);
        (void)READ(MC_MEMBUF);
        WRITE(MC_GEN_TEST, g.gen_test);
        if ((READ(MC_GEN_TEST) ^ g.gen_test) & MC_GUI_ENABLE) goto unsafe;
        g.saved_valid = 0U; cvmach64_status.owns_engine = 0U;
    }
    if (g.probe) cvgpu_unmap_mmio(g.probe);
    if (g.mmio) cvgpu_unmap_mmio(g.mmio);
    zero(&g, sizeof(g)); return 0U;
unsafe:
    cvmach64_status.error_stage = 8U; return 1U;
}

static void command_write(uint32_t reg, uint32_t value)
{
    WRITE(reg, value); ++cvmach64_status.command_words;
}

static uint32_t format(void)
{
    uint32_t f = g.bpp == 16U ? 4U : g.bpp == 32U ? 6U : 2U;
    return 0x01000000UL | f | (f << 8);
}

/* x coordinates and widths are engine units (bytes in packed24 mode). */
static uint32_t rectangle(uint32_t base, uint32_t pitch, uint32_t x,
                          uint32_t y, uint32_t width, uint32_t rows,
                          uint32_t colour)
{
    uint32_t ctl = 3U;
    if (g.bpp == 24U) ctl |= 0x80U | (((x / 4U) % 6U) << 8);
    if (fifo(12U)) return 1U;
    command_write(MC_DST_OFF, (base >> 3) | ((pitch / g.unitbytes / 8U) << 22));
    command_write(MC_PIX, format());
    command_write(MC_MASK, g.bpp == 24U ? 0x00ffffffUL : 0xffffffffUL);
    command_write(MC_SRC, 0x100U);
    command_write(MC_MIX, 0x00070003UL);
    command_write(MC_COLOUR, colour);
    command_write(MC_CMP, 0U);
    command_write(MC_SC_LR, 0x0fff0000UL);
    command_write(MC_SC_TB, 0x3fff0000UL);
    command_write(MC_DST_CNTL, ctl);
    command_write(MC_DST_YX, (x << 16) | y);
    command_write(MC_DST_HW, (width << 16) | rows);
    return idle();
}

static uint32_t copy_rectangle(uint32_t source_base, uint32_t dest_base,
                               uint32_t pitch, uint32_t sx, uint32_t sy,
                               uint32_t dx, uint32_t dy, uint32_t width,
                               uint32_t rows, uint32_t reverse)
{
    uint32_t ctl = reverse ? 0U : 3U;
    if (reverse) { sx += width - 1U; dx += width - 1U;
                   sy += rows - 1U; dy += rows - 1U; }
    if (g.bpp == 24U) ctl |= 0x80U | (((dx / 4U) % 6U) << 8);
    if (fifo(14U)) return 1U;
    command_write(MC_SRC_OFF, (source_base >> 3) | ((pitch / g.unitbytes / 8U) << 22));
    command_write(MC_DST_OFF, (dest_base >> 3) | ((pitch / g.unitbytes / 8U) << 22));
    command_write(MC_PIX, format());
    command_write(MC_MASK, g.bpp == 24U ? 0x00ffffffUL : 0xffffffffUL);
    command_write(MC_SRC, 0x300U);
    command_write(MC_MIX, 0x00070003UL);
    command_write(MC_CMP, 0U);
    command_write(MC_SC_LR, 0x0fff0000UL);
    command_write(MC_SC_TB, 0x3fff0000UL);
    command_write(MC_DST_CNTL, ctl);
    command_write(MC_SRC_YX, (sx << 16) | sy);
    command_write(MC_SRC_WIDTH, width);
    command_write(MC_DST_YX, (dx << 16) | dy);
    command_write(MC_DST_HW, (width << 16) | rows);
    return idle();
}

static uint32_t pixel_proof(void)
{
    uint32_t i, row, x, byte, delta = g.probe_delta, pitch = 96U;
    uint32_t colour1 = 0x0013579bUL, colour2 = 0x002468acUL;
    uint32_t expected, actual;
    for (i = 0U; i < sizeof(probe_bytes); ++i) probe_bytes[i] = PROBE_READ(i);
    g.probe_saved = 1U;
    for (i = 0U; i < 1024U; ++i) PROBE_WRITE(delta + i, 0xe5U);
    if (rectangle(g.probe_offset, pitch, 2U * g.pixelbytes / g.unitbytes,
                  0U, 2U * g.pixelbytes / g.unitbytes, 2U, colour1) ||
        rectangle(g.probe_offset, pitch, 5U * g.pixelbytes / g.unitbytes,
                  0U, 2U * g.pixelbytes / g.unitbytes, 2U, colour2) ||
        cvmach64_sync()) return 1U;
    for (row = 0U; row < 2U; ++row) for (x = 0U; x < 8U; ++x)
        for (byte = 0U; byte < g.pixelbytes; ++byte) {
            expected = x >= 2U && x < 4U ? (colour1 >> (byte * 8U)) & 255U :
                       x >= 5U && x < 7U ? (colour2 >> (byte * 8U)) & 255U : 0xe5U;
            actual = PROBE_READ(delta + row * pitch + x * g.pixelbytes + byte);
            if (actual != expected) {
                cvmach64_status.probe_actual = actual;
                cvmach64_status.probe_expected = expected; return 1U;
            }
        }
    ++cvmach64_status.selftests;
    if (copy_rectangle(g.probe_offset, g.probe_offset + 576U, pitch,
                       0U, 0U, 0U, 0U, 8U * g.pixelbytes / g.unitbytes,
                       2U, 0U) || cvmach64_sync()) return 1U;
    for (row = 0U; row < 2U; ++row) for (i = 0U; i < 8U * g.pixelbytes; ++i) {
        expected = PROBE_READ(delta + row * pitch + i);
        actual = PROBE_READ(delta + 576U + row * pitch + i);
        if (actual != expected) {
            cvmach64_status.probe_actual = actual;
            cvmach64_status.probe_expected = expected; return 1U;
        }
    }
    ++cvmach64_status.copy_selftests;
    for (i = 0U; i < sizeof(probe_bytes); ++i) PROBE_WRITE(i, probe_bytes[i]);
    g.probe_saved = 0U; return 0U;
}

uint32_t cvmach64_bind_step(uint32_t physical, uint32_t bytes, uint32_t width,
                           uint32_t height, uint32_t pitch, uint32_t bpp,
                           uint32_t usable_bytes, uint32_t step)
{
    uint32_t mmio, i, gen, crtc, ram, pixelbytes = bpp / 8U;
    uint32_t binds = cvmach64_status.binds + 1U, depth;
    if (g.saved_valid) return 2U;
    if (g.mmio) (void)cvmach64_release();
    zero((void *)&cvmach64_status, sizeof(cvmach64_status));
    cvmach64_status.binds = binds; cvmach64_status.error_stage = 1U;
    cvmach64_status.width = width; cvmach64_status.height = height;
    cvmach64_status.pitch = pitch; cvmach64_status.bpp = bpp;
    if (step < 1U || step > 4U || (bpp != 16U && bpp != 24U && bpp != 32U) ||
        physical < 0x100000UL || !width || width > 2048U || !height || height > 2048U ||
        !pitch || pitch > 8192U || pitch % pixelbytes || (pitch & 7U) ||
        pitch < width * pixelbytes || !bytes || bytes > 0x1000000UL ||
        bytes < pitch * height || usable_bytes > 0x1000000UL ||
        physical > 0xffffffffUL - usable_bytes) return 0U;
    g.unitbytes = bpp == 24U ? 1U : pixelbytes;
    if (pitch / g.unitbytes > 4096U || pitch / g.unitbytes / 8U > 1023U ||
        bytes / pitch >= 16383U) return 0U;
    mmio = find_mmio(physical); if (!mmio) return 0U;
    cvmach64_status.error_stage = 2U;
    g.mmio = cvgpu_map_mmio(mmio, 4096U); if (!g.mmio) return 0U;
    depth = bpp == 16U ? 4U : bpp == 24U ? 5U : 6U;
    gen = READ(0x01cU); crtc = READ(0x014U);
    ram = READ(0x0b0U) & 15U;
    ram = ram < 8U ? (ram + 1U) * 524288UL :
          ram < 12U ? (ram - 3U) * 1048576UL : (ram - 7U) * 2097152UL;
    cvmach64_status.vram_bytes = ram;
    /* CRTC firmware pitch is in8-pixel units, unlike packed24 engine pitch. */
    if ((READ(0x0e0U) & 0xffffU) != 0x4c4dU ||
        (gen & 0x03000000UL) != 0x03000000UL || (gen & 0x00200001UL) ||
        ((gen >> 8) & 7U) != depth ||
        (((READ(0U) >> 16) & 0x1ffU) + 1U) * 8U != width ||
        (((READ(8U) >> 16) & 0x7ffU) + 1U) != height ||
        (crtc >> 22) * 8U * pixelbytes != pitch || (crtc & 0xfffffU) != 0U ||
        usable_bytes > ram || usable_bytes < 65536U + 8192U ||
        bytes > usable_bytes - 65536U - 8192U) goto fail;
    g.bytes = bytes; g.pitch = pitch; g.bpp = bpp; g.pixelbytes = pixelbytes;
    g.scratch_page = (bytes + 4095U) & ~4095UL;
    g.probe_offset = ((g.scratch_page + 23U) / 24U) * 24U;
    g.probe_delta = g.probe_offset - g.scratch_page;
    if (g.scratch_page > usable_bytes - 65536U - 4096U) goto fail;
    cvmach64_status.scratch_offset = g.probe_offset;
    cvmach64_status.scratch_bytes = 4096U;
    if (idle()) goto fail;
    if (step == 1U) { (void)cvmach64_release();
                     cvmach64_status.error_stage = 0U; return 1U; }
    cvmach64_status.error_stage = 3U;
    g.gen_test = READ(MC_GEN_TEST); g.mem_buf = READ(MC_MEMBUF);
    for (i = 0U; i < MC_CONTEXT; ++i) g.saved[i] = READ(context[i]);
    g.saved_valid = 1U; cvmach64_status.owns_engine = 1U;
    if (!(g.gen_test & MC_GUI_ENABLE)) {
        WRITE(MC_GEN_TEST, g.gen_test | MC_GUI_ENABLE);
        if (!(READ(MC_GEN_TEST) & MC_GUI_ENABLE) || idle()) goto fail;
    }
    if (fifo(5U)) goto fail;
    command_write(MC_SCALE, 0U); command_write(MC_HOST, 0U);
    command_write(MC_PAT, 0U); command_write(MC_SRC_CNTL, 0x10U);
    command_write(MC_CHAIN, bpp == 16U ? 0x8410U : 0x8080U);
    if (idle()) goto fail;
    if (step == 2U) { if (cvmach64_release()) return 2U;
                     cvmach64_status.error_stage = 0U; return 1U; }
    cvmach64_status.error_stage = 4U;
    g.probe = cvgpu_map_mmio(physical + g.scratch_page, 4096U);
    if (!g.probe || pixel_proof()) goto fail;
    if (step == 3U) { if (cvmach64_release()) return 2U;
                     cvmach64_status.error_stage = 0U; return 1U; }
    cvmach64_status.ready = 1U;
    cvmach64_status.caps = CVMACH64_CAP_FILL | CVMACH64_CAP_COPY;
    cvmach64_status.error_stage = 0U; return 1U;
fail:
    return cvmach64_release() ? 2U : 0U;
}

uint32_t cvmach64_bind(uint32_t physical, uint32_t bytes, uint32_t width,
                      uint32_t height, uint32_t pitch, uint32_t bpp,
                      uint32_t usable_bytes)
{
    return cvmach64_bind_step(physical, bytes, width, height, pitch, bpp,
                             usable_bytes, 1U);
}

uint32_t cvmach64_fill(uint32_t offset, uint32_t pixels, uint32_t colour,
                     uint32_t pixelbytes)
{
    uint32_t rowpixels, x, y, count;
    if (!cvmach64_status.ready || pixelbytes != g.pixelbytes || !pixels ||
        offset >= g.bytes || offset % pixelbytes ||
        pixels > (g.bytes - offset) / pixelbytes) return 1U;
    rowpixels = g.pitch / pixelbytes;
    cvmach64_status.error_stage = 5U;
    while (pixels) {
        x = (offset % g.pitch) / pixelbytes; y = offset / g.pitch;
        count = pixels < rowpixels - x ? pixels : rowpixels - x;
        if (rectangle(0U, g.pitch, x * pixelbytes / g.unitbytes, y,
                      count * pixelbytes / g.unitbytes, 1U, colour) ||
            cvmach64_sync()) { cvmach64_status.ready = cvmach64_status.caps = 0U;
                               return 2U; }
        offset += count * pixelbytes; pixels -= count;
    }
    ++cvmach64_status.fills; cvmach64_status.error_stage = 0U; return 0U;
}

uint32_t cvmach64_page_copy(uint32_t source, uint32_t dest, uint32_t rowbytes,
                          uint32_t rows, uint32_t pitch)
{
    uint32_t extent, sx, dx;
    if (!cvmach64_status.ready || pitch != g.pitch || !rows || rows > 16383U ||
        !rowbytes || rowbytes > pitch || rowbytes % g.pixelbytes ||
        source >= g.bytes || dest >= g.bytes || source % g.pixelbytes ||
        dest % g.pixelbytes) return 1U;
    extent = (rows - 1U) * pitch + rowbytes;
    if (extent > g.bytes - source || extent > g.bytes - dest ||
        (source % pitch) > pitch - rowbytes || (dest % pitch) > pitch - rowbytes)
        return 1U;
    if (source == dest) return 0U;
    sx = (source % pitch) / g.unitbytes; dx = (dest % pitch) / g.unitbytes;
    cvmach64_status.error_stage = 6U;
    if (copy_rectangle(0U, 0U, pitch, sx, source / pitch, dx, dest / pitch,
                       rowbytes / g.unitbytes, rows,
                       dest > source && dest - source < extent) ||
        cvmach64_sync()) { cvmach64_status.ready = cvmach64_status.caps = 0U;
                          return 2U; }
    ++cvmach64_status.blits; cvmach64_status.error_stage = 0U; return 0U;
}
