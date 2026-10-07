/*
 * Native SuperSavage IX/C SDR (5333:8c2e) BCI PIO backend.
 * Protocol implementation researched from the permissively licensed X.Org
 * xf86-video-savage 2.4.1, Mesa 7.11.2 Savage driver and Linux v6.2 Savage DRM.
 * See docs/design/native-supersavage-bci.md for pinned sources and decisions.
 * No clocks, timings, scanout addresses, PCI decode or DMA are programmed.
 * A tiled private 3D destination is transferred by the hardware 2D engine
 * to/from the BIOS-established linear front buffer. No CPU rasterizer runs.
 */
#include <stdint.h>
#include "session_gpu_savage.h"

extern uint32_t cvdev_in(uint32_t port, uint32_t size);
extern void cvdev_out(uint32_t port, uint32_t value, uint32_t size);
extern uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes);
extern void cvgpu_unmap_mmio(uint32_t linear);

#define SV_ID 0x8c2e5333UL
#define SV_MMIO_BYTES 0x00080000UL
#define SV_FIFO 0x00010000UL
#define SV_ALT_STATUS 0x00048c60UL
#define SV_CONTROL 0x00048c18UL
#define SV_SPINS 65536UL
#define SV_USED 0x001fffffUL
#define SV_IDLE 0x00e00000UL
#define SV_PLANE_WRITE 0x8128UL
#define SV_GBD_HIGH 0x816cUL
#define SV_BW_DISABLE 0x10000000UL
#define SV_TILE_DEST 0x01000000UL
#define SV_RECT_FILL 0x4bf08c00UL /* positive XY, PATCOPY, colour, new PBD */
#define SV_RECT_COPY 0x4bcc0d40UL /* positive XY, SRCCOPY, new PBD and SBD */
#define SV_SET_REG 0x96000000UL
#define SV_WAIT_2D 0xc0020000UL
#define SV_WAIT_BOTH 0xc0030000UL
#define SV_DRAW_TRIANGLE 0x800300faUL /* 3 * [float X,Y,Z,ARGB] */
#define SV_MAX_RECT_PIXELS 65536UL

volatile cvsavage_status_info cvsavage_status;

/* Registers changed by this backend, including descriptor/3D state. */
static const uint32_t saved_offsets[] = {
    0x8128UL, 0x812cUL, 0x8134UL, 0x8168UL, 0x816cUL,
    0x8170UL, 0x8174UL, 0x8178UL, 0x817cUL,
    0x48c0cUL, 0x48c10UL, 0x48c18UL,
    0x48584UL, 0x4859cUL, 0x485a0UL, 0x485a4UL, 0x485a8UL,
    0x485ccUL, 0x485d0UL, 0x485d4UL, 0x485d8UL,
    0x485dcUL, 0x485e0UL, 0x485e4UL, 0x485e8UL, 0x485ecUL
};
#define SV_SAVE_COUNT (sizeof(saved_offsets) / sizeof(saved_offsets[0]))
typedef struct {
    uint32_t mmio, probe, bytes, width, height, pitch, bpp, pixelbytes;
    uint32_t descriptor, scratch_descriptor, scratch_offset, scratch_bytes;
    uint32_t probe_offset;
    uint32_t saved[SV_SAVE_COUNT];
    uint8_t cr38, cr39, cr40, cr31, saved_valid, cr_enabled;
} savage_state;
static savage_state g;
static uint32_t probe_saved[512];
static uint8_t panel_bus, panel_dev, panel_fn, panel_cached;

#ifdef CVSAVAGE_HOST_TEST
extern uint32_t cvsavage_test_read(uint32_t linear, uint32_t offset);
extern void cvsavage_test_write(uint32_t linear, uint32_t offset, uint32_t value);
#define SV_READ(base, offset) cvsavage_test_read((base), (offset))
#define SV_WRITE(base, offset, value) cvsavage_test_write((base), (offset), (value))
#else
#define SV_READ(base, offset) (*(volatile uint32_t *)((base) + (offset)))
#define SV_WRITE(base, offset, value) (*(volatile uint32_t *)((base) + (offset)) = (value))
#endif

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    cvdev_out(0xcf8U, 0x80000000UL | ((uint32_t)bus << 16) |
              ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xfcU), 4U);
    return cvdev_in(0xcfcU, 4U);
}

