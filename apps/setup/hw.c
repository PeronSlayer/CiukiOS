/* SPDX-License-Identifier: MIT
 * Screen 4: the hardware check. The rows come from the kernel's read-only
 * records (ciuki_devices, the display query, the process ledger's usable
 * memory, the /dev/net snapshot) and libdisk's enumeration. The verdict
 * refuses only what the kernel can evaluate once Setup runs (plan §3.2):
 * unsupported video, no ATA disk 0, no ATAPI drive with the Setup CD, an
 * unsupported disk. CPU (CMOV) and memory refusals happen before Setup
 * starts, in the loader and the kernel console. The report text is what
 * libsetup writes to \SYSTEM\HARDWARE.TXT. */
#include "setup.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void st_disk_size(char *out, size_t cap, uint64_t bytes)
{
    uint64_t mib = bytes >> 20;
    if (mib < 1024) { snprintf(out, cap, "%" PRIu64 " MB", mib); return; }
    uint64_t tenths = (bytes * 10 + ((uint64_t)1 << 29)) >> 30;
    snprintf(out, cap, "%" PRIu64 ".%" PRIu64 " GB", tenths / 10, tenths % 10);
}

const char *st_part_name(uint8_t type)
{
    switch (type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: return "FAT16";
    case 0x0e: return "FAT16 LBA";
    case 0x0b: return "FAT32";
    case 0x0c: return "FAT32 LBA";
    case 0x05: case 0x0f: return "extended";
    case 0x07: return "NTFS";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xee: return "GPT";
    default: return NULL;
    }
}

void st_disk_contents(const struct st_disk *d, char *out, size_t cap)
{
    if (d->install == ST_INSTALL_DONE) { snprintf(out, cap, "CiukiOS"); return; }
    if (d->install == ST_INSTALL_INCOMPLETE) { snprintf(out, cap, "Incomplete"); return; }
    if (!d->part_count) { snprintf(out, cap, "Empty"); return; }
    const struct st_part *p = &d->parts[0];
    if (!strcmp(p->label, "CIUKIOS")) { snprintf(out, cap, "CiukiOS"); return; }
    const char *name = p->fs[0] ? p->fs : st_part_name(p->type);
    if (name) snprintf(out, cap, "%s data", name);
    else snprintf(out, cap, "Type %02Xh", p->type);
}

static const struct ciuki_device_record *device(const struct st_app *a, unsigned cls)
{
    if (!a->devices_ok) return NULL;
    const struct ciuki_device_record *any = NULL;
    for (unsigned i = 0; i < a->devices.count && i < CIUKI_DEVICES_MAX; i++) {
        const struct ciuki_device_record *d = &a->devices.devices[i];
        if (d->class_id != cls) continue;
        if (d->state == CIUKI_DEVICE_READY || d->state == CIUKI_DEVICE_FIRMWARE_FIRST) return d;
        if (!any) any = d;
    }
    (void)any;
    return NULL;
}

static const struct st_disk *disk0(const struct st_app *a)
{
    for (int i = 0; i < a->disk_count; i++) if (a->disks[i].present && !a->disks[i].slot && !a->disks[i].unit) return &a->disks[i];
    return NULL;
}

void st_inventory(struct st_app *a)
{
    struct st_host *h = &a->h;
    a->devices.version = CIUKI_DEVICES_VERSION; a->devices.size = sizeof(a->devices);
    a->devices_ok = h->devices && !h->devices(h->ctx, &a->devices);
    a->mem_ok = h->memory && !h->memory(h->ctx, &a->mem_kib);
    a->display.version = CIUKI_DISPLAY_VERSION; a->display.size = sizeof(a->display);
    a->display_ok = h->display && !h->display(h->ctx, &a->display);
    a->fb_ok = h->framebuffer && !h->framebuffer(h->ctx, &a->fb_w, &a->fb_h, &a->fb_bpp);
    a->net.version = CIUKI_NET_VERSION; a->net.size = sizeof(a->net);
    a->net_ok = h->network && !h->network(h->ctx, &a->net);
    int n = h->disks ? h->disks(h->ctx, a->disks, 4) : -1;
    a->disk_count = n > 0 && n <= 4 ? n : 0;
    n = h->volumes ? h->volumes(h->ctx, a->volumes, 26) : -1;
    a->volume_count = n > 0 && n <= 26 ? n : 0;
    a->cd_ok = h->cd && !h->cd(h->ctx, &a->cd);
    a->sizes_ok = h->catalogue && !h->catalogue(h->ctx, a->sizes);
    if (!h->release || h->release(h->ctx, a->release, sizeof(a->release))) a->release[0] = 0;
    st_log(a, "inventory devices=%d memory=%d display=%d net=%d disks=%d volumes=%d cd=%d catalogue=%d release=%s",
           a->devices_ok, a->mem_ok, a->display_ok, a->net_ok, a->disk_count, a->volume_count, a->cd_ok, a->sizes_ok,
           a->release[0] ? a->release : "unknown");
}

