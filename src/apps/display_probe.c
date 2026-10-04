/* Firmware display discovery for DISPLAY.APP. This only reads the active
 * firmware configuration; it never changes the video mode. */
#include "display_probe.h"

#ifndef DISP_PROBE_HOST_TEST
#define VBE_OK 0x004F
#define MAX_FW_MODES 256
#define VBE_MODE_INFO_BYTES 256
static u8 vbe_info[512];
static u8 mode_info[VBE_MODE_INFO_BYTES];
static u8 edid_block[DISP_EDID_BYTES];
static u16 fw_modes[MAX_FW_MODES];
static u16 fw_mode_count;
#endif

static u16 rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
#ifndef DISP_PROBE_HOST_TEST
static int bios_call(struct regs *r)
{
    r->ds = app_seg();
    intr(0x10, r);
    return r->ax == VBE_OK;
}
static int pci_word(u16 bus, u16 devfn, u16 reg, u16 *value)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0xB109;
    r.bx = (u16)((bus << 8) | devfn);
    r.di = reg;
    r.ds = r.es = app_seg();
    if (intr(0x1A, &r) || (r.ax & 0xFF00)) return 0;
    *value = r.cx;
    return 1;
}
static u32 pci_dword(u16 bus, u16 devfn, u16 reg, int *ok)
{
    u16 lo, hi;
    if (!pci_word(bus, devfn, reg, &lo) || !pci_word(bus, devfn, (u16)(reg + 2), &hi)) {
        *ok = 0; return 0;
    }
    return (u32)lo | ((u32)hi << 16);
}
#endif

int disp_probe_parse_edid(const u8 *edid, u16 bytes, struct disp_monitor *out)
{
    static const u8 header[8] = {0, 255, 255, 255, 255, 255, 255, 0};
    u16 sum = 0, mfg, i, dtd, clock, ha, hb, va, vb;
    u32 pixels, rem, mhz;
    if (!out) return 0;
    mem_set(out, 0, sizeof *out);
    if (!edid || bytes < DISP_EDID_BYTES || edid[18] != 1) return 0;
    for (i = 0; i < 8; ++i) if (edid[i] != header[i]) return 0;
    for (i = 0; i < DISP_EDID_BYTES; ++i) sum = (u16)(sum + edid[i]);
    if ((sum & 255) != 0) return 0;

    mfg = (u16)(((u16)edid[8] << 8) | edid[9]);
    for (i = 0; i < 3; ++i) {
        u16 c = (u16)((mfg >> (10 - 5 * i)) & 31);
        if (c < 1 || c > 26) return 0;
        out->manufacturer[i] = (char)('A' + c - 1);
    }
    out->manufacturer[3] = 0;
    out->product = rd16(edid + 10);
    out->serial = rd32(edid + 12);
    out->width_cm = edid[21];
    out->height_cm = edid[22];
    out->input_digital = (u16)((edid[20] >> 7) & 1);

    for (dtd = 54; dtd + 18 <= 126; dtd += 18) {
        int is_name = edid[dtd] == 0 && edid[dtd + 1] == 0 &&
                      edid[dtd + 2] == 0 && edid[dtd + 3] == 0xFC;
        if (is_name && !out->name[0]) {
            int k;
            for (k = 0; k < 13; ++k) {
                u8 c = edid[dtd + 5 + k];
                if (c == 0 || c == '\n' || c == '\r') break;
                out->name[k] = (c >= 32 && c < 127) ? (char)c : ' ';
            }
            while (k > 0 && out->name[k - 1] == ' ') --k;
            out->name[k] = 0;
        }
    }

    /* The base block's first detailed timing is the EDID preferred timing. */
    dtd = 54;
    clock = rd16(edid + dtd);
    if (clock && (edid[24] & 2) && !(edid[dtd + 17] & 0x80)) {
        ha = (u16)(edid[dtd + 2] | ((u16)(edid[dtd + 4] & 0xF0) << 4));
        hb = (u16)(edid[dtd + 3] | ((u16)(edid[dtd + 4] & 0x0F) << 8));
        va = (u16)(edid[dtd + 5] | ((u16)(edid[dtd + 7] & 0xF0) << 4));
        vb = (u16)(edid[dtd + 6] | ((u16)(edid[dtd + 7] & 0x0F) << 8));
        pixels = (u32)(ha + hb) * (u32)(va + vb);
        if (ha && va && pixels) {
            out->preferred_width = ha;
            out->preferred_height = va;
            /* clock is in 10 kHz units. Decimal long division avoids a
             * 64-bit multiply on the 16-bit OpenWatcom runtime. */
            mhz = clock / pixels;
            rem = clock % pixels;
            for (i = 0; i < 7; ++i) {
                rem *= 10;
                if (mhz > (0xFFFFFFFFUL - rem / pixels) / 10) { mhz = 0; break; }
                mhz = mhz * 10 + rem / pixels;
                rem %= pixels;
            }
            out->preferred_millihz = mhz;
        }
    }
    out->valid = 1;
    return 1;
}

