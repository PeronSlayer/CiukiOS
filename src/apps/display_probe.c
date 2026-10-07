/* Firmware display discovery for DISPLAY.APP. This only reads the active
 * firmware configuration; it never changes the video mode. */
#include "display_probe.h"

#ifdef DISP_PROBE_HOST_TEST
#define PROBE_OFF(p) disp_probe_host_ptr(p)
#else
#define PROBE_OFF(p) ((u16)(p))
#endif

#if !defined(DISP_PROBE_HOST_TEST) || defined(DISP_PROBE_TEST_BIOS)
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
#if !defined(DISP_PROBE_HOST_TEST) || defined(DISP_PROBE_TEST_BIOS)
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

#if !defined(DISP_PROBE_HOST_TEST) || defined(DISP_PROBE_TEST_BIOS)
static int get_mode_info(u16 id)
{
    struct regs r;
    mem_set(mode_info, 0, sizeof mode_info);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x4F01; r.cx = id;
    r.es = app_seg(); r.di = PROBE_OFF(mode_info);
    return bios_call(&r);
}

static int valid_pitch(u16 width, u16 height, u16 bpp, u16 pitch, u32 *frame_out)
{
    u16 frame_bpp = (u16)(((u32)width * ((bpp + 7) / 8)) & 0xFFFF);
    u32 frame;
    if (!pitch || pitch > 32767 || pitch < frame_bpp) return 0;
    frame = (u32)pitch * height;
    if (frame > 0x00800000UL || frame < frame_bpp) return 0;
    *frame_out = frame;
    return 1;
}

static int valid_rgb_masks(const u8 *masks, u16 bpp)
{
    u32 used = 0;
    u16 i;
    for (i = 0; i < 3; ++i) {
        u8 size = masks[i * 2], pos = masks[i * 2 + 1];
        u32 bits;
        if (size < 5 || size > 8 || (u16)size + pos > bpp) return 0;
        bits = ((1UL << size) - 1) << pos;
        if (used & bits) return 0;
        used |= bits;
    }
    return 1;
}

static int mode_format_ok(u16 width, u16 height, u16 bpp, u8 model)
{
    return width >= 640 && width <= 2560 && height >= 480 && height <= 1600 &&
        (bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32) && model == 6;
}

static int banked_access_ok(u16 attrs, u16 width, u16 height, u16 bpp, u16 pitch)
{
    u16 gran, size, seg;
    u32 frame, last_bank;
    if (attrs & 0x40) return 0;                  /* no VGA window */
    if ((mode_info[2] & 5) != 5) return 0;        /* WinA relocatable + writable */
    gran = rd16(mode_info + 4); size = rd16(mode_info + 6);
    seg = rd16(mode_info + 8);
    if (!gran || gran > 64 || (64 % gran) || size < 64 || (seg && seg != 0xA000)) return 0;
    if (!valid_pitch(width, height, bpp, pitch, &frame)) return 0;
    if (bpp > 8 && !valid_rgb_masks(mode_info + 31, bpp)) return 0;
    last_bank = ((frame - 1) >> 16) * (64 / gran);
    return last_bank <= 0xFFFFUL;
}

static int linear_access_ok(u16 attrs, u16 width, u16 height, u16 bpp, u16 pitch, u16 vbe_version)
{
    u32 frame;
    const u8 *masks = mode_info + 31;
    if (!(attrs & 0x80) || !rd32(mode_info + 40)) return 0;
    if (!valid_pitch(width, height, bpp, pitch, &frame)) return 0;
    if (bpp > 8) {
        if (vbe_version >= 0x0300 && mode_info[54]) masks = mode_info + 54;
        if (!valid_rgb_masks(masks, bpp)) return 0;
    }
    return 1;
}

static int get_scanline(u16 *pitch, u16 *lines)
{
    struct regs r;
    mem_set(&r, 0, sizeof r); r.ax = 0x4F06; r.bx = 1;
    if (!bios_call(&r)) return 0;
    *pitch = r.bx; *lines = r.dx;
    return 1;
}

