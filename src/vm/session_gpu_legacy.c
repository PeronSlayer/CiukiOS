/*
 * Firmware-mode scanout-base backend for selected ATI and NVIDIA GPUs.
 * This only changes the displayed framebuffer start address. VBE sets the
 * mode and the CPU continues to draw into the existing VBE framebuffer; this
 * provides no 2D or 3D acceleration.
 *
 * Register/ID references (used only for the narrowly documented offsets):
 * Linux ATI Radeon register definitions, including CRTC_OFFSET 0x224,
 * CRTC_OFFSET_CNTL 0x228 and CRTC_PITCH 0x22c:
 * https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/radeon/radeon_reg.h
 * Linux fbdev ATI Radeon mode code documents CRTC pitch in 8-pixel units:
 * https://cos.googlesource.com/third_party/kernel/+/d27d96ddb1dbeefe5da2e15e83cfcfa9ee27e3cb/drivers/video/fbdev/aty/radeon_base.c
 * QEMU ATI model masks CRTC_OFFSET to an 8-byte-aligned address and lock bit:
 * https://github.com/qemu/qemu/blob/v11.1.1/hw/display/ati.c
 * Linux NVIDIA fbdev writes the byte start to PCRTC+0x800:
 * https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/nvidia/nv_hw.c
 * Envytools documents PCRTC at BAR0+0x600000 on NV4:NV11 and NV20:NV25:
 * https://envytools.readthedocs.io/en/latest/hw/mmio.html
 * Device ID families are from Linux pci_ids.h:
 * https://github.com/torvalds/linux/blob/master/include/linux/pci_ids.h
 *
 * Deliberately conservative: only VGA-class devices on discovered PCI buses,
 * including behind AGP bridges, with an exact
 * framebuffer BAR base and active memory decode are considered. No BAR sizing,
 * mode programming, engine setup, tiling, secondary-head or PCI writes occur.
 * Unsupported/incongruent firmware modes fall back to the BIOS scanout path.
 */
#include <stdint.h>
#include "session_gpu_legacy.h"

extern uint32_t cvdev_in(uint32_t port, uint32_t size);
extern void cvdev_out(uint32_t port, uint32_t value, uint32_t size);
extern uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes);
extern void cvgpu_unmap_mmio(uint32_t linear);

#define PCI_ADDR 0x0cf8U
#define PCI_DATA 0x0cfcU
#define PCI_MEMORY_ENABLE 0x0002U
#define GPU_LIMIT 0x01000000UL
#define QEMU_VENDOR 0x1234U
#define QEMU_VGA_DEVICE 0x1111U
#define VIRTIO_VENDOR 0x1af4U
#define VIRTIO_GPU_DEVICE 0x1050U
#define ATI_VENDOR 0x1002U
#define NVIDIA_VENDOR 0x10deU
#define ATI_CRTC_OFFSET 0x0224U
#define ATI_CRTC_OFFSET_CNTL 0x0228U
#define ATI_CRTC_PITCH 0x022cU
#define ATI_CRTC_GEN_CNTL 0x0050U
#define ATI_CRTC_H_TOTAL_DISP 0x0200U
#define ATI_CRTC_V_TOTAL_DISP 0x0208U
#define ATI_CRTC2_GEN_CNTL 0x03f8U
#define ATI_OFFSET_MASK 0x07fffff8UL
#define ATI_OFFSET_LOCK 0x80000000UL
#define NV_PCRTC_START 0x00000800U

typedef struct {
    uint8_t bus, dev, fn, kind, nv04;
    uint16_t device;
    uint32_t fb_phys, mmio_phys, mmio, bytes;
    uint32_t width, height, pitch, bpp, last_offset, saved_offset;
    uint32_t sig0, sig1, sig2, sig3, sig4, sig5, sig6;
    uint8_t saved;
} cvlegacy_state;

static cvlegacy_state g_legacy;
static uint8_t g_bus_visited[256];
static uint8_t g_bus_queue[256];