static void row(struct st_app *a, enum st_mark mark, const char *name, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void row(struct st_app *a, enum st_mark mark, const char *name, const char *fmt, ...)
{
    if (a->row_count >= sizeof(a->rows) / sizeof(a->rows[0])) return;
    struct st_hw_row *r = &a->rows[a->row_count++];
    r->mark = (uint8_t)mark;
    snprintf(r->name, sizeof(r->name), "%s", name);
    va_list ap; va_start(ap, fmt); vsnprintf(r->text, sizeof(r->text), fmt, ap); va_end(ap);
}

static void trimmed(char *out, size_t cap, const char *s)
{
    while (*s == ' ') s++;
    snprintf(out, cap, "%s", s);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

const char *st_stop_code(enum st_stop s)
{
    switch (s) {
    case ST_STOP_VIDEO: return "SETUP-HW-0003";
    case ST_STOP_NO_DISK: return "SETUP-HW-0004";
    case ST_STOP_NO_CD: return "SETUP-HW-0005";
    case ST_STOP_DISK: return "SETUP-HW-0006";
    default: return "SETUP-HW-0000";
    }
}

static void report(struct st_app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void report(struct st_app *a, const char *fmt, ...)
{
    if (a->report_length >= sizeof(a->report) - 1) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(a->report + a->report_length, sizeof(a->report) - a->report_length, fmt, ap);
    va_end(ap);
    if (n > 0) a->report_length += (size_t)n;
    if (a->report_length > sizeof(a->report) - 1) a->report_length = sizeof(a->report) - 1;
}

/* HARDWARE.TXT is plain ASCII: drop the UTF-8 the screen rows use. */
static void ascii(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 2 < cap; p++) {
        if (*p < 0x80) out[k++] = (char)*p;
        else if (p[0] == 0xc3 && p[1] == 0x97) { out[k++] = 'x'; p++; }
        else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) { out[k++] = '-'; p += 2; }
        else if (*p >= 0xc0) out[k++] = '?';
    }
    out[k] = 0;
}

static const char *class_name(unsigned c)
{
    static const char *const names[] = {"system", "display", "keyboard", "mouse", "disk", "cdrom", "sound", "network", "port"};
    return c < sizeof(names) / sizeof(names[0]) ? names[c] : "other";
}
static const char *state_name(unsigned s)
{
    static const char *const names[] = {"ready", "absent", "quarantined", "firmware-first"};
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "unknown";
}

void st_hw_evaluate(struct st_app *a)
{
    char text[200], size[24], model[48];
    a->row_count = 0; a->stop = ST_STOP_NONE; a->report_length = 0;
    /* Processor: the loader refuses CPUs without CMOV, so Setup runs on one that has it. */
    if (a->devices_ok && a->devices.cpu_brand[0]) trimmed(text, sizeof(text), a->devices.cpu_brand);
    else if (a->devices_ok) snprintf(text, sizeof(text), "%.16s family %u model %u stepping %u", a->devices.cpu_vendor,
                                     a->devices.cpu_family, a->devices.cpu_model, a->devices.cpu_stepping);
    else snprintf(text, sizeof(text), "i686-class processor");
    row(a, ST_MARK_OK, "Processor", "%s, CMOV present", text);
    if (a->mem_ok) row(a, ST_MARK_OK, "Memory", "%" PRIu64 " MiB usable (minimum 128 MiB)", a->mem_kib / 1024);
    else row(a, ST_MARK_WARN, "Memory", "Usable memory not reported (minimum 128 MiB)");
    row(a, ST_MARK_OK, "Firmware", "BIOS with E820 map, INT 13h extensions, A20");
    const struct st_disk *d = disk0(a);
    enum st_stop video = ST_STOP_NONE;
    if (!d) {
        row(a, ST_MARK_STOP, "Hard disk", "No hard disk on ATA 0:0 (primary master). This Beta installs only to that disk.");
        if (!a->stop) a->stop = ST_STOP_NO_DISK;
    } else {
        trimmed(model, sizeof(model), d->model);
        st_disk_size(size, sizeof(size), d->sectors * (d->sector_size ? d->sector_size : 512));
        if (!d->installable) {
            row(a, ST_MARK_STOP, "Hard disk", "ATA 0:0 %s, %s: %s", model, size, d->refusal[0] ? d->refusal : "not supported");
        } else if (d->sectors > ((uint64_t)1 << 28)) {
            row(a, ST_MARK_WARN, "Hard disk", "ATA 0:0 %s, %s%s%s; CiukiOS uses the first 128 GB", model, size, d->mode[0] ? ", " : "", d->mode);
        } else row(a, ST_MARK_OK, "Hard disk", "ATA 0:0 %s, %s%s%s", model, size, d->mode[0] ? ", " : "", d->mode);
    }
    if (a->cd_ok && a->cd.present && a->cd.ready) {
        trimmed(model, sizeof(model), a->cd.model);
        row(a, ST_MARK_OK, "CD-ROM", "ATAPI %u:%u %s, Setup CD in the drive", a->cd.slot, a->cd.unit, model);
    } else row(a, ST_MARK_STOP, "CD-ROM", "No ATAPI drive with the CiukiOS Setup CD was found.");
    const char *name = !a->display_ok ? "VBE framebuffer" : a->display.product[0] ? a->display.product :
                       a->display.vendor[0] ? a->display.vendor : a->display.oem[0] ? a->display.oem : "VBE framebuffer";
    unsigned vbe = a->display_ok ? a->display.vbe_version : 0;
    if (!a->fb_ok || a->fb_w < ST_W || a->fb_h < ST_H || a->fb_bpp < 15 || (a->display_ok && vbe < 0x200)) {
        video = ST_STOP_VIDEO;
        row(a, ST_MARK_STOP, "Display", "%.60s, %ux%ux%u: Setup needs a VBE 2.0 linear frame buffer of at least 640x480 with 15-bit colour",
            name, a->fb_w, a->fb_h, a->fb_bpp);
    } else if (a->display_ok && a->display.pci_vendor == 0x5333)
        row(a, ST_MARK_WARN, "Display", "%.60s, VBE %u.%u, %u\xc3\x97%u\xc3\x97%u; 2D accelerator will be tested at first boot",
            name, (vbe >> 8) & 0xff, vbe & 0xff, a->fb_w, a->fb_h, a->fb_bpp);
    else row(a, ST_MARK_OK, "Display", "%.60s, VBE %u.%u, %u\xc3\x97%u\xc3\x97%u", name, (vbe >> 8) & 0xff, vbe & 0xff, a->fb_w, a->fb_h, a->fb_bpp);
    const struct ciuki_device_record *kbd = device(a, CIUKI_DEVICE_KEYBOARD), *mouse = device(a, CIUKI_DEVICE_MOUSE);
    if (kbd && mouse) row(a, ST_MARK_OK, "Input", "%.60s and %.60s%s", kbd->name, mouse->name,
                          kbd->state == CIUKI_DEVICE_FIRMWARE_FIRST ? " (BIOS input)" : "");
    else if (kbd) row(a, ST_MARK_OK, "Input", "%.60s; no mouse (Setup needs only the keyboard)", kbd->name);
    else row(a, ST_MARK_WARN, "Input", "No keyboard reported by the input driver");
    const struct ciuki_device_record *sound = device(a, CIUKI_DEVICE_SOUND), *nic = device(a, CIUKI_DEVICE_NETWORK);
    if (sound) row(a, ST_MARK_OK, "Audio", "%.64s", sound->name);
    else row(a, ST_MARK_WARN, "Audio", "No supported audio device; games run without sound");
    if (nic) row(a, ST_MARK_OK, "Network", "%.64s%s", nic->name, !a->net_ok ? "" : a->net.link == CIUKI_NET_LINK_UP ? ", link up" :
                 a->net.link == CIUKI_NET_LINK_DOWN ? ", cable not connected" : "");
    else row(a, ST_MARK_WARN, "Network", "No supported network adapter; LAN play and Internet are not available");
    row(a, ST_MARK_INFO, "Profile", "Generic PC of the 1998\xe2\x80\x93" "2004 hardware baseline");
    /* Order of the refusals on screen 12. */
    if (video) a->stop = ST_STOP_VIDEO;
    else if (!d) a->stop = ST_STOP_NO_DISK;
    else if (!(a->cd_ok && a->cd.present && a->cd.ready)) a->stop = ST_STOP_NO_CD;
    else if (!d->installable) a->stop = ST_STOP_DISK;

    /* HARDWARE.TXT */
    char line[256];
    time_t now = a->h.realtime ? (time_t)a->h.realtime(a->h.ctx) : 0;
    struct tm tm; gmtime_r(&now, &tm);
    report(a, "CiukiOS 1.0 Beta Setup - hardware report\r\nRelease: %s\r\nDate: %04d-%02d-%02d %02d:%02d UTC\r\nVerdict: %s%s%s\r\n\r\n",
           a->release[0] ? a->release : "unknown", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
           a->stop ? "refused " : "supported", a->stop ? st_stop_code(a->stop) : "", "");
    static const char *const marks[] = {"OK  ", "WARN", "STOP", "INFO", "....", "...."};
    for (unsigned i = 0; i < a->row_count; i++) {
        ascii(line, sizeof(line), a->rows[i].text);
        report(a, "[%s] %-10s %s\r\n", marks[a->rows[i].mark], a->rows[i].name, line);
    }
    /* Build identity of the running CD image (written by scripts/build_image.py).
     * It never holds the image hash: testers quote that from the release page. */
    {
        char build[768]; size_t length = 0;
        if (a->h.read_file && !a->h.read_file(a->h.ctx, "C:\\SYSTEM\\BUILD.TXT", build, sizeof(build) - 1, &length) && length) {
            report(a, "\r\nBuild identity (C:\\SYSTEM\\BUILD.TXT):\r\n");
            for (size_t i = 0, start = 0; i <= length; i++) {
                if (i == length || build[i] == '\n') {
                    size_t end = i; if (end > start && build[end - 1] == '\r') end--;
                    if (end > start) report(a, "  %.*s\r\n", (int)(end - start), build + start);
                    start = i + 1;
                }
            }
        } else report(a, "\r\nBuild identity: not available on this medium.\r\n");
    }
    if (a->display_ok) report(a, "\r\nDisplay: %ux%ux%u VBE %04x vram=%u KiB oem=\"%.40s\" vendor=\"%.40s\" product=\"%.40s\" pci=%04x:%04x\r\n",
                              a->display.width, a->display.height, a->display.bpp, a->display.vbe_version, a->display.vram_bytes / 1024,
                              a->display.oem, a->display.vendor, a->display.product, a->display.pci_vendor, a->display.pci_device);
    report(a, "\r\nDisks:\r\n");
    for (int i = 0; i < a->disk_count; i++) {
        const struct st_disk *k = &a->disks[i];
        trimmed(model, sizeof(model), k->model);
        report(a, "  ATA %u:%u \"%s\" serial \"%.20s\" %" PRIu64 " sectors of %u bytes, %s%s%s\r\n", k->slot, k->unit, model, k->serial,
               k->sectors, k->sector_size, k->installable ? "installable" : "not installable: ", k->installable ? "" : k->refusal,
               k->install == ST_INSTALL_DONE ? ", CiukiOS installed" : k->install == ST_INSTALL_INCOMPLETE ? ", incomplete installation" : "");
        for (unsigned p = 0; p < k->part_count && p < ST_PARTS; p++) {
            const struct st_part *q = &k->parts[p];
            st_disk_size(size, sizeof(size), q->count * 512);
            report(a, "    partition %u type %02Xh%s start %" PRIu64 " count %" PRIu64 " (%s)%s%c%s %s\r\n", p + 1, q->type, q->active ? " active" : "",
                   q->start, q->count, size, q->letter ? " " : "", q->letter ? q->letter : ' ', q->letter ? ":" : "", q->label);
        }
    }
    if (a->cd_ok) report(a, "  ATAPI %u:%u \"%.40s\" %s\r\n", a->cd.slot, a->cd.unit, a->cd.model, a->cd.ready ? "media ready" : "no medium");
    if (a->devices_ok) {
        report(a, "\r\nDevices (kernel inventory, generation %u%s):\r\n", a->devices.generation,
               a->devices.flags & CIUKI_DEVICES_TRUNCATED ? ", truncated" : "");
        for (unsigned i = 0; i < a->devices.count && i < CIUKI_DEVICES_MAX; i++) {
            const struct ciuki_device_record *r = &a->devices.devices[i];
            report(a, "  %-8s %-14s %04x:%04x %-20.32s %.64s%s%.80s\r\n", class_name(r->class_id), state_name(r->state),
                   r->vendor & 0xffff, r->device & 0xffff, r->driver, r->name, r->reason[0] ? " - " : "", r->reason);
        }
    }
    st_log(a, "hardware verdict=%s rows=%u", a->stop ? st_stop_code(a->stop) : "supported", a->row_count);
}