static uint32_t memory_bar(uint8_t bus, uint8_t dev, uint8_t fn,
                            uint8_t index, uint32_t *base)
{
    uint32_t value = pci_read(bus, dev, fn, (uint8_t)(0x10U + 4U * index));
    /* SuperSavage exposes 32-bit memory BARs; reject other resource types. */
    if (value == 0xffffffffUL || (value & 7U) != 0U) return 0U;
    *base = value & 0xfffffff0UL;
    return *base >= 0x100000UL;
}

static uint8_t cr_read(uint8_t index)
{
    uint8_t old = (uint8_t)cvdev_in(0x3d4U, 1U), value;
    cvdev_out(0x3d4U, index, 1U);
    value = (uint8_t)cvdev_in(0x3d5U, 1U);
    cvdev_out(0x3d4U, old, 1U);
    return value;
}

static void cr_write(uint8_t index, uint8_t value)
{
    uint8_t old = (uint8_t)cvdev_in(0x3d4U, 1U);
    cvdev_out(0x3d4U, index, 1U);
    cvdev_out(0x3d5U, value, 1U);
    cvdev_out(0x3d4U, old, 1U);
}

static uint8_t sr_read(uint8_t index)
{
    uint8_t old = (uint8_t)cvdev_in(0x3c4U, 1U), value;
    cvdev_out(0x3c4U, index, 1U);
    value = (uint8_t)cvdev_in(0x3c5U, 1U);
    cvdev_out(0x3c4U, old, 1U);
    return value;
}

/* X.Org SavageGetPanelInfo's mobile sequencer report. Indexed VGA reads
 * need no MMIO mapping, CR40 enable, clock/timing or lock-state writes. */
uint32_t cvsavage_panel_probe(void)
{
    uint32_t pci_selector, bus, dev, fn, functions, mmio, width, height, i;
    uint8_t first[5], second[5];
    cvsavage_status.panel_flags = 0U;
    cvsavage_status.panel_width = 0U; cvsavage_status.panel_height = 0U;
    pci_selector = cvdev_in(0xcf8U, 4U);
    if (panel_cached && pci_read(panel_bus, panel_dev, panel_fn, 0U) != SV_ID)
        panel_cached = 0U;
    if (!panel_cached) {
        for (bus = 0U; bus < 256U && !panel_cached; ++bus) {
            for (dev = 0U; dev < 32U && !panel_cached; ++dev) {
                if ((pci_read((uint8_t)bus, (uint8_t)dev, 0U, 0U) & 0xffffU) == 0xffffU)
                    continue;
                functions = (pci_read((uint8_t)bus, (uint8_t)dev, 0U, 0x0cU) &
                             0x00800000UL) ? 8U : 1U;
                for (fn = 0U; fn < functions; ++fn) {
                    if (pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0U) == SV_ID) {
                        panel_bus = (uint8_t)bus; panel_dev = (uint8_t)dev;
                        panel_fn = (uint8_t)fn; panel_cached = 1U; break;
                    }
                }
            }
        }
    }
    if (!panel_cached ||
        ((pci_read(panel_bus, panel_dev, panel_fn, 8U) >> 16) & 0xffffU) != 0x0300U ||
        !(pci_read(panel_bus, panel_dev, panel_fn, 4U) & 1U) ||
        !memory_bar(panel_bus, panel_dev, panel_fn, 0U, &mmio)) goto unavailable;
    cvdev_out(0xcf8U, pci_selector, 4U);
    if (!(cvdev_in(0x3c3U, 1U) & 1U) || !(cvdev_in(0x3ccU, 1U) & 1U)) return 0U;
    first[0] = cr_read(0x6bU); first[1] = sr_read(0x61U);
    first[2] = sr_read(0x66U); first[3] = sr_read(0x69U); first[4] = sr_read(0x6eU);
    second[0] = cr_read(0x6bU); second[1] = sr_read(0x61U);
    second[2] = sr_read(0x66U); second[3] = sr_read(0x69U); second[4] = sr_read(0x6eU);
    for (i = 0U; i < 5U; ++i) if (first[i] != second[i]) return 0U;
    if (first[0] == 0xffU || !(first[0] & 2U)) return 0U;
    width = ((uint32_t)first[1] + (((uint32_t)first[2] & 2U) << 7) + 1U) * 8U;
    height = (uint32_t)first[3] + (((uint32_t)first[4] & 0x70U) << 4) + 1U;
    if (width < 640U || width > 1920U || height < 480U || height > 1200U ||
        width < height || width > height * 3U) return 0U;
    cvsavage_status.panel_width = width; cvsavage_status.panel_height = height;
    cvsavage_status.panel_flags = 1U;
    cvsavage_status.device_id = SV_ID;
    cvsavage_status.mmio_physical = mmio;
    return 1U;