enum { LEGACY_NONE = 0, LEGACY_ATI = 1, LEGACY_NVIDIA = 2 };

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t address = 0x80000000UL | ((uint32_t)bus << 16) |
                       ((uint32_t)dev << 11) |
                       ((uint32_t)fn << 8) | ((uint32_t)reg & 0xfcU);
    cvdev_out(PCI_ADDR, address, 4U);
    return cvdev_in(PCI_DATA, 4U);
}

static uint32_t pci_bar(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t index,
                        uint32_t *address)
{
    uint32_t low = pci_read(bus, dev, fn, (uint8_t)(0x10U + index * 4U));
    uint32_t type;
    if ((low & 1U) != 0U) return 0;
    type = (low >> 1) & 3U;
    if (type == 2U) {
        uint32_t high = pci_read(bus, dev, fn, (uint8_t)(0x14U + index * 4U));
        if (high != 0U || index == 5U) return 0;
    } else if (type != 0U) return 0;
    *address = low & 0xfffffff0UL;
    return *address != 0U;
}

/*
 * VBE 3.0 distinguishes the windowed and flat framebuffer paths in Set Mode
 * D14; PhysBasePtr describes the flat framebuffer when available. CiukiOS's
 * banked fallback may still use a BAR alias, but only for the two QEMU devices
 * whose VBE framebuffer BAR is documented:
 * QEMU standard VGA (1234:1111, BAR0) and virtio-vga (1af4:1050, BAR2).
 * SeaBIOS's Bochs-VGA VBE setup selects these BARs by device vendor.
 * VBE 3.0: https://courses.cs.washington.edu/courses/cse451/24wi/documentation/vbe3.pdf
 * QEMU std VGA BAR0: https://chromium.googlesource.com/external/qemu/+/v2.4.0-rc2/docs/specs/standard-vga.txt
 * QEMU virtio-vga VBE BAR selection: https://qemu.googlesource.com/seabios/+/refs/tags/rel-1.8.0/vgasrc/bochsvga.c
 * This routine only reads PCI configuration; it never probes BAR sizing or
 * writes PCI command/status registers (whose upper half is W1C).
 */
uint32_t cvlegacy_vbe_alias(uint32_t physical)
{
    uint8_t bus, dev;
    if (physical == 0U || physical < 0x00100000UL) return 0U;
    for (bus = 0U; bus < 8U; ++bus) {
        for (dev = 0U; dev < 32U; ++dev) {
            uint32_t id = pci_read(bus, dev, 0U, 0U);
            uint32_t header;
            uint8_t functions, fn;
            if ((id & 0xffffU) == 0xffffU) continue;
            header = pci_read(bus, dev, 0U, 0x0cU);
            functions = ((header >> 23) & 1U) ? 8U : 1U;
            for (fn = 0U; fn < functions; ++fn) {
                uint32_t vendor_device = pci_read(bus, dev, fn, 0U);
                uint32_t command, cls;
                uint16_t vendor = (uint16_t)vendor_device;
                uint16_t device = (uint16_t)(vendor_device >> 16);
                uint8_t fb_bar;
                uint32_t fb_base;
                if (!((vendor == QEMU_VENDOR && device == QEMU_VGA_DEVICE) ||
                      (vendor == VIRTIO_VENDOR && device == VIRTIO_GPU_DEVICE)))
                    continue;
                command = pci_read(bus, dev, fn, 4U);
                cls = (pci_read(bus, dev, fn, 0x08U) >> 16) & 0xffffU;
                fb_bar = vendor == QEMU_VENDOR ? 0U : 2U;
                if ((command & PCI_MEMORY_ENABLE) == 0U || cls != 0x0300U ||
                    !pci_bar(bus, dev, fn, fb_bar, &fb_base)) continue;
                if (fb_base == physical) return 1U;
            }
        }
    }
    return 0U;
}

static uint8_t ati_supported(uint16_t id)
{
    if (id >= 0x5041U && id <= 0x5058U) return 1;
    if (id >= 0x5245U && id <= 0x5247U) return 1;
    if (id == 0x524bU || id == 0x524cU) return 1;
    if (id == 0x4c45U || id == 0x4c46U ||
        id == 0x4d46U || id == 0x4d4cU) return 1;
    if (id >= 0x5345U && id <= 0x5347U) return 1;
    if (id == 0x5348U || (id >= 0x534bU && id <= 0x534eU)) return 1;
    if (id == 0x5446U || (id >= 0x544cU && id <= 0x5455U)) return 1;
    return (id >= 0x5144U && id <= 0x5147U) ||
           (id >= 0x514cU && id <= 0x514fU) || id == 0x516cU;
}