static int add_mode(struct disp_probe *p, u16 id, int current, int require_banked)
{
    u16 attrs, width, height, bank_pitch, linear_pitch, pitch, bpp, flags = 0;
    u16 active_pitch = 0, active_lines = 0;
    u32 frame = 0;
    int banked_ok, linear_ok, current_linear;
    struct disp_mode *m;
    if (!get_mode_info(id)) return 0;
    attrs = rd16(mode_info);
    width = rd16(mode_info + 18); height = rd16(mode_info + 20);
    bpp = mode_info[25];
    if (!mode_format_ok(width, height, bpp, mode_info[27])) return 0;
    bank_pitch = rd16(mode_info + 16);
    linear_pitch = p->vbe_version >= 0x0300 && rd16(mode_info + 50) ?
        rd16(mode_info + 50) : bank_pitch;
    banked_ok = (attrs & 0x19) == 0x19 &&
        banked_access_ok(attrs, width, height, bpp, bank_pitch);
    linear_ok = (attrs & 0x19) == 0x19 &&
        linear_access_ok(attrs, width, height, bpp, linear_pitch, p->vbe_version);
    if (banked_ok) flags |= DISP_MODE_BANKED;
    if (linear_ok) flags |= DISP_MODE_LINEAR;

    if (current) {
        current_linear = !!(p->current_mode_flags & 0x4000);
        pitch = current_linear ? linear_pitch : bank_pitch;
        if (get_scanline(&active_pitch, &active_lines) &&
            active_pitch >= (u16)(((u32)width * ((bpp + 7) / 8)) & 0xFFFF) &&
            active_pitch <= 32767)
            pitch = active_pitch;
        (void)active_lines;
        if (valid_pitch(width, height, bpp, pitch, &frame)) {
            p->current_width = width; p->current_height = height;
            p->current_pitch = pitch; p->current_bpp = bpp;
            p->current_frame_bytes = frame;
            p->framebuffer_phys = rd32(mode_info + 40);
            p->flags |= DISP_F_CURRENT;
        }
    }

    /* Match the runtime access path: V86 cannot open the local LFB mapping. */
    if (require_banked) {
        if (!banked_ok) return 0;
        pitch = bank_pitch;
    } else if (linear_ok) pitch = linear_pitch;
    else if (banked_ok) pitch = bank_pitch;
    else return 0;
    if (!valid_pitch(width, height, bpp, pitch, &frame)) return 0;
    if (p->video_memory_64k &&
        p->video_memory_64k < (u16)((frame + 65535UL) / 65536UL)) return 0;
    {
        u16 i;
        for (i = 0; i < p->mode_count; ++i) if (p->modes[i].id == id) return 1;
    }
    if (p->mode_count >= DISP_MAX_MODES) { p->modes_truncated = 1; p->flags |= DISP_F_TRUNCATED; return 0; }
    m = &p->modes[p->mode_count++];
    m->id = id; m->width = width; m->height = height; m->pitch = pitch;
    m->bpp = bpp; m->flags = flags; m->frame_bytes = frame;
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
    r.es = app_seg(); r.di = PROBE_OFF(edid_block);
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
    else if (p->pci_vendor == 0x5333) p->adapter_kind = DISP_ADAPTER_S3;
    else if (p->pci_vendor == 0x102B) p->adapter_kind = DISP_ADAPTER_MATROX;
    else if (p->pci_vendor == 0x1039) p->adapter_kind = DISP_ADAPTER_SIS;
    else if (p->pci_vendor == 0x121A) p->adapter_kind = DISP_ADAPTER_3DFX;
    else if (p->pci_vendor == 0x1013) p->adapter_kind = DISP_ADAPTER_CIRRUS;
    else if (p->pci_vendor == 0x1023) p->adapter_kind = DISP_ADAPTER_TRIDENT;
    else if (p->pci_vendor == 0x10C8) p->adapter_kind = DISP_ADAPTER_NEOMAGIC;
    else p->adapter_kind = DISP_ADAPTER_UNKNOWN;
}