unavailable:
    cvdev_out(0xcf8U, pci_selector, 4U);
    return 0U;
}

static uint32_t wait_idle(void)
{
    uint32_t i, value;
    for (i = 0U; i < SV_SPINS; ++i) {
        value = SV_READ(g.mmio, SV_ALT_STATUS);
        cvsavage_status.last_status = value;
        if (value == 0xffffffffUL) break;
        if ((value & (SV_USED | SV_IDLE)) == SV_IDLE) return 0U;
    }
    ++cvsavage_status.idle_timeouts;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    return 1U;
}

static uint32_t fifo(uint32_t words)
{
    uint32_t i, value;
    if (words > 32U) return 1U;
    for (i = 0U; i < SV_SPINS; ++i) {
        value = SV_READ(g.mmio, SV_ALT_STATUS);
        cvsavage_status.last_status = value;
        if (value == 0xffffffffUL) break;
        if ((value & SV_USED) <= 32U - words) return 0U;
    }
    ++cvsavage_status.fifo_timeouts;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    return 1U;
}

static uint32_t send(const uint32_t *values, uint32_t count)
{
    uint32_t i;
    if (fifo(count)) return 1U;
    for (i = 0U; i < count; ++i)
        SV_WRITE(g.mmio, SV_FIFO + i * 4U, values[i]);
    cvsavage_status.command_words += count;
    return 0U;
}

static uint32_t rectangle(uint32_t base, uint32_t descriptor, uint32_t x,
                           uint32_t y, uint32_t width, uint32_t height,
                           uint32_t colour)
{
    uint32_t values[6];
    values[0] = SV_RECT_FILL; values[1] = base; values[2] = descriptor;
    values[3] = colour; values[4] = (y << 16) | x;
    values[5] = (height << 16) | width;
    return send(values, 6U);
}

static uint32_t copy_rectangle(uint32_t dest, uint32_t dest_descriptor,
                               uint32_t source, uint32_t source_descriptor,
                               uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height)
{
    uint32_t values[8];
    values[0] = SV_RECT_COPY;
    values[1] = dest; values[2] = dest_descriptor;
    values[3] = source; values[4] = source_descriptor;
    values[5] = (y << 16) | x; values[6] = values[5];
    values[7] = (height << 16) | width;
    return send(values, 8U);
}

uint32_t cvsavage_owned(void)
{
    return g.saved_valid != 0U;
}

uint32_t cvsavage_sync(void)
{
    if (!g.saved_valid) return 0U;
    cvsavage_status.error_stage = 8U;
    if (wait_idle()) return 1U;
    cvsavage_status.error_stage = 0U;
    return 0U;
}

uint32_t cvsavage_release(void)
{
    uint32_t i;
    if (g.saved_valid) {
        if (wait_idle()) return 1U;
        /* Disable our PIO interface before returning original state. */
        SV_WRITE(g.mmio, SV_CONTROL, SV_READ(g.mmio, SV_CONTROL) & 0x3ff0U);
        for (i = 0U; i < SV_SAVE_COUNT; ++i)
            if (saved_offsets[i] != SV_CONTROL)
                SV_WRITE(g.mmio, saved_offsets[i], g.saved[i]);
        /* Original control last; no commands remain that could use it. */
        SV_WRITE(g.mmio, SV_CONTROL, g.saved[11]);
        cr_write(0x31U, g.cr31); cr_write(0x40U, g.cr40);
        cr_write(0x39U, g.cr39); cr_write(0x38U, g.cr38);
        g.saved_valid = 0U; g.cr_enabled = 0U;
    } else if (g.cr_enabled) {
        /* MMIO access was enabled, but no engine commands were submitted. */
        cr_write(0x40U, g.cr40);
        cr_write(0x39U, g.cr39); cr_write(0x38U, g.cr38);
        g.cr_enabled = 0U;
    }
    if (g.probe) { cvgpu_unmap_mmio(g.probe); g.probe = 0U; }
    if (g.mmio) { cvgpu_unmap_mmio(g.mmio); g.mmio = 0U; }
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    cvsavage_status.owns_engine = 0U;
    return 0U;
}