static uint8_t nvidia_supported(uint16_t id)
{
    /* RIVA TNT; GeForce 256 / GeForce2; GeForce3 / GeForce4 through NV25. */
    if (id == 0x0020U || id == 0x0100U || id == 0x0101U) return 1;
    if (id >= 0x0110U && id <= 0x0113U) return 1;
    if (id >= 0x0150U && id <= 0x0153U) return 1;
    if (id >= 0x0170U && id <= 0x0173U) return 1;
    if (id >= 0x0181U && id <= 0x0183U) return 1;
    if (id >= 0x0200U && id <= 0x0203U) return 1;
    if (id >= 0x0250U && id <= 0x0253U) return 1;
    return 0;
}

static uint8_t geometry_ok(uint32_t bytes, uint32_t width, uint32_t height,
                           uint32_t pitch, uint32_t bpp)
{
    uint32_t bytespp, row, total;
    if (width == 0U || height == 0U || pitch == 0U || bytes == 0U ||
        bytes > GPU_LIMIT) return 0;
    if (bpp != 8U && bpp != 15U && bpp != 16U && bpp != 24U && bpp != 32U)
        return 0;
    bytespp = (bpp + 7U) >> 3;
    if (width > 0xffffffffUL / bytespp) return 0;
    row = width * bytespp;
    if (pitch < row || height > 0xffffffffUL / pitch) return 0;
    total = pitch * height;
    return total <= bytes;
}

static uint32_t reg_read(volatile uint32_t *base, uint32_t offset)
{
    return base[offset >> 2];
}

static void reg_write(volatile uint32_t *base, uint32_t offset, uint32_t value)
{
    base[offset >> 2] = value;
}

static uint8_t ati_pitch_matches(uint32_t pitch_reg, uint32_t pitch,
                                 uint32_t bpp)
{
    uint32_t bytespp = (bpp + 7U) >> 3;
    uint32_t pixels;
    if ((pitch % bytespp) != 0U) return 0;
    pixels = pitch / bytespp;
    if ((pixels & 7U) != 0U || pixels > 0x7ff8U) return 0;
    return (pitch_reg & 0x7ffU) == (pixels >> 3);
}

static uint8_t ati_mode_matches(volatile uint32_t *r, uint32_t width,
                                uint32_t height, uint32_t pitch,
                                uint32_t bpp)
{
    uint32_t h = reg_read(r, ATI_CRTC_H_TOTAL_DISP);
    uint32_t v = reg_read(r, ATI_CRTC_V_TOTAL_DISP);
    uint32_t offcntl = reg_read(r, ATI_CRTC_OFFSET_CNTL);
    uint32_t pitch_reg = reg_read(r, ATI_CRTC_PITCH);
    uint32_t gen = reg_read(r, ATI_CRTC_GEN_CNTL);
    if ((gen & ((1UL << 24) | (1UL << 25))) !=
        ((1UL << 24) | (1UL << 25))) return 0;
    if (((((h >> 16) & 0x1ffU) + 1U) * 8U) != width) return 0;
    if ((((v >> 16) & 0x7ffU) + 1U) != height) return 0;
    if ((offcntl & ((1UL << 16) | (1UL << 15) | (1UL << 14) |
                    (1UL << 13) | (1UL << 9))) != 0U)
        return 0;
    return ati_pitch_matches(pitch_reg, pitch, bpp);
}

static uint8_t nv_crtc_read(uint8_t index)
{
    uint8_t old_index = (uint8_t)cvdev_in(0x03d4U, 1U);
    uint8_t value;
    cvdev_out(0x03d4U, index, 1U);
    value = (uint8_t)cvdev_in(0x03d5U, 1U);
    cvdev_out(0x03d4U, old_index, 1U);
    return value;
}

