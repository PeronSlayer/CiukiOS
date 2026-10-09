/*
 * Native SuperSavage IX/C SDR (5333:8c2e) BCI/packed-MMIO backend.
 * Protocol researched from X.Org xf86-video-savage 2.4.1, Mesa 7.11.2,
 * Linux v6.2 Savage DRM, S3 register manuals and 9front's Savage driver.
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
#define SV_MISC 0x8144UL
#define SV_MISC_32B 0x0200UL
#define SV_MISC_WRITABLE 0x0bbfUL /* MM8144 bits6/10 are reserved. */
#define SV_MISC_RSF 0x0010UL /* Access half selector, not persistent state. */
#define SV_MISC_PERSISTENT (SV_MISC_WRITABLE & ~SV_MISC_RSF)
#define SV_BW_DISABLE 0x10000000UL
#define SV_TILE_DEST 0x01000000UL
/* A non-stippled solid colour is the Savage SOURCE operand, not PATTERN.
 * X.Org's SavageHelpSolidROP selects copy ROP CC for this command format. */
#define SV_RECT_FILL 0x4bcc8c00UL /* positive XY, SRCCOPY, colour, new PBD */
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
    0x485dcUL, 0x485e0UL, 0x485e4UL, 0x485e8UL, 0x485ecUL,
    0x8124UL, /* SEND_COLOR changes the foreground register too. */
    SV_MISC, /* Restore access width after every DWORD 2D register. */
    0x8120UL, 0x8100UL, 0x8108UL, 0x8148UL /* packed BG, source/dest XY, WH */
};
#define SV_SAVE_COUNT (sizeof(saved_offsets) / sizeof(saved_offsets[0]))
#define SV_SETUP_MASK 0x0c000fffUL
#define SV_2D_MASK 0x0c000fffUL
#define SV_3D_MASK 0x03fff000UL
#define SV_PACKED_MASK 0xf0000000UL
typedef struct {
    uint32_t mmio, probe, bytes, width, height, pitch, bpp, pixelbytes;
    uint32_t descriptor, scratch_descriptor, scratch_offset, scratch_bytes;
    uint32_t probe_offset;
    uint32_t saved[SV_SAVE_COUNT], dirty_mask;
    uint8_t cr38, cr39, cr40, cr31, cr50, cr66;
    uint8_t saved_valid, cr_enabled, gbd_bci, engine_enabled, packed_2d;
} savage_state;
static savage_state g;
static uint32_t probe_saved[512];
static uint8_t panel_bus, panel_dev, panel_fn, panel_cached, panel_scanned;

#ifdef CVSAVAGE_HOST_TEST
/* Each host fixture reset models a fresh module load, including discovery. */
void cvsavage_test_reset_discovery(void)
{
    panel_cached = panel_scanned = 0U;
}
extern uint32_t cvsavage_test_read(uint32_t linear, uint32_t offset);
extern void cvsavage_test_write(uint32_t linear, uint32_t offset, uint32_t value);
extern uint16_t cvsavage_test_read16(uint32_t linear, uint32_t offset);
extern void cvsavage_test_write16(uint32_t linear, uint32_t offset, uint16_t value);
#define SV_READ(base, offset) cvsavage_test_read((base), (offset))
#define SV_WRITE(base, offset, value) cvsavage_test_write((base), (offset), (value))
#define SV_READ16(base, offset) cvsavage_test_read16((base), (offset))
#define SV_WRITE16(base, offset, value) cvsavage_test_write16((base), (offset), (value))
#else
#define SV_READ(base, offset) (*(volatile uint32_t *)((base) + (offset)))
#define SV_WRITE(base, offset, value) (*(volatile uint32_t *)((base) + (offset)) = (value))
#define SV_READ16(base, offset) (*(volatile uint16_t *)((base) + (offset)))
#define SV_WRITE16(base, offset, value) (*(volatile uint16_t *)((base) + (offset)) = (uint16_t)(value))
#endif

/* Section 7 specifies 16-bit reads even for 32-bit 2D registers. A dword
 * read has an undefined upper word; it cannot validate or save a descriptor.
 * BCI/status/3D registers outside this block retain their documented width. */
static uint32_t engine_read(uint32_t offset)
{
    return SV_READ16(g.mmio, offset) |
           ((uint32_t)SV_READ16(g.mmio, offset + 2U) << 16);
}

static uint32_t saved_read(uint32_t index)
{
    /* S3 p142 permits direct packed reads of the legacy write-only indices.
     * Misc is one word; its neighbour 8146 is a different read selector. */
    if (saved_offsets[index] == SV_MISC) return SV_READ16(g.mmio, SV_MISC);
    if (saved_offsets[index] >= 0x8100U && saved_offsets[index] < 0x8180U)
        return engine_read(saved_offsets[index]);
    return SV_READ(g.mmio, saved_offsets[index]);
}

static void saved_write(uint32_t index, uint32_t value)
{
    if (saved_offsets[index] == SV_MISC) {
        /* A word access also works before 32-bit access is enabled. Retain
         * the legacy E index nibble, as the independent SuperSavage driver. */
        SV_WRITE16(g.mmio, SV_MISC, (value & SV_MISC_WRITABLE) | 0xe000U);
    } else if (index == 2U) {
        SV_WRITE16(g.mmio, 0x8134U, value);
        SV_WRITE16(g.mmio, 0x8136U, value >> 16);
    } else SV_WRITE(g.mmio, saved_offsets[index], value);
}