static uint32_t selftest(void)
{
    uint32_t saved[4], i, colour, expected, values[1], bad = 0U;
    colour = g.bpp == 32U ? 0x00123456UL : 0x5aa5U;
    expected = g.bpp == 32U ? colour : colour | (colour << 16);
    for (i = 0U; i < 4U; ++i) saved[i] = SV_READ(g.probe, i * 4U);
    if (rectangle(g.probe_offset, g.descriptor, 0U, 0U,
                  16U / g.pixelbytes, 1U, colour)) return 1U;
    values[0] = SV_WAIT_2D;
    if (send(values, 1U) || wait_idle()) return 1U;
    for (i = 0U; i < 4U; ++i)
        if ((SV_READ(g.probe, i * 4U) &
             (g.bpp == 32U ? 0x00ffffffUL : 0xffffffffUL)) != expected) bad = 1U;
    /* Idle has been proved; restore private VRAM even on a mismatch. */
    for (i = 0U; i < 4U; ++i) SV_WRITE(g.probe, i * 4U, saved[i]);
    if (!bad) ++cvsavage_status.selftests;
    return bad;
}

static uint32_t draw_state(uint32_t tile_pitch, uint32_t x0, uint32_t y0,
                           uint32_t x1, uint32_t y1)
{
    uint32_t values[10];
    values[0] = SV_SET_REG | (1U << 16) | 0x1eU;
    values[1] = 0x44000010UL;
    if (send(values, 2U)) return 1U;
    values[0] = SV_SET_REG | (4U << 16) | 0x24U;
    values[1] = 0x00850405UL; values[2] = 0x00870407UL;
    values[3] = 0U; values[4] = 0U;
    if (send(values, 5U)) return 1U;
    values[0] = SV_SET_REG | (9U << 16) | 0x30U;
    values[1] = 0U; values[2] = 0U; values[3] = 0U; values[4] = 0U;
    values[5] = 0x80U | tile_pitch / 128U |
                 ((g.scratch_offset >> 11) << 8) |
                 (g.bpp == 32U ? 0x80000000UL : 0U);
    values[6] = (y0 << 12) | x0; values[7] = (y1 << 12) | x1;
    values[8] = 0U; values[9] = 0x4f000000UL;
    return send(values, 10U);
}

/* Qualify linear PBD/SBD copies independently from the 3D/tiled path. */
static uint32_t copy_selftest(void)
{
    uint32_t i, row, values[1], descriptor, expected, observed, bad = 0U;
    for (i = 0U; i < 512U; ++i) probe_saved[i] = SV_READ(g.probe, i * 4U);
    for (row = 0U; row < 2U; ++row)
        for (i = 0U; i < 4U; ++i) {
            expected = g.bpp == 32U ? 0x00123456UL + i + row * 16U :
                                      0x12345678UL + i + row * 16U;
            SV_WRITE(g.probe, row * 128U + i * 4U, expected);
            SV_WRITE(g.probe, 512U + row * 128U + i * 4U, 0U);
        }
    descriptor = SV_BW_DISABLE | (g.bpp << 16) | (128U / g.pixelbytes);
    if (copy_rectangle(g.probe_offset + 512U, descriptor, g.probe_offset,
                        descriptor, 0U, 0U, 16U / g.pixelbytes, 2U)) return 2U;
    values[0] = SV_WAIT_2D;
    if (send(values, 1U) || wait_idle()) return 2U;
    for (row = 0U; row < 2U; ++row)
        for (i = 0U; i < 4U; ++i) {
            expected = g.bpp == 32U ? 0x00123456UL + i + row * 16U :
                                      0x12345678UL + i + row * 16U;
            observed = SV_READ(g.probe, 512U + row * 128U + i * 4U);
            if ((observed & (g.bpp == 32U ? 0x00ffffffUL : 0xffffffffUL)) != expected)
                bad = 1U;
        }
    for (i = 0U; i < 512U; ++i) SV_WRITE(g.probe, i * 4U, probe_saved[i]);
    if (!bad) ++cvsavage_status.copy_selftests;
    return bad;
}

/* Verify a tiled GPU triangle entirely in private VRAM, before advertising it.
 * A mismatch with a proven idle engine keeps 2D available. A timeout retains
 * ownership and prohibits CPU fallback until the hardware has drained. */