static uint8_t nv_mode_matches(uint32_t width, uint32_t height,
                               uint32_t pitch, uint32_t bpp,
                               const cvlegacy_state *s)
{
    uint8_t hdisp = nv_crtc_read(0x01U);
    uint8_t vdisp = nv_crtc_read(0x12U);
    uint8_t voverflow = nv_crtc_read(0x07U);
    uint8_t doublescan = nv_crtc_read(0x09U);
    uint8_t extv = nv_crtc_read(0x25U);
    uint8_t exth = nv_crtc_read(0x2dU);
    uint8_t format = nv_crtc_read(0x28U);
    uint8_t pitchreg = nv_crtc_read(0x13U);
    uint8_t pitchhi = nv_crtc_read(0x19U);
    uint8_t head = nv_crtc_read(0x44U);
    uint32_t actual_width = (((uint32_t)hdisp | ((uint32_t)(exth & 1U) << 8)) + 1U) * 8U;
    uint32_t actual_height = (uint32_t)vdisp |
        ((uint32_t)(voverflow & 0x02U) << 7) |
        ((uint32_t)(voverflow & 0x40U) << 3) |
        ((uint32_t)(extv & 0x02U) << 9);
    uint32_t expected_format, actual_pitch;
    actual_height += 1U;
    if (bpp == 8U) expected_format = 1U;
    else if (bpp == 16U) expected_format = 2U;
    else if (bpp == 32U) expected_format = 3U;
    else return 0;
    actual_pitch = ((uint32_t)pitchreg | ((uint32_t)(pitchhi & 0xe0U) << 3)) * 8U;
    if (actual_height > 2048U || actual_width != width ||
        actual_height != height || (doublescan & 0x80U) != 0U ||
        (format & 3U) != expected_format || actual_pitch != pitch ||
        (head & 3U) != 0U || (s->nv04 && (exth & 0x40U) != 0U))
        return 0;
    return (uint8_t)(hdisp == s->sig0 && vdisp == s->sig1 &&
                     pitchreg == s->sig2 && format == s->sig3 &&
                     (uint32_t)extv == s->sig4 &&
                     (uint32_t)head == s->sig5 &&
                     (uint32_t)pitchhi == s->sig6);
}

static uint8_t find_candidate(uint8_t *dev_out, uint8_t *fn_out,
                              uint8_t *bus_out, uint8_t *kind_out,
                              uint16_t *id_out)
{
    uint16_t qhead = 0U, qtail = 1U;
    g_bus_queue[0] = 0U;
    for (qhead = 0U; qhead < qtail; ++qhead) {
        uint8_t bus = g_bus_queue[qhead];
        uint8_t dev;
        g_bus_visited[bus] = 1U;
        for (dev = 0; dev < 32U; ++dev) {
            uint32_t id = pci_read(bus, dev, 0, 0);
            uint32_t header, cls;
            uint8_t functions, i;
            if ((id & 0xffffU) == 0xffffU) continue;
            header = pci_read(bus, dev, 0, 0x0cU);
            functions = ((header >> 23) & 1U) ? 8U : 1U;
            for (i = 0; i < functions; ++i) {
                uint32_t vendor_device = pci_read(bus, dev, i, 0);
                uint16_t vendor = (uint16_t)vendor_device;
                uint16_t device = (uint16_t)(vendor_device >> 16);
                cls = (pci_read(bus, dev, i, 0x08U) >> 16) & 0xffffU;
                if (cls == 0x0604U) {
                    uint32_t buses = pci_read(bus, dev, i, 0x18U);
                    uint8_t secondary = (uint8_t)(buses >> 8);
                    uint8_t subordinate = (uint8_t)(buses >> 16);
                    if (secondary != 0U && secondary <= subordinate &&
                        !g_bus_visited[secondary] && qtail < 256U) {
                        g_bus_queue[qtail++] = secondary;
                        g_bus_visited[secondary] = 1U;
                    }
                }
                if (cls != 0x0300U) continue;
                if (vendor == ATI_VENDOR && ati_supported(device)) {
                    *bus_out = bus; *dev_out = dev; *fn_out = i;
                    *kind_out = LEGACY_ATI; *id_out = device; return 1;
                }
                if (vendor == NVIDIA_VENDOR && nvidia_supported(device)) {
                    *bus_out = bus; *dev_out = dev; *fn_out = i;
                    *kind_out = LEGACY_NVIDIA; *id_out = device; return 1;
                }
            }
        }
    }
    return 0;
}