static void probe_pci(struct disp_probe *p)
{
    struct regs r;
    u16 max_bus, bus, dev, fn;
    int found = 0, first_found = 0;
    u16 first_vendor = 0, first_device = 0, first_bus = 0, first_devfn = 0;
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
            if (!first_found) {
                first_vendor = vendor; first_device = device;
                first_bus = bus; first_devfn = devfn;
                first_found = 1;
            }
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
    if (!found && first_found) {
        p->pci_vendor = first_vendor;
        p->pci_device = first_device;
        p->pci_bus = first_bus;
        p->pci_devfn = first_devfn;
        found = 1;
    }
    if (p->pci_display_count) p->flags |= DISP_F_PCI;
    if (found) p->flags |= DISP_F_PCI_MATCH;
    if (found) classify_adapter(p);
}

int disp_probe_init(struct disp_probe *out, int require_banked)
{
    struct regs r;
    u16 list_seg = 0, list_off = 0, id, n, current_id;
    int have_vbe = 0;
    if (!out) return 0;
    mem_set(out, 0, sizeof *out);
    mem_set(vbe_info, 0, sizeof vbe_info);
    vbe_info[0] = 'V'; vbe_info[1] = 'B'; vbe_info[2] = 'E'; vbe_info[3] = '2';
    mem_set(&r, 0, sizeof r); r.ax = 0x4F00; r.es = app_seg(); r.di = PROBE_OFF(vbe_info);
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
        if (current_id >= 0x100 && current_id <= 0xFFF)
            add_mode(out, current_id, 1, require_banked);
        for (n = 0; n < fw_mode_count; ++n) {
            id = fw_modes[n];
            if (id >= 0x100 && id <= 0xFFF)
                add_mode(out, id, id == current_id, require_banked);
        }
    }
    probe_edid(out);
    probe_pci(out);
    return (out->flags & (DISP_F_VBE | DISP_F_EDID | DISP_F_PCI)) != 0;
}

int disp_probe_get_mode_diag(u16 id, struct disp_mode_diag *out)
{
    if (!out || !get_mode_info(id)) return 0;
    mem_set(out, 0, sizeof *out);
    out->attributes = rd16(mode_info);
    out->window_a_attributes = mode_info[2];
    out->window_b_attributes = mode_info[3];
    out->granularity_kb = rd16(mode_info + 4);
    out->window_kb = rd16(mode_info + 6);
    out->window_a_segment = rd16(mode_info + 8);
    out->window_b_segment = rd16(mode_info + 10);
    out->banked_pitch = rd16(mode_info + 16);
    out->linear_pitch = rd16(mode_info + 50);
    out->bpp = mode_info[25];
    out->memory_model = mode_info[27];
    out->framebuffer_phys = rd32(mode_info + 40);
    mem_set(out->bank_masks, 0, sizeof out->bank_masks);
    {
        u16 i;
        for (i = 0; i < 8; ++i) out->bank_masks[i] = mode_info[31 + i];
        for (i = 0; i < 8; ++i) out->linear_masks[i] = mode_info[54 + i];
    }
    return 1;
}
#endif

void disp_probe_sort_modes(struct disp_probe *p)
{
    u16 i, j;
    struct disp_mode temp;
    if (!p) return;
    /* Smallest geometry first, then the lowest usable direct-color depth. */
    for (i = 1; i < p->mode_count; ++i) {
        temp = p->modes[i]; j = i;
        while (j > 0) {
            struct disp_mode *prev = &p->modes[j - 1];
            if (prev->width < temp.width ||
                (prev->width == temp.width && prev->height < temp.height) ||
                (prev->width == temp.width && prev->height == temp.height && prev->bpp <= temp.bpp))
                break;
            p->modes[j] = *prev; --j;
        }
        p->modes[j] = temp;
    }
}