static uint32_t triangle_selftest(void)
{
    uint32_t tile_width = 128U / g.pixelbytes, descriptor;
    uint32_t values[13], expected, outside, interior, i, bad;
    descriptor = SV_BW_DISABLE | (g.bpp << 16) | tile_width;
    outside = g.bpp == 32U ? 0x00112233UL : 0x1122U;
    expected = g.bpp == 32U ? 0x00ff0000UL : 0xf800U;
    for (i = 0U; i < 512U; ++i)
        probe_saved[i] = SV_READ(g.probe, i * 4U);
    if (rectangle(g.scratch_offset, descriptor | SV_TILE_DEST,
                  0U, 0U, tile_width, 16U, outside)) return 2U;
    values[0] = SV_WAIT_2D;
    if (send(values, 1U) || wait_idle() ||
        draw_state(128U, 0U, 0U, tile_width - 1U, 15U)) return 2U;
    values[0] = SV_DRAW_TRIANGLE;
    values[1] = 0x40000000UL; values[2] = 0x40000000UL;
    values[3] = 0x3f000000UL; values[4] = 0xffff0000UL;
    values[5] = g.bpp == 32U ? 0x41e00000UL : 0x42700000UL; /* 28 / 60 */
    values[6] = 0x40000000UL; values[7] = 0x3f000000UL;
    values[8] = 0xffff0000UL;
    values[9] = 0x40000000UL; values[10] = 0x41500000UL; /* y=13 */
    values[11] = 0x3f000000UL; values[12] = 0xffff0000UL;
    if (send(values, 13U)) return 2U;
    values[0] = SV_WAIT_BOTH;
    if (send(values, 1U) || wait_idle() ||
        copy_rectangle(g.probe_offset, descriptor, g.scratch_offset,
                         descriptor | SV_TILE_DEST, 0U, 0U, tile_width, 16U))
        return 2U;
    values[0] = SV_WAIT_BOTH;
    if (send(values, 1U) || wait_idle()) return 2U;
    interior = SV_READ(g.probe, 5U * 128U + (g.bpp == 32U ? 20U : 8U));
    if (g.bpp == 16U) interior = (interior >> 16) & 0xffffU; /* pixel x=5 */
    else interior &= 0x00ffffffUL;
    bad = interior != expected;
    if ((SV_READ(g.probe, 0U) &
         (g.bpp == 32U ? 0x00ffffffUL : 0xffffffffUL)) !=
        (g.bpp == 32U ? outside : outside | (outside << 16))) bad = 1U;
    cvsavage_status.triangle_probe_pixel = interior;
    cvsavage_status.triangle_probe_expected = expected;
    for (i = 0U; i < 512U; ++i)
        SV_WRITE(g.probe, i * 4U, probe_saved[i]);
    if (bad) ++cvsavage_status.triangle_probe_failures;
    else ++cvsavage_status.triangle_selftests;
    return bad;
}