uint32_t cvlegacy_bind(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp)
{
    cvlegacy_state s;
    uint32_t command, bar_fb, bar_mmio, mmio_map_phys, fb_bar_index;
    uint8_t kind;
    if (g_legacy.saved) return 0;
    if (!geometry_ok(bytes, width, height, pitch, bpp)) return 0;
    for (command = 0U; command < 256U; ++command) g_bus_visited[command] = 0U;
    if (!find_candidate(&s.dev, &s.fn, &s.bus, &kind, &s.device)) return 0;
    command = pci_read(s.bus, s.dev, s.fn, 4U);
    if ((command & PCI_MEMORY_ENABLE) == 0U) return 0;
    s.kind = kind;
    s.nv04 = (uint8_t)(kind == LEGACY_NVIDIA && s.device == 0x0020U);
    fb_bar_index = kind == LEGACY_ATI ? 0U : 1U;
    if (!pci_bar(s.bus, s.dev, s.fn, (uint8_t)fb_bar_index, &bar_fb) ||
        !pci_bar(s.bus, s.dev, s.fn, (uint8_t)(kind == LEGACY_ATI ? 2U : 0U),
                 &bar_mmio)) return 0;
    if (physical != bar_fb || physical < 0x00100000UL) return 0;
    s.fb_phys = bar_fb; s.mmio_phys = bar_mmio; s.bytes = bytes;
    mmio_map_phys = bar_mmio;
    if (kind == LEGACY_NVIDIA) {
        if (bar_mmio > 0xffffffffUL - 0x600000UL) return 0;
        mmio_map_phys += 0x600000UL;
    }
    s.mmio = cvgpu_map_mmio(mmio_map_phys, 0x1000UL);
    if (s.mmio == 0U) return 0;
    s.width = width; s.height = height; s.pitch = pitch; s.bpp = bpp;
    if (kind == LEGACY_ATI) {
        volatile uint32_t *r = (volatile uint32_t *)s.mmio;
        uint32_t secondary = 0U;
        if (s.device >= 0x5144U && s.device <= 0x514fU)
            secondary = reg_read(r, ATI_CRTC2_GEN_CNTL);
        if ((secondary & (1UL << 25)) != 0U ||
            !ati_mode_matches(r, width, height, pitch, bpp)) {
            cvgpu_unmap_mmio(s.mmio); return 0;
        }
        s.sig0 = reg_read(r, ATI_CRTC_H_TOTAL_DISP);
        s.sig1 = reg_read(r, ATI_CRTC_V_TOTAL_DISP);
        s.sig2 = reg_read(r, ATI_CRTC_PITCH);
        s.sig3 = reg_read(r, ATI_CRTC_OFFSET_CNTL);
        s.sig4 = reg_read(r, ATI_CRTC_GEN_CNTL);
        s.saved_offset = reg_read(r, ATI_CRTC_OFFSET) & ATI_OFFSET_MASK;
        if (s.saved_offset > bytes - pitch * height) {
            cvgpu_unmap_mmio(s.mmio); return 0;
        }
    } else {
        volatile uint32_t *r = (volatile uint32_t *)s.mmio;
        uint32_t start = reg_read(r, NV_PCRTC_START);
        if (start >= 0x01000000UL || (s.nv04 && (start & 0x01000000UL))) {
            cvgpu_unmap_mmio(s.mmio); return 0;
        }
        s.saved_offset = start;
        s.sig0 = nv_crtc_read(0x01U);
        s.sig1 = nv_crtc_read(0x12U);
        s.sig2 = nv_crtc_read(0x13U);
        s.sig3 = nv_crtc_read(0x28U);
        s.sig4 = nv_crtc_read(0x25U);
        s.sig5 = nv_crtc_read(0x44U);
        s.sig6 = nv_crtc_read(0x19U);
        if (!nv_mode_matches(width, height, pitch, bpp, &s)) {
            cvgpu_unmap_mmio(s.mmio); return 0;
        }
    }
    s.last_offset = s.saved_offset;
    s.saved = 1U;
    g_legacy = s;
    return 1U;
}