#ifndef DISP_PROBE_HOST_TEST
static int get_mode_info(u16 id)
{
    struct regs r;
    mem_set(mode_info, 0, sizeof mode_info);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x4F01; r.cx = id;
    r.es = app_seg(); r.di = (u16)mode_info;
    return bios_call(&r);
}

static int add_mode(struct disp_probe *p, u16 id, int current)
{
    u16 attrs, width, height, pitch, bpp, frame_bpp, bytes_per_pixel;
    u32 frame;
    struct disp_mode *m;
    if (!get_mode_info(id)) return 0;
    attrs = rd16(mode_info);
    if (!(attrs & 1) || !(attrs & 0x10) || mode_info[24] != 1 ||
        (mode_info[27] != 4 && mode_info[27] != 6)) return 0;
    width = rd16(mode_info + 18); height = rd16(mode_info + 20);
    bpp = mode_info[25];
    if (width < 640 || width > 2560 || height < 480 || height > 1600 ||
        (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)) return 0;
    if (current && (p->current_mode_flags & 0x4000)) {
        if (!(attrs & 0x80)) return 0;
        pitch = (p->vbe_version >= 0x0300 && rd16(mode_info + 50)) ?
                rd16(mode_info + 50) : rd16(mode_info + 16);
    } else if ((attrs & 0x80) && p->vbe_version >= 0x0300 && rd16(mode_info + 50))
        pitch = rd16(mode_info + 50);
    else pitch = rd16(mode_info + 16);
    if (!pitch || pitch > 32767 || pitch < (u16)(((u32)width * bpp + 7) / 8)) return 0;
    bytes_per_pixel = (u16)((bpp + 7) / 8);
    frame_bpp = (u16)(width * bytes_per_pixel);
    if (pitch < frame_bpp) return 0;
    frame = (u32)pitch * height;
    if (frame > 0x00800000UL || frame < frame_bpp) return 0;
    if (p->video_memory_64k && p->video_memory_64k < (u16)((frame + 32767) / 32768)) return 0;

    if (current) {
        p->current_width = width; p->current_height = height;
        p->current_pitch = pitch; p->current_bpp = bpp;
        p->current_frame_bytes = frame;
        p->framebuffer_phys = rd32(mode_info + 40);
        p->flags |= DISP_F_CURRENT;
    }
    {
        u16 i;
        for (i = 0; i < p->mode_count; ++i) if (p->modes[i].id == id) return 1;
    }
    if (p->mode_count >= DISP_MAX_MODES) { p->modes_truncated = 1; p->flags |= DISP_F_TRUNCATED; return 0; }
    m = &p->modes[p->mode_count++];
    m->id = id; m->width = width; m->height = height; m->pitch = pitch;
    m->bpp = bpp; m->flags = (u16)((attrs & 0x80) ? 1 : 0); m->frame_bytes = frame;
    return 1;
}

static void probe_edid(struct disp_probe *p)
{
    struct regs r;
    mem_set(&r, 0, sizeof r); r.ax = 0x4F15; r.bx = 0;
    r.es = 0; r.di = 0;
    if (!bios_call(&r) || !(r.bx & 2)) return;
    mem_set(edid_block, 0, sizeof edid_block);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x4F15; r.bx = 1; r.cx = 0;
    r.es = app_seg(); r.di = (u16)edid_block;
    if (bios_call(&r) && disp_probe_parse_edid(edid_block, sizeof edid_block, &p->monitor))
        p->flags |= DISP_F_EDID;
}

static void classify_adapter(struct disp_probe *p)
{
    if (p->pci_vendor == 0x1234 && p->pci_device == 0x1111) p->adapter_kind = DISP_ADAPTER_QEMU;
    else if (p->pci_vendor == 0x1AF4 && p->pci_device == 0x1050) p->adapter_kind = DISP_ADAPTER_VIRTIO;
    else if (p->pci_vendor == 0x1002) p->adapter_kind = DISP_ADAPTER_ATI;
    else if (p->pci_vendor == 0x10DE) p->adapter_kind = DISP_ADAPTER_NVIDIA;
    else if (p->pci_vendor == 0x8086) p->adapter_kind = DISP_ADAPTER_INTEL;
    else p->adapter_kind = DISP_ADAPTER_UNKNOWN;
}