uint32_t cvsavage_bind(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp, uint32_t usable_bytes)
{
    static const uint8_t ram_megabytes[8] = {2U,8U,4U,16U,8U,16U,4U,16U};
    uint32_t i, bus, dev, fn, functions, mmio = 0U, framebuffer = 0U;
    uint32_t aperture = 0U, vram, tile_pitch, scratch_bytes, pixelbytes;
    uint32_t old_stage, triangle_possible, result;
    if (g.saved_valid) return 2U;
    ++cvsavage_status.binds;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    cvsavage_status.error_stage = 1U;
    if ((bpp != 16U && bpp != 32U) || !width || !height ||
        width > 2048U || height > 2048U || !bytes || bytes > 0x01000000UL)
        return 0U;
    pixelbytes = bpp / 8U;
    if (!pitch || (pitch & 15U) || pitch % pixelbytes ||
        pitch / pixelbytes > 4095U || pitch < width * pixelbytes ||
        height > bytes / pitch || physical > 0xffffffffUL - bytes)
        return 0U;
    for (bus = 0U; bus < 256U && !mmio; ++bus) {
        for (dev = 0U; dev < 32U && !mmio; ++dev) {
            if ((pci_read((uint8_t)bus, (uint8_t)dev, 0U, 0U) & 0xffffU) == 0xffffU)
                continue;
            functions = (pci_read((uint8_t)bus, (uint8_t)dev, 0U, 0x0cU) &
                         0x00800000UL) ? 8U : 1U;
            for (fn = 0U; fn < functions; ++fn) {
                if (pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0U) != SV_ID ||
                    ((pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 8U) >> 16) &
                      0xffffU) != 0x0300U ||
                    !(pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 4U) & 2U))
                    continue;
                if (!memory_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0U, &mmio) ||
                    !memory_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 1U, &framebuffer) ||
                    !memory_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 2U, &aperture) ||
                    framebuffer != physical || mmio > 0xffffffffUL - SV_MMIO_BYTES) {
                    mmio = 0U; continue;
                }
                break;
            }
        }
    }
    if (!mmio) return 0U;
    cvsavage_status.device_id = SV_ID;
    cvsavage_status.mmio_physical = mmio;
    cvsavage_status.framebuffer_physical = framebuffer;
    cvsavage_status.aperture_physical = aperture;
    cvsavage_status.error_stage = 2U;
    if (!(cvdev_in(0x3c3U, 1U) & 1U) || !(cvdev_in(0x3ccU, 1U) & 1U))
        return 0U; /* Already active VGA/color decode is required. */
    g.cr38 = cr_read(0x38U); g.cr39 = cr_read(0x39U);
    cr_write(0x38U, 0x48U); cr_write(0x39U, 0xa5U);
    vram = (uint32_t)ram_megabytes[(cr_read(0x36U) & 0x0eU) >> 1] << 20;
    g.cr31 = cr_read(0x31U); g.cr40 = cr_read(0x40U);
    cr_write(0x39U, g.cr39); cr_write(0x38U, g.cr38);
    cvsavage_status.vram_bytes = vram;
    /* Firmware usable VRAM excludes BIOS-owned cursor/COB/reserved memory.
     * Never infer free scratch from a T23 SKU or from the larger CR36 total. */
    if (!usable_bytes || usable_bytes > vram) return 0U;
    vram = usable_bytes;
    /* An additional 64 KiB guard stays within the firmware-usable extent. */
    if (vram <= 65536U + 4096U) return 0U;
    if (bytes > vram - 65536U || physical > 0xffffffffUL - vram) return 0U;
    g.scratch_offset = (bytes + 2047U) & ~2047U;
    if (g.scratch_offset > vram - 65536U - 4096U) return 0U;
    /* Xorg SavageEnableMMIO sets CR40 before its first BAR0 access. The BIOS
     * can leave this bit off even while the linear framebuffer is active. */
    cr_write(0x38U, 0x48U); cr_write(0x39U, 0xa5U);
    cr_write(0x40U, (uint8_t)(g.cr40 | 1U)); g.cr_enabled = 1U;
    g.mmio = cvgpu_map_mmio(mmio, SV_MMIO_BYTES);
    if (!g.mmio) { cvsavage_release(); return 0U; }
    if (wait_idle()) { cvsavage_release(); return 0U; }
    g.bytes = bytes; g.width = width; g.height = height; g.pitch = pitch;
    g.bpp = bpp; g.pixelbytes = pixelbytes;
    g.descriptor = SV_BW_DISABLE | (bpp << 16) | (pitch / pixelbytes);
    tile_pitch = (width * pixelbytes + 127U) & ~127U;
    scratch_bytes = tile_pitch * ((height + 15U) & ~15U);
    g.scratch_bytes = scratch_bytes;
    g.scratch_descriptor = SV_BW_DISABLE | SV_TILE_DEST | (bpp << 16) |
                            (tile_pitch / pixelbytes);
    triangle_possible = scratch_bytes <= vram - 65536U - 4096U - g.scratch_offset &&
                         tile_pitch / 128U <= 127U;
    g.probe_offset = triangle_possible ? g.scratch_offset + scratch_bytes :
                                          g.scratch_offset;
    g.probe = cvgpu_map_mmio(physical + g.probe_offset, 4096U);
    if (!g.probe) { cvsavage_release(); return 0U; }
    cvsavage_status.scratch_offset = g.scratch_offset;
    cvsavage_status.scratch_bytes = scratch_bytes;
    cvsavage_status.pitch = pitch; cvsavage_status.bpp = bpp;
    cvsavage_status.width = width; cvsavage_status.height = height;
    for (i = 0U; i < SV_SAVE_COUNT; ++i)
        g.saved[i] = SV_READ(g.mmio, saved_offsets[i]);
    g.saved_valid = 1U; cvsavage_status.owns_engine = 1U;
    cvsavage_status.error_stage = 3U;
    cr_write(0x38U, 0x48U); cr_write(0x39U, 0xa5U);
    cr_write(0x40U, (uint8_t)(g.cr40 | 1U));
    cr_write(0x31U, (uint8_t)(g.cr31 | 0x0cU));
    /* Enable on-chip PIO only. Shadow DMA and overflow/DMA are disabled. */
    SV_WRITE(g.mmio, SV_CONTROL, g.saved[11] & 0x3ff0U);
    SV_WRITE(g.mmio, 0x48c0cUL, 0U);
    SV_WRITE(g.mmio, SV_CONTROL, (g.saved[11] & 0x3ff0U) | 8U);
    SV_WRITE(g.mmio, SV_PLANE_WRITE, 0xffffffffUL);
    SV_WRITE(g.mmio, 0x812cUL, 0xffffffffUL);
    SV_WRITE(g.mmio, 0x8134UL, 0x00070027UL);
    /* SuperSavage uses bit 0 (64-bit descriptor), NOT Savage4's bit 3. */
    SV_WRITE(g.mmio, SV_GBD_HIGH, g.saved[4] | SV_BW_DISABLE | 1U);
    if ((SV_READ(g.mmio, SV_CONTROL) & 0x0eU) != 8U ||
        !(SV_READ(g.mmio, SV_GBD_HIGH) & 1U) ||
        SV_READ(g.mmio, SV_PLANE_WRITE) != 0xffffffffUL) goto fail;
    cvsavage_status.error_stage = 4U;
    if (selftest()) goto fail;
    cvsavage_status.ready = 1U; cvsavage_status.caps = CVSAVAGE_CAP_FILL;
    cvsavage_status.error_stage = 10U;
    result = copy_selftest();
    if (result == 2U) goto fail;
    if (result == 0U) cvsavage_status.caps |= CVSAVAGE_CAP_COPY;
    if (triangle_possible && (cvsavage_status.caps & CVSAVAGE_CAP_COPY)) {
        cvsavage_status.error_stage = 9U;
        result = triangle_selftest();
        if (result == 2U) goto fail;
        if (result == 0U) cvsavage_status.caps |= CVSAVAGE_CAP_TRIANGLE;
    }
    cvsavage_status.error_stage = 0U;
    return 1U;