uint32_t cvlegacy_present(uint32_t yoffset)
{
    uint32_t offset;
    if (!g_legacy.saved || yoffset > 0xffffffffUL / g_legacy.pitch) return 1U;
    offset = yoffset * g_legacy.pitch;
    if (yoffset > 0xffffffffUL - g_legacy.height ||
        (yoffset + g_legacy.height) > g_legacy.bytes / g_legacy.pitch ||
        offset > g_legacy.bytes - g_legacy.pitch * g_legacy.height)
        return 1U;
    if (g_legacy.kind == LEGACY_ATI) {
        volatile uint32_t *r = (volatile uint32_t *)g_legacy.mmio;
        uint32_t current = reg_read(r, ATI_CRTC_OFFSET);
        uint32_t value;
        if ((current & ATI_OFFSET_MASK) != g_legacy.last_offset ||
            !ati_mode_matches(r, g_legacy.width, g_legacy.height,
                              g_legacy.pitch, g_legacy.bpp)) return 1U;
        value = (current & ATI_OFFSET_LOCK) | (offset & ATI_OFFSET_MASK);
        reg_write(r, ATI_CRTC_OFFSET, value);
        if ((reg_read(r, ATI_CRTC_OFFSET) & ATI_OFFSET_MASK) !=
            (offset & ATI_OFFSET_MASK)) return 1U;
    } else {
        volatile uint32_t *r = (volatile uint32_t *)g_legacy.mmio;
        uint32_t current = reg_read(r, NV_PCRTC_START);
        if (current != g_legacy.last_offset || offset >= 0x01000000UL ||
            (g_legacy.nv04 && (offset & 0x01000000UL)) ||
            !nv_mode_matches(g_legacy.width, g_legacy.height,
                             g_legacy.pitch, g_legacy.bpp, &g_legacy)) return 1U;
        reg_write(r, NV_PCRTC_START, offset);
        if (reg_read(r, NV_PCRTC_START) != offset) return 1U;
    }
    g_legacy.last_offset = offset;
    return 0U;
}

uint32_t cvlegacy_release(void)
{
    uint8_t restore = 0;
    if (!g_legacy.saved) return 0U;
    if (g_legacy.kind == LEGACY_ATI) {
        volatile uint32_t *r = (volatile uint32_t *)g_legacy.mmio;
        restore = (uint8_t)(
            (reg_read(r, ATI_CRTC_OFFSET) & ATI_OFFSET_MASK) ==
                g_legacy.last_offset &&
            reg_read(r, ATI_CRTC_H_TOTAL_DISP) == g_legacy.sig0 &&
            reg_read(r, ATI_CRTC_V_TOTAL_DISP) == g_legacy.sig1 &&
            reg_read(r, ATI_CRTC_PITCH) == g_legacy.sig2 &&
            reg_read(r, ATI_CRTC_OFFSET_CNTL) == g_legacy.sig3 &&
            reg_read(r, ATI_CRTC_GEN_CNTL) == g_legacy.sig4);
        if (restore) {
            uint32_t current = reg_read(r, ATI_CRTC_OFFSET);
            reg_write(r, ATI_CRTC_OFFSET,
                      (current & ATI_OFFSET_LOCK) | g_legacy.saved_offset);
            (void)reg_read(r, ATI_CRTC_OFFSET);
        }
    } else {
        volatile uint32_t *r = (volatile uint32_t *)g_legacy.mmio;
        restore = (uint8_t)(reg_read(r, NV_PCRTC_START) == g_legacy.last_offset &&
                            nv_mode_matches(g_legacy.width, g_legacy.height,
                                            g_legacy.pitch, g_legacy.bpp,
                                            &g_legacy));
        if (restore) {
            reg_write(r, NV_PCRTC_START, g_legacy.saved_offset);
            (void)reg_read(r, NV_PCRTC_START);
        }
    }
    cvgpu_unmap_mmio(g_legacy.mmio);
    g_legacy.saved = 0U;
    g_legacy.mmio = 0U;
    return 0U;
}