static void probe_pci(struct disp_probe *p)
{
    struct regs r;
    u16 max_bus, bus, dev, fn;
    int found = 0;
    mem_set(&r, 0, sizeof r); r.ax = 0xB101; r.ds = r.es = app_seg();
    if (intr(0x1A, &r) || (r.ax & 0xFF00)) return;
    max_bus = (u16)(r.cx & 0xFF);
    if (max_bus > 7) { max_bus = 7; p->flags |= DISP_F_TRUNCATED; }
    for (bus = 0; bus <= max_bus; ++bus) for (dev = 0; dev < 32; ++dev) {
        u16 header = 0;
        int fnmax = 1;
        for (fn = 0; fn < 8; ++fn) {
            u16 devfn = (u16)((dev << 3) | fn), vendor, device, cls;
            u32 bar;
            int ok = 1, matched = 0, bi;
            if (fn >= fnmax) break;
            if (!pci_word(bus, devfn, 0, &vendor) || vendor == 0xFFFF) {
                if (!fn) break;
                continue;
            }
            if (!fn && pci_word(bus, devfn, 0x0E, &header) && (header & 0x80)) fnmax = 8;
            if (!pci_word(bus, devfn, 2, &device) || !pci_word(bus, devfn, 0x0A, &cls) || (cls >> 8) != 3) continue;
            ++p->pci_display_count;
            for (bi = 0; bi < 6; ++bi) {
                u16 reg = (u16)(0x10 + bi * 4), low;
                if (!pci_word(bus, devfn, reg, &low)) continue;
                if (low & 1) continue;
                bar = pci_dword(bus, devfn, reg, &ok);
                if (!ok) continue;
                if ((low & 6) == 4 && bi < 5) {
                    u32 high = pci_dword(bus, devfn, (u16)(reg + 4), &ok);
                    ++bi;
                    if (!ok || high) continue;
                }
                if ((bar & ~0xFUL) == (p->framebuffer_phys & ~0xFUL) && p->framebuffer_phys) matched = 1;
            }
            if (!matched) ++p->pci_unmatched_count;
            if (matched && !found) {
                p->pci_vendor = vendor; p->pci_device = device; p->pci_bus = bus; p->pci_devfn = devfn;
                found = 1;
            }
        }
    }
    if (p->pci_display_count) p->flags |= DISP_F_PCI;
    if (found) p->flags |= DISP_F_PCI_MATCH;
    if (found) classify_adapter(p);
}

int disp_probe_init(struct disp_probe *out)
{
    struct regs r;
    u16 list_seg = 0, list_off = 0, id, n, current_id;
    int have_vbe = 0;
    if (!out) return 0;
    mem_set(out, 0, sizeof *out);
    mem_set(vbe_info, 0, sizeof vbe_info);
    vbe_info[0] = 'V'; vbe_info[1] = 'B'; vbe_info[2] = 'E'; vbe_info[3] = '2';
    mem_set(&r, 0, sizeof r); r.ax = 0x4F00; r.es = app_seg(); r.di = (u16)vbe_info;
    if (bios_call(&r) && vbe_info[0] == 'V' && vbe_info[1] == 'E' && vbe_info[2] == 'S' && vbe_info[3] == 'A') {
        out->flags |= DISP_F_VBE;
        out->vbe_version = rd16(vbe_info + 4);
        out->video_memory_64k = rd16(vbe_info + 18);
        list_off = rd16(vbe_info + 14); list_seg = rd16(vbe_info + 16);
        fw_mode_count = 0;
        if (list_seg || list_off) {
            u32 linear = ((u32)list_seg << 4) + list_off;
            for (n = 0; n < MAX_FW_MODES && linear + 2 <= 0x00100000UL; ++n) {
                id = peek16((u16)(linear >> 4), (u16)(linear & 15));
                linear += 2;
                if (id == 0xFFFF) break;
                fw_modes[fw_mode_count++] = id;
            }
            if (n == MAX_FW_MODES || linear + 2 > 0x00100000UL) {
                out->modes_truncated = 1; out->flags |= DISP_F_TRUNCATED;
            }
        }
        have_vbe = 1;
    }
    if (have_vbe) {
        mem_set(&r, 0, sizeof r); r.ax = 0x4F03;
        if (bios_call(&r)) {
            out->current_mode_flags = (u16)(r.bx & 0xC000);
            out->current_mode = (u16)(r.bx & 0x3FFF);
        }
        current_id = out->current_mode;
        if (current_id >= 0x100 && current_id <= 0xFFF) add_mode(out, current_id, 1);
        for (n = 0; n < fw_mode_count; ++n) {
            id = fw_modes[n];
            if (id >= 0x100 && id <= 0xFFF) add_mode(out, id, id == current_id);
        }
    }
    probe_edid(out);
    probe_pci(out);
    return (out->flags & (DISP_F_VBE | DISP_F_EDID | DISP_F_PCI)) != 0;
}
#endif