fail:
    old_stage = cvsavage_status.error_stage;
    if (cvsavage_release()) return 2U;
    cvsavage_status.error_stage = old_stage;
    return 0U;
}

uint32_t cvsavage_fill(uint32_t offset, uint32_t pixels,
                       uint32_t colour, uint32_t pixelbytes)
{
    uint32_t x, y, values[1];
    if (!g.saved_valid || !cvsavage_status.ready ||
        !(cvsavage_status.caps & CVSAVAGE_CAP_FILL))
        return g.saved_valid ? 2U : 1U;
    if (pixelbytes != g.pixelbytes || !pixels || pixels > 16384U / pixelbytes ||
        offset % pixelbytes || offset >= g.bytes) return 1U;
    y = offset / g.pitch; x = (offset % g.pitch) / pixelbytes;
    if (y >= g.height || x >= g.width || pixels > g.width - x) return 1U;
    cvsavage_status.error_stage = 5U;
    if (wait_idle() || rectangle(0U, g.descriptor, x, y, pixels, 1U, colour))
        return 2U;
    values[0] = SV_WAIT_2D;
    if (send(values, 1U) || wait_idle()) return 2U;
    ++cvsavage_status.fills; cvsavage_status.error_stage = 0U;
    return 0U;
}

uint32_t cvsavage_page_copy(uint32_t source, uint32_t dest, uint32_t rowbytes,
                            uint32_t rows, uint32_t pitch)
{
    uint32_t extent, framebytes, sx, dx, sy, dy, values[1];
    if (!g.saved_valid || !cvsavage_status.ready)
        return g.saved_valid ? 2U : 1U;
    if (!(cvsavage_status.caps & CVSAVAGE_CAP_COPY)) return 1U;
    if (pitch != g.pitch || !rowbytes || !rows || rows > 64U ||
        rowbytes > 16384U / rows || rowbytes % g.pixelbytes ||
        source % g.pixelbytes || dest % g.pixelbytes) return 1U;
    sx = source % pitch; dx = dest % pitch;
    if (sx != dx || sx >= g.width * g.pixelbytes ||
        rowbytes > g.width * g.pixelbytes - sx) return 1U;
    extent = (rows - 1U) * pitch + rowbytes;
    if (extent > g.bytes || source > g.bytes - extent || dest > g.bytes - extent)
        return 1U;
    if ((source <= dest && dest - source < extent) ||
        (source > dest && source - dest < extent)) return 1U;
    framebytes = g.pitch * g.height;
    sy = (source % framebytes) / pitch; dy = (dest % framebytes) / pitch;
    if (rows > g.height - sy || rows > g.height - dy) return 1U;
    cvsavage_status.error_stage = 11U;
    if (wait_idle() || copy_rectangle(dest - dx, g.descriptor,
                         source - sx, g.descriptor, sx / g.pixelbytes,
                         0U, rowbytes / g.pixelbytes, rows)) return 2U;
    values[0] = SV_WAIT_2D;
    if (send(values, 1U) || wait_idle()) return 2U;
    ++cvsavage_status.blits; cvsavage_status.error_stage = 0U;
    return 0U;
}