/* The high word identifies a setup predicate; stage 3 remains ABI-compatible.
 * Preserve the complete read value, not only the bits which failed. */
static uint32_t setup_readback(uint32_t id, uint32_t actual,
                               uint32_t mask, uint32_t expected)
{
    if ((actual & mask) == expected) return 1U;
    cvsavage_status.error_stage = (id << 16) | 3U;
    cvsavage_status.last_status = actual;
    return 0U;
}

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

static uint32_t preflight_reject(uint32_t reason, uint32_t detail)
{
    cvsavage_status.error_stage = 1U | reason;
    cvsavage_status.last_status = detail;
    return 0U;
}

/* Preserve the raw BAR for a failed resource predicate without writing a
 * sizing pattern or changing PCI decode. Prefetchable 32-bit memory passes. */
static uint32_t preflight_bar(uint8_t bus, uint8_t dev, uint8_t fn,
                              uint8_t index, uint32_t reason, uint32_t *base)
{
    uint32_t value = pci_read(bus, dev, fn, (uint8_t)(0x10U + 4U * index));
    *base = value == 0xffffffffUL ? 0U : value & 0xfffffff0UL;
    if (value == 0xffffffffUL || (value & 7U) != 0U || *base < 0x100000UL)
        return preflight_reject(reason, value);
    return 1U;
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

/* CR40 only enables MMIO decode. CR66 bit0 separately enables drawing.
 * Preserve all firmware bits and make the transition only while blanked or
 * after observing the start of vertical retrace. No timing/reset is changed. */
static uint32_t engine_switch(uint8_t value, uint32_t stage)
{
    uint32_t i, status = 0U;
    if (cr_read(0x66U) == value) return 0U;
    cvsavage_status.error_stage = stage;
    if (!(sr_read(0x01U) & 0x20U)) {
        for (i = 0U; i < SV_SPINS; ++i) {
            status = cvdev_in(0x3daU, 1U);
            if (!(status & 8U)) break;
        }
        if (i == SV_SPINS) goto failed;
        for (i = 0U; i < SV_SPINS; ++i) {
            status = cvdev_in(0x3daU, 1U);
            if (status & 8U) break;
        }
        if (i == SV_SPINS) goto failed;
    }
    cr_write(0x66U, value);
    status = cr_read(0x66U);
    if (status == value) return 0U;
failed:
    cvsavage_status.last_status = status;
    return 1U;
}

/* X.Org SavageGetPanelInfo's mobile sequencer report. Indexed VGA reads
 * need no MMIO mapping, CR40 enable, clock/timing or lock-state writes. */
uint32_t cvsavage_panel_probe(void)
{
    uint32_t pci_selector, bus, dev, fn, functions, mmio, width, height, i;
    uint8_t first[5], second[5];
    cvsavage_status.panel_flags = 0U;
    cvsavage_status.panel_width = 0U; cvsavage_status.panel_height = 0U;
    /* DISPLAY_INFO is polled during normal UI service. This fixed PCI GPU
     * has no hotplug path: retain absence for this module load instead of
     * rescanning 256 buses with interrupts disabled on every status query.
     * Present devices still get live identity and LCD-register checks. */
    if (panel_scanned && !panel_cached) return 0U;
    pci_selector = cvdev_in(0xcf8U, 4U);
    if (panel_cached && pci_read(panel_bus, panel_dev, panel_fn, 0U) != SV_ID)
        panel_cached = panel_scanned = 0U;
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
        panel_scanned = 1U;
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

/* The upstream Savage framebuffer driver submits GBD E0/E1 twice through
 * BCI. Verify through the documented read width, and restore through the
 * same command path before giving the engine back to the firmware. */
static uint32_t program_gbd(uint32_t low, uint32_t high)
{
    uint32_t values[2], pass;
    for (pass = 0U; pass < 2U; ++pass) {
        values[0] = SV_SET_REG | (1U << 16) | 0xe0U;
        values[1] = low;
        if (send(values, 2U)) return 1U;
        values[0] = SV_SET_REG | (1U << 16) | 0xe1U;
        values[1] = high;
        if (send(values, 2U)) return 1U;
    }
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

/* The packed command is a trigger, not restorable state. PBD/SBD selectors
 * are documented in S3's command format and 9front's exact8C2E backend.
 * Draw only after the preceding work is idle; never substitute CPU pixels. */
static uint32_t packed_rectangle(uint32_t base, uint32_t descriptor,
                                  uint32_t x, uint32_t y, uint32_t width,
                                  uint32_t height, uint32_t colour)
{
    if (wait_idle()) return 1U;
    saved_write(5U, base); saved_write(6U, descriptor);
    SV_WRITE(g.mmio, 0x8124U, colour); SV_WRITE(g.mmio, 0x8120U, colour);
    SV_WRITE16(g.mmio, 0x8134U, 0x27U); SV_WRITE16(g.mmio, 0x8136U, 0x27U);
    SV_WRITE16(g.mmio, 0x8100U, y); SV_WRITE16(g.mmio, 0x8102U, x);
    SV_WRITE16(g.mmio, 0x8148U, height - 1U);
    SV_WRITE16(g.mmio, 0x814aU, width - 1U);
    /* S3 Drawing Command bit0 must always be1 (manual pp137–139). */
    SV_WRITE(g.mmio, 0x8118U, 0x000440b1UL); /* draw fill, DstPBD, +XY */
    return wait_idle();
}

static uint32_t packed_copy(uint32_t dest, uint32_t dest_descriptor,
                             uint32_t source, uint32_t source_descriptor,
                             uint32_t x, uint32_t y, uint32_t width,
                             uint32_t height)
{
    if (wait_idle()) return 1U;
    saved_write(5U, source); saved_write(6U, source_descriptor);
    saved_write(7U, dest); saved_write(8U, dest_descriptor);
    SV_WRITE16(g.mmio, 0x8134U, 0x67U); SV_WRITE16(g.mmio, 0x8136U, 0x67U);
    SV_WRITE16(g.mmio, 0x8100U, y); SV_WRITE16(g.mmio, 0x8102U, x);
    SV_WRITE16(g.mmio, 0x8108U, y); SV_WRITE16(g.mmio, 0x810aU, x);
    SV_WRITE16(g.mmio, 0x8148U, height - 1U);
    SV_WRITE16(g.mmio, 0x814aU, width - 1U);
    SV_WRITE(g.mmio, 0x8118U, 0x0009c0b1UL); /* bit0 required, SrcPBD, DstSBD */
    return wait_idle();
}

uint32_t cvsavage_owned(void)
{
    return g.saved_valid != 0U || g.engine_enabled != 0U;
}

uint32_t cvsavage_sync(void)
{
    if (!cvsavage_owned()) return 0U;
    cvsavage_status.error_stage = 8U;
    if (wait_idle()) return 1U;
    cvsavage_status.error_stage = 0U;
    return 0U;
}

static uint32_t misc_restore_failure(uint32_t reason, uint32_t original_stage,
                                    uint32_t original_status, uint32_t actual)
{
    /* All DWORD restores have been submitted. Retain just the misc/control
     * replay; retry must not send BCI through the disabled interface. */
    g.gbd_bci = 0U;
    g.dirty_mask &= (1UL << 27) | (1UL << 11);
    cvsavage_status.error_stage = CVSAVAGE_RELEASE_DIAG_TAG | (reason << 24) |
        ((g.saved[27] & SV_MISC_WRITABLE) << 12) | 8U;
    cvsavage_status.last_status = actual;
    cvsavage_status.triangle_probe_pixel = original_stage;
    cvsavage_status.triangle_probe_expected = original_status;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    return 1U;
}

uint32_t cvsavage_release(void)
{
    uint32_t i, actual, original_stage = cvsavage_status.error_stage;
    uint32_t original_status = cvsavage_status.last_status;
    if ((original_stage & CVSAVAGE_RELEASE_DIAG_MASK) == CVSAVAGE_RELEASE_DIAG_TAG) {
        /* A second failed retry must retain the first cause, not nest tags. */
        original_stage = cvsavage_status.triangle_probe_pixel;
        original_status = cvsavage_status.triangle_probe_expected;
    }
    if (g.saved_valid) {
        if (wait_idle()) return 1U;
        if (g.gbd_bci) {
            /* Do not abandon a partially submitted descriptor, and do not
             * disable the command interface before its restoration. */
            if (program_gbd(g.saved[3], g.saved[4]) || wait_idle()) {
                cvsavage_status.error_stage = 8U;
                return 1U;
            }
            for (i = 3U; i <= 4U; ++i) {
                uint32_t actual = saved_read(i);
                if (actual != g.saved[i]) {
                    cvsavage_status.error_stage = 8U;
                    cvsavage_status.last_status = actual;
                    return 1U;
                }
            }
        }
        /* Disable our PIO interface before returning original state. */
        if (g.dirty_mask & (1UL << 11))
            SV_WRITE(g.mmio, SV_CONTROL, SV_READ(g.mmio, SV_CONTROL) & 0x3ff0U);
        for (i = 0U; i < SV_SAVE_COUNT; ++i)
            if ((g.dirty_mask & (1UL << i)) && saved_offsets[i] != SV_CONTROL &&
                saved_offsets[i] != SV_MISC &&
                !(g.gbd_bci && (i == 3U || i == 4U))) {
                saved_write(i, g.saved[i]);
            }
        if (g.dirty_mask & (1UL << 27)) {
            /* The RSF half selector is not a stable configuration readback.
             * Complete DWORD restores before changing their access width. */
            if (wait_idle())
                return misc_restore_failure(1U, original_stage, original_status,
                                            cvsavage_status.last_status);
            saved_write(27U, g.saved[27] & ~SV_MISC_RSF);
            if (wait_idle())
                return misc_restore_failure(2U, original_stage, original_status,
                                            cvsavage_status.last_status);
            actual = SV_READ16(g.mmio, SV_MISC);
            if ((actual ^ g.saved[27]) & SV_MISC_PERSISTENT)
                return misc_restore_failure(3U, original_stage, original_status, actual);
        }
        /* Original control last; no commands remain that could use it. */
        if (g.dirty_mask & (1UL << 11)) {
            SV_WRITE(g.mmio, SV_CONTROL, g.saved[11]);
            if (wait_idle())
                return misc_restore_failure(4U, original_stage, original_status,
                                            cvsavage_status.last_status);
        }
        cr_write(0x50U, g.cr50); cr_write(0x31U, g.cr31);
        /* Replay the borrowed access phase after all reads which validate
         * stable state. No subsequent 2D register reads depend on RSF. */
        if (g.dirty_mask & (1UL << 27)) {
            saved_write(27U, g.saved[27]);
            if (wait_idle())
                return misc_restore_failure(5U, original_stage, original_status,
                                            cvsavage_status.last_status);
        }
        /* If retrace later times out, a retry must not submit packets after
         * the original (possibly disabled) BCI control has been restored. */
        g.dirty_mask = 0U;
        g.gbd_bci = 0U;
    }
    if (g.engine_enabled) {
        if (engine_switch(g.cr66, 13U)) return 1U;
        g.engine_enabled = 0U;
    }
    if (g.cr_enabled) {
        cr_write(0x40U, g.cr40);
        cr_write(0x39U, g.cr39); cr_write(0x38U, g.cr38);
        g.cr_enabled = 0U;
    }
    g.saved_valid = 0U;
    if (g.probe) { cvgpu_unmap_mmio(g.probe); g.probe = 0U; }
    if (g.mmio) { cvgpu_unmap_mmio(g.mmio); g.mmio = 0U; }
    g.dirty_mask = 0U;
    g.gbd_bci = 0U;
    g.packed_2d = 0U;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    cvsavage_status.owns_engine = 0U;
    return 0U;
}

static uint32_t fill_readback(uint32_t reason, uint32_t actual, uint32_t mask,
                               uint32_t expected)
{
    if ((actual & mask) == expected) return 0U;
    cvsavage_status.error_stage = CVSAVAGE_FILL_DIAG_TAG | (reason << 24) |
        ((g.saved[27] & SV_MISC_32B) ? CVSAVAGE_FILL_ORIGINAL_32B : 0U) |
        ((uint32_t)cr_read(0x50U) << 16) | ((uint32_t)g.cr50 << 8) | 4U;
    cvsavage_status.last_status = engine_read(0x8124U);
    cvsavage_status.triangle_probe_pixel = actual;
    cvsavage_status.triangle_probe_expected = expected;
    return 1U;
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
    for (i = 0U; i < 4U && !bad; ++i)
        bad = fill_readback(3U + i, SV_READ(g.probe, i * 4U),
                            g.bpp == 32U ? 0x00ffffffUL : 0xffffffffUL, expected);
    /* Only the private pixels decide the fill proof. The generic register
     * specification makes PBD readable, but does not establish SuperSavage
     * inline-NEW CPU readback semantics. Inspect it only after pixels fail,
     * so a successful hardware fill cannot be rejected by this diagnostic. */
    if (bad && !fill_readback(1U, engine_read(0x8170U),
                              0xffffffffUL, g.probe_offset))
        fill_readback(2U, engine_read(0x8174U), 0xffffffffUL, g.descriptor);
    /* Idle has been proved; restore private VRAM even on a mismatch. */
    for (i = 0U; i < 4U; ++i) SV_WRITE(g.probe, i * 4U, saved[i]);
    if (!bad) ++cvsavage_status.selftests;
    return bad;
}

static void packed_failure(uint32_t reason, uint32_t actual, uint32_t expected)
{
    cvsavage_status.error_stage = CVSAVAGE_PACKED_DIAG_TAG | (reason << 24) |
        ((g.saved[27] & SV_MISC_32B) ? CVSAVAGE_FILL_ORIGINAL_32B : 0U) |
        ((uint32_t)cr_read(0x50U) << 16) | ((uint32_t)g.cr50 << 8) | 4U;
    cvsavage_status.last_status = engine_read(0x8124U);
    cvsavage_status.triangle_probe_pixel = actual;
    cvsavage_status.triangle_probe_expected = expected;
}

static void packed_timeout(uint32_t reason)
{
    /* No engine-register read is legal as a diagnostic for pending work.
     * last_status already contains the failed bounded idle poll. */
    cvsavage_status.error_stage = CVSAVAGE_PACKED_DIAG_TAG | (reason << 24) |
        ((g.saved[27] & SV_MISC_32B) ? CVSAVAGE_FILL_ORIGINAL_32B : 0U) |
        ((uint32_t)((g.cr50 & ~0x30U) | 0xc1U |
            (g.bpp == 32U ? 0x30U : 0x10U)) << 16) |
        ((uint32_t)g.cr50 << 8) | 4U;
    cvsavage_status.triangle_probe_pixel = 0U;
    cvsavage_status.triangle_probe_expected = 0U;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
}

/* Try the independent command frontend only after an idle BCI mismatch.
 * Restore private bytes after completion; on timeout retain their mapping
 * and engine ownership because the pending command can still write them. */
static uint32_t packed_selftest(void)
{
    uint32_t saved[4], i, actual, colour, expected, bad = 0U;
    colour = g.bpp == 32U ? 0x00123456UL : 0x5aa5U;
    expected = g.bpp == 32U ? colour : colour | (colour << 16);
    for (i = 28U; i < SV_SAVE_COUNT; ++i) g.saved[i] = saved_read(i);
    g.dirty_mask |= SV_PACKED_MASK;
    /* 9front initializes EA00 for packed drawing: no stale compare or
     * inverted clipping can suppress our private rectangle. Borrow it. */
    saved_write(27U, 0x0a00U);
    if (wait_idle()) { packed_timeout(6U); return 2U; }
    for (i = 0U; i < 4U; ++i) {
        saved[i] = SV_READ(g.probe, i * 4U);
        SV_WRITE(g.probe, i * 4U, 0xa55aa55aUL);
    }
    if (packed_rectangle(g.probe_offset, g.descriptor, 0U, 0U,
                           16U / g.pixelbytes, 1U, colour)) {
        packed_timeout(6U); return 2U;
    }
    for (i = 0U; i < 4U && !bad; ++i) {
        actual = SV_READ(g.probe, i * 4U);
        if ((actual & (g.bpp == 32U ? 0x00ffffffUL : 0xffffffffUL)) != expected) {
            packed_failure(1U + i, actual, expected); bad = 1U;
        }
    }
    for (i = 0U; i < 4U; ++i) SV_WRITE(g.probe, i * 4U, saved[i]);
    if (!bad) { g.packed_2d = 1U; ++cvsavage_status.selftests; }
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
    values[6] = (y0 << 12) | (1U << 11) | x0; values[7] = (y1 << 12) | x1;
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
    if (g.packed_2d) {
        if (packed_copy(g.probe_offset + 512U, descriptor, g.probe_offset,
                         descriptor, 0U, 0U, 16U / g.pixelbytes, 2U)) {
            packed_timeout(7U); return 2U;
        }
    } else {
        if (copy_rectangle(g.probe_offset + 512U, descriptor, g.probe_offset,
                            descriptor, 0U, 0U, 16U / g.pixelbytes, 2U)) return 2U;
        values[0] = SV_WAIT_2D;
        if (send(values, 1U) || wait_idle()) return 2U;
    }
    for (row = 0U; row < 2U; ++row)
        for (i = 0U; i < 4U; ++i) {
            expected = g.bpp == 32U ? 0x00123456UL + i + row * 16U :
                                      0x12345678UL + i + row * 16U;
            observed = SV_READ(g.probe, 512U + row * 128U + i * 4U);
            if ((observed & (g.bpp == 32U ? 0x00ffffffUL : 0xffffffffUL)) != expected)
                if (!bad) {
                    bad = 1U;
                    if (g.packed_2d) packed_failure(5U, observed, expected);
                }
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

uint32_t cvsavage_bind_step(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp, uint32_t usable_bytes,
                       uint32_t step)
{
    static const uint8_t ram_megabytes[8] = {2U,8U,4U,16U,8U,16U,4U,16U};
    uint32_t i, bus, dev, fn, functions, mmio = 0U, framebuffer = 0U;
    uint32_t aperture = 0U, vram, tile_pitch, scratch_bytes, pixelbytes;
    uint32_t old_stage, old_status, old_packed, triangle_possible, result, value, save_mask;
    uint32_t setup_limit = 0U, setup_done = 0U;
    if (cvsavage_owned()) return 2U;
    if (step > 0x100U && step <= 0x100U + CVSAVAGE_SETUP_CHECKS) {
        setup_limit = step - 0x100U; step = 2U;
    }
    if (step > 5U) return 0U;
    ++cvsavage_status.binds;
    cvsavage_status.ready = 0U; cvsavage_status.caps = 0U;
    cvsavage_status.error_stage = 1U;
    cvsavage_status.width = width; cvsavage_status.height = height;
    cvsavage_status.pitch = pitch; cvsavage_status.bpp = bpp;
    cvsavage_status.device_id = 0U; cvsavage_status.mmio_physical = 0U;
    cvsavage_status.framebuffer_physical = 0U; cvsavage_status.aperture_physical = 0U;
    cvsavage_status.vram_bytes = 0U;
    cvsavage_status.scratch_offset = 0U; cvsavage_status.scratch_bytes = 0U;
    if (bpp != 16U && bpp != 32U)
        return preflight_reject(CVSAVAGE_PREFLIGHT_FORMAT, bytes);
    if (!width || !height || width > 2048U || height > 2048U ||
        !bytes || bytes > 0x01000000UL)
        return preflight_reject(CVSAVAGE_PREFLIGHT_GEOMETRY, bytes);
    pixelbytes = bpp / 8U;
    if (!pitch || (pitch & 15U) || pitch % pixelbytes ||
        pitch / pixelbytes > 4095U || pitch < width * pixelbytes)
        return preflight_reject(CVSAVAGE_PREFLIGHT_PITCH, bytes);
    if (height > bytes / pitch || physical > 0xffffffffUL - bytes)
        return preflight_reject(CVSAVAGE_PREFLIGHT_EXTENT, bytes);
    preflight_reject(CVSAVAGE_PREFLIGHT_MISSING, physical);
    for (bus = 0U; bus < 256U && !mmio; ++bus) {
        for (dev = 0U; dev < 32U && !mmio; ++dev) {
            if ((pci_read((uint8_t)bus, (uint8_t)dev, 0U, 0U) & 0xffffU) == 0xffffU)
                continue;
            functions = (pci_read((uint8_t)bus, (uint8_t)dev, 0U, 0x0cU) &
                         0x00800000UL) ? 8U : 1U;
            for (fn = 0U; fn < functions; ++fn) {
                if (pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0U) != SV_ID)
                    continue;
                cvsavage_status.device_id = SV_ID;
                value = pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 8U);
                if (((value >> 16) & 0xffffU) != 0x0300U) {
                    preflight_reject(CVSAVAGE_PREFLIGHT_CLASS, value); continue;
                }
                value = pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 4U);
                if (!(value & 2U)) {
                    preflight_reject(CVSAVAGE_PREFLIGHT_DECODE, value); continue;
                }
                if (!preflight_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0U,
                                    CVSAVAGE_PREFLIGHT_BAR0, &mmio) ||
                    !preflight_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 1U,
                                    CVSAVAGE_PREFLIGHT_BAR1, &framebuffer)) {
                    cvsavage_status.mmio_physical = mmio;
                    cvsavage_status.framebuffer_physical = framebuffer;
                    mmio = 0U; continue;
                }
                /* BAR2 is only the CPU tiled aperture. A staged, post-boot
                 * proof may run BAR0 PIO + BAR1 readback without allocating it.
                 * Automatic bind retains the known-safe boot predicate. */
                value = pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x18U);
                if (!(step && (value == 0U || value == 8U)) &&
                    !preflight_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 2U,
                                    CVSAVAGE_PREFLIGHT_BAR2, &aperture)) {
                    cvsavage_status.mmio_physical = mmio;
                    cvsavage_status.framebuffer_physical = framebuffer;
                    cvsavage_status.aperture_physical = aperture;
                    mmio = 0U; continue;
                }
                cvsavage_status.mmio_physical = mmio;
                cvsavage_status.framebuffer_physical = framebuffer;
                cvsavage_status.aperture_physical = aperture;
                if (framebuffer != physical) {
                    preflight_reject(CVSAVAGE_PREFLIGHT_PHYSICAL, physical);
                    mmio = 0U; continue;
                }
                if (mmio > 0xffffffffUL - SV_MMIO_BYTES) {
                    preflight_reject(CVSAVAGE_PREFLIGHT_MMIO_SPAN, mmio);
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
    cvsavage_status.last_status = 0U;
    if (!(cvdev_in(0x3c3U, 1U) & 1U) || !(cvdev_in(0x3ccU, 1U) & 1U))
        return 0U; /* Already active VGA/color decode is required. */
    g.cr38 = cr_read(0x38U); g.cr39 = cr_read(0x39U);
    cr_write(0x38U, 0x48U); cr_write(0x39U, 0xa5U);
    vram = (uint32_t)ram_megabytes[(cr_read(0x36U) & 0x0eU) >> 1] << 20;
    g.cr31 = cr_read(0x31U); g.cr40 = cr_read(0x40U);
    g.cr50 = cr_read(0x50U);
    g.cr66 = cr_read(0x66U);
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
    if (step == 1U) {
        cvsavage_release(); cvsavage_status.error_stage = 0U; return 1U;
    }
    if (!(g.cr66 & 1U)) {
        /* Own this transition even when readback fails, so rollback restores
         * the original engine state before any CPU framebuffer access. */
        g.engine_enabled = 1U;
        cvsavage_status.owns_engine = 1U;
        if (engine_switch((uint8_t)(g.cr66 | 1U), 12U)) goto fail;
        if (!setup_readback(8U, cr_read(0x66U), 1U, 1U)) goto fail;
        if (wait_idle()) goto fail;
    }
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
    if (step != 2U) {
        g.probe = cvgpu_map_mmio(physical + g.probe_offset, 4096U);
        if (!g.probe) return cvsavage_release() ? 2U : 0U;
    }
    cvsavage_status.scratch_offset = g.scratch_offset;
    cvsavage_status.scratch_bytes = scratch_bytes;
    cvsavage_status.pitch = pitch; cvsavage_status.bpp = bpp;
    cvsavage_status.width = width; cvsavage_status.height = height;
    /* Never read or replay 3D state merely to prepare the 2D engine. */
    save_mask = step == 2U ? SV_SETUP_MASK : SV_2D_MASK;
    g.dirty_mask = 0U;
#define SETUP_CHECK() do { if (++setup_done == setup_limit) goto prepared; } while (0)
#define SETUP_WRITE(index, val) do { \
    g.dirty_mask |= 1UL << (index); saved_write((index), (val)); SETUP_CHECK(); \
} while (0)
    /* MISC includes an access phase. Capture it before reading other engine
     * registers and own its restoration even for a read-only setup prefix. */
    g.saved[27] = saved_read(27U);
    g.saved_valid = 1U; cvsavage_status.owns_engine = 1U;
    g.dirty_mask = 1UL << 27;
    for (i = 0U; i < SV_SAVE_COUNT; ++i) {
        if (!(save_mask & (1UL << i))) continue;
        if (i == 27U) continue;
        g.saved[i] = saved_read(i);
        /* Save all inline-descriptor/colour state as one addition to the
         * existing checkpoint contract; retain the 21-prefix client ABI. */
        if ((i < 5U || i > 8U) && i < 26U) SETUP_CHECK();
    }
    g.saved_valid = 1U; cvsavage_status.owns_engine = 1U;
    cvsavage_status.error_stage = 3U;
    cr_write(0x38U, 0x48U); cr_write(0x39U, 0xa5U);
    cr_write(0x40U, (uint8_t)(g.cr40 | 1U));
    cr_write(0x31U, (uint8_t)((g.cr31 | 0x0cU) & ~1U));
    /* Pixel depth and register access width are independent. S3 MM8144_9
     * and 9front's exact 8C2E setup require explicit DWORD access before
     * masks/descriptors/colour writes. Preserve the other misc controls. */
    g.dirty_mask |= 1UL << 27;
    saved_write(27U, (g.saved[27] | SV_MISC_32B) & ~SV_MISC_RSF);
    if (wait_idle()) goto fail;
    if (!setup_readback(13U, SV_READ16(g.mmio, SV_MISC),
                       SV_MISC_32B, SV_MISC_32B)) goto fail;
    SETUP_CHECK();
    /* X.Org programs masks/mix before enabling BCI, with hi=32/lo=0
     * thresholds (packed value 1) when the overflow buffer is disabled. */
    SETUP_WRITE(0U, 0xffffffffUL);
    SETUP_WRITE(1U, 0xffffffffUL);
    SETUP_WRITE(2U, 0x00070027UL);
    SETUP_WRITE(11U, g.saved[11] & 0x3ff0U);
    SETUP_WRITE(10U, 1U);
    /* No DMA shadow buffer is used here. Control bit 1 already disables
     * updates: leave the firmware pointer at 48c0c untouched. On diskseq64
     * the first write/replay of that pointer was the prefix that hung. */
    if (wait_idle()) goto fail;
    if (!setup_readback(7U, SV_READ(g.mmio, SV_CONTROL), 0x0eU, 0U)) goto fail;
    SETUP_CHECK();
    SETUP_WRITE(11U, (g.saved[11] & 0x3ff0U) | 8U);
    /* SuperSavage: temporary BD64/block-write-disable, select GBD, then
     * install its final address/format, in the upstream order. */
    SETUP_WRITE(4U, SV_BW_DISABLE | 1U);
    /* Full X.Org mode initialization sets the execution pixel length before
     * the SuperSavage GBD setup. A VBE scanout descriptor alone must not
     * leave this independent drawing format at an inherited depth. */
    cr_write(0x50U, (uint8_t)((g.cr50 & ~0x30U) | 0xc1U |
                              (bpp == 32U ? 0x30U : 0x10U)));
    SETUP_CHECK();
    /* Refuse command submission if the interface did not enable. */
    if (wait_idle()) goto fail;
    if (!setup_readback(1U, SV_READ(g.mmio, SV_CONTROL), 0x0eU, 8U)) goto fail;
    g.gbd_bci = 1U;
    g.dirty_mask |= (1UL << 3) | (1UL << 4);
    if (program_gbd(0U, g.descriptor | 1U)) goto fail;
    SETUP_CHECK();
    if (wait_idle()) goto fail;
    /* SuperSavage SavageSetGBD_PM initializes all three descriptors before
     * any drawing packet. An inline NEW destination is not a substitute
     * for this initial PBD/SBD format contract. Preserve its original state. */
    g.dirty_mask |= 0x000001e0UL;
    saved_write(5U, 0U); saved_write(6U, g.descriptor);
    saved_write(7U, 0U); saved_write(8U, g.descriptor);
    SETUP_CHECK();
    if (wait_idle()) goto fail;
    if (!setup_readback(1U, SV_READ(g.mmio, SV_CONTROL), 0x0eU, 8U) ||
        !setup_readback(2U, engine_read(0x8168U), 0xffffffffUL, 0U) ||
        !setup_readback(3U, engine_read(SV_GBD_HIGH),
                         0xffffffffUL, g.descriptor | 1U) ||
        !setup_readback(4U, cr_read(0x50U), 0xf1U,
                         bpp == 32U ? 0xf1U : 0xd1U) ||
        !setup_readback(5U, cr_read(0x31U), 0x0dU, 0x0cU) ||
        !setup_readback(6U, engine_read(SV_PLANE_WRITE),
                         0xffffffffUL, 0xffffffffUL) ||
        !setup_readback(9U, engine_read(0x8170U), 0xffffffffUL, 0U) ||
        !setup_readback(10U, engine_read(0x8174U), 0xffffffffUL, g.descriptor) ||
        !setup_readback(11U, engine_read(0x8178U), 0xffffffffUL, 0U) ||
        !setup_readback(12U, engine_read(0x817cU), 0xffffffffUL, g.descriptor)) goto fail;
    SETUP_CHECK();
#undef SETUP_WRITE
#undef SETUP_CHECK
    if (step == 2U) {
prepared:
        if (cvsavage_release()) return 2U;
        cvsavage_status.error_stage = 0U; return 1U;
    }
    cvsavage_status.error_stage = 4U;
    g.dirty_mask |= 0x040001e0UL; /* PBD/SBD plus SEND_COLOR foreground state. */
    if (selftest()) {
        /* The tag is emitted only after a completed private pixel mismatch.
         * Timeouts never authorize a second frontend or CPU VRAM access. */
        if ((cvsavage_status.error_stage & CVSAVAGE_FILL_DIAG_MASK) !=
                CVSAVAGE_FILL_DIAG_TAG || packed_selftest()) goto fail;
    }
    cvsavage_status.ready = 1U; cvsavage_status.caps = CVSAVAGE_CAP_FILL |
        (g.packed_2d ? CVSAVAGE_CAP_PACKED2D : 0U);
    cvsavage_status.error_stage = 10U;
    result = copy_selftest();
    if (result == 2U) goto fail;
    if (result == 0U) cvsavage_status.caps |= CVSAVAGE_CAP_COPY;
    if (step && result != 0U) goto fail;
    if (step == 3U) {
        if (cvsavage_release()) return 2U;
        cvsavage_status.error_stage = 0U; return 1U;
    }
    if (step == 5U) {
        /* A distinct 2D-only action re-proves both operations. It cannot
         * inherit a triangle capability from any earlier attempt. */
        if (!g.packed_2d || !(cvsavage_status.caps & CVSAVAGE_CAP_COPY)) {
            cvsavage_status.error_stage = 9U; goto fail;
        }
        cvsavage_status.error_stage = 0U; return 1U;
    }
    if (triangle_possible && (cvsavage_status.caps & CVSAVAGE_CAP_COPY)) {
        /* Do not probe 3D registers until the 2D copy proof succeeded and
         * this call will actually alter the 3D state. */
        for (i = 12U; i < SV_SAVE_COUNT; ++i)
            if (SV_3D_MASK & (1UL << i)) g.saved[i] = saved_read(i);
        g.dirty_mask |= SV_3D_MASK;
        cvsavage_status.error_stage = 9U;
        result = triangle_selftest();
        if (result == 2U) goto fail;
        if (result == 0U) cvsavage_status.caps |= CVSAVAGE_CAP_TRIANGLE;
    }
    if (step == 4U && !(cvsavage_status.caps & CVSAVAGE_CAP_TRIANGLE)) {
        cvsavage_status.error_stage = 9U; goto fail;
    }
    cvsavage_status.error_stage = 0U;
    return 1U;
fail:
    old_stage = cvsavage_status.error_stage;
    old_status = cvsavage_status.last_status;
    old_packed = old_stage == 9U && g.packed_2d &&
                  (cvsavage_status.caps & CVSAVAGE_CAP_COPY);
    if (cvsavage_release()) {
        /* A failed rollback leaves live status/ownership, not a register
         * readback. Do not mislabel its idle-poll value as that register. */
        if ((old_stage & 0xffffU) == 3U &&
            (cvsavage_status.error_stage & CVSAVAGE_RELEASE_DIAG_MASK) !=
                CVSAVAGE_RELEASE_DIAG_TAG)
            cvsavage_status.error_stage = 8U;
        return 2U;
    }
    cvsavage_status.error_stage = old_stage;
    /* Only after successful release, retain this informational path hint
     * for the caller's separately authorized 2D-only phase. No ready/caps
     * bit for an operation survives a failed retained-3D qualification. */
    if (old_packed) cvsavage_status.caps = CVSAVAGE_CAP_PACKED2D;
    if (((old_stage & 0xffffU) == 3U && (old_stage >> 16)) || old_stage == 12U ||
        (old_stage & CVSAVAGE_FILL_DIAG_MASK) == CVSAVAGE_FILL_DIAG_TAG ||
        (old_stage & CVSAVAGE_FILL_DIAG_MASK) == CVSAVAGE_PACKED_DIAG_TAG)
        cvsavage_status.last_status = old_status;
    return 0U;
}

uint32_t cvsavage_bind(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp, uint32_t usable_bytes)
{
    return cvsavage_bind_step(physical, bytes, width, height, pitch, bpp,
                              usable_bytes, 0U);
}

uint32_t cvsavage_fill(uint32_t offset, uint32_t pixels,
                       uint32_t colour, uint32_t pixelbytes)
{
    uint32_t x, y, values[1];
    if (!g.saved_valid || !cvsavage_status.ready ||
        !(cvsavage_status.caps & CVSAVAGE_CAP_FILL))
        return cvsavage_owned() ? 2U : 1U;
    if (pixelbytes != g.pixelbytes || !pixels || pixels > 16384U / pixelbytes ||
        offset % pixelbytes || offset >= g.bytes) return 1U;
    y = offset / g.pitch; x = (offset % g.pitch) / pixelbytes;
    if (y >= g.height || x >= g.width || pixels > g.width - x) return 1U;
    cvsavage_status.error_stage = 5U;
    if (g.packed_2d) {
        if (packed_rectangle(0U, g.descriptor, x, y, pixels, 1U, colour)) return 2U;
    } else {
        if (wait_idle() || rectangle(0U, g.descriptor, x, y, pixels, 1U, colour))
            return 2U;
        values[0] = SV_WAIT_2D;
        if (send(values, 1U) || wait_idle()) return 2U;
    }
    ++cvsavage_status.fills; cvsavage_status.error_stage = 0U;
    return 0U;
}

uint32_t cvsavage_page_copy(uint32_t source, uint32_t dest, uint32_t rowbytes,
                            uint32_t rows, uint32_t pitch)
{
    uint32_t extent, framebytes, sx, dx, sy, dy, values[1];
    if (!g.saved_valid || !cvsavage_status.ready)
        return cvsavage_owned() ? 2U : 1U;
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
    if (g.packed_2d) {
        if (packed_copy(dest - dx, g.descriptor, source - sx, g.descriptor,
                          sx / g.pixelbytes, 0U, rowbytes / g.pixelbytes, rows)) return 2U;
    } else {
        if (wait_idle() || copy_rectangle(dest - dx, g.descriptor,
                             source - sx, g.descriptor, sx / g.pixelbytes,
                             0U, rowbytes / g.pixelbytes, rows)) return 2U;
        values[0] = SV_WAIT_2D;
        if (send(values, 1U) || wait_idle()) return 2U;
    }
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
        return cvsavage_owned() ? 2U : 1U;
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
