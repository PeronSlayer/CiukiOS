#ifndef CIUKIOS_DISPLAY_PROBE_H
#define CIUKIOS_DISPLAY_PROBE_H

#ifdef DISP_PROBE_HOST_TEST
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
void mem_set(void *dst, int value, int bytes);
#else
#include "app.h"
#endif

#define DISP_MAX_MODES 128
#define DISP_EDID_BYTES 128
#define DISP_NAME_BYTES 14

#define DISP_F_VBE       0x0001
#define DISP_F_CURRENT   0x0002
#define DISP_F_EDID      0x0004
#define DISP_F_PCI       0x0008
#define DISP_F_PCI_MATCH 0x0010
#define DISP_F_TRUNCATED 0x0020

#define DISP_ADAPTER_UNKNOWN 0
#define DISP_ADAPTER_QEMU    1
#define DISP_ADAPTER_VIRTIO  2
#define DISP_ADAPTER_ATI     3
#define DISP_ADAPTER_NVIDIA  4
#define DISP_ADAPTER_INTEL   5

/* Exactly 16 bytes, suitable for the native desktop query's mode table. */
struct disp_mode {
    u16 id, width, height, pitch, bpp, flags;
    u32 frame_bytes;
};

struct disp_monitor {
    char manufacturer[4];
    char name[DISP_NAME_BYTES];
    u16 product, width_cm, height_cm, input_digital;
    u16 preferred_width, preferred_height;
    u32 preferred_millihz;
    u32 serial;
    u16 valid;
};

struct disp_probe {
    u16 flags, vbe_version, video_memory_64k, current_mode, current_mode_flags;
    u16 current_width, current_height, current_pitch, current_bpp;
    u32 current_frame_bytes, framebuffer_phys;
    u16 adapter_kind, pci_vendor, pci_device, pci_bus, pci_devfn;
    u16 pci_display_count, pci_unmatched_count, mode_count, modes_truncated;
    struct disp_monitor monitor;
    struct disp_mode modes[DISP_MAX_MODES];
};

/* Parse and validate one base EDID block. Returns 1 when valid, else 0. */
int disp_probe_parse_edid(const u8 *edid, u16 bytes, struct disp_monitor *out);

/* Probe VBE, current scanout, EDID and matching PCI display adapter. */
int disp_probe_init(struct disp_probe *out);

#endif