/* Positive IEEE754 -> floor, entirely integer: ring-0 never touches x87. */
static uint32_t coordinate(uint32_t bits, uint32_t limit,
                           uint32_t *floor_value, uint32_t *ceil_value)
{
    uint32_t exponent, mantissa, value, fraction = 0U, shift;
    if (bits & 0x80000000UL) return 0U;
    exponent = (bits >> 23) & 0xffU;
    if (exponent == 255U) return 0U;
    if (exponent < 127U) {
        value = 0U; fraction = bits != 0U;
    } else {
        exponent -= 127U;
        if (exponent > 11U) return 0U;
        mantissa = (bits & 0x7fffffUL) | 0x800000UL;
        shift = 23U - exponent;
        value = mantissa >> shift;
        fraction = (mantissa & ((1UL << shift) - 1U)) != 0U;
    }
    if (value >= limit || (fraction && value + 1U >= limit)) return 0U;
    *floor_value = value; *ceil_value = value + fraction;
    return 1U;
}

uint32_t cvsavage_triangle(const cvsavage_triangle_packet *packet)
{
    uint32_t i, x0 = 0xffffffffUL, y0 = 0xffffffffUL, x1 = 0U, y1 = 0U;
    uint32_t floor_x, ceil_x, floor_y, ceil_y, width, height, values[13];
    uint32_t tile_pitch;
    if (!g.saved_valid || !cvsavage_status.ready)
        return g.saved_valid ? 2U : 1U;
    if (!(cvsavage_status.caps & CVSAVAGE_CAP_TRIANGLE) || !packet ||
        packet->magic != CVSAVAGE_TRIANGLE_MAGIC || packet->size != 64U ||
        packet->flags || packet->reserved) return 1U;
    for (i = 0U; i < 3U; ++i) {
        if (!coordinate(packet->vertex[i].x, g.width, &floor_x, &ceil_x) ||
            !coordinate(packet->vertex[i].y, g.height, &floor_y, &ceil_y) ||
            packet->vertex[i].z > 0x3f800000UL) return 1U;
        if (floor_x < x0) x0 = floor_x;
        if (floor_y < y0) y0 = floor_y;
        if (ceil_x > x1) x1 = ceil_x;
        if (ceil_y > y1) y1 = ceil_y;
    }
    width = x1 - x0 + 1U; height = y1 - y0 + 1U;
    if (width > SV_MAX_RECT_PIXELS / height) return 1U;
    cvsavage_status.error_stage = 6U;
    if (wait_idle() || copy_rectangle(g.scratch_offset, g.scratch_descriptor,
                         0U, g.descriptor, x0, y0, width, height)) return 2U;
    values[0] = SV_WAIT_2D;
    if (send(values, 1U) || wait_idle()) return 2U;
    ++cvsavage_status.blits;
    tile_pitch = (g.width * g.pixelbytes + 127U) & ~127U;
    if (draw_state(tile_pitch, x0, y0, x1, y1)) return 2U;
    values[0] = SV_DRAW_TRIANGLE;
    for (i = 0U; i < 3U; ++i) {
        values[1U + i * 4U] = packet->vertex[i].x;
        values[2U + i * 4U] = packet->vertex[i].y;
        values[3U + i * 4U] = packet->vertex[i].z;
        values[4U + i * 4U] = packet->vertex[i].argb;
    }
    if (send(values, 13U)) return 2U;
    values[0] = SV_WAIT_BOTH;
    if (send(values, 1U) || wait_idle()) return 2U;
    cvsavage_status.error_stage = 7U;
    if (copy_rectangle(0U, g.descriptor, g.scratch_offset,
                        g.scratch_descriptor, x0, y0, width, height)) return 2U;
    values[0] = SV_WAIT_BOTH;
    if (send(values, 1U) || wait_idle()) return 2U;
    ++cvsavage_status.blits; ++cvsavage_status.triangles;
    cvsavage_status.error_stage = 0U;
    return 0U;
}
