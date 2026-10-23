/* Read-only inventory. Sources and invariants: docs/design/device-manager.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/devices.h>
#include <ciuki/registry.h>
#include <ciuki/init.h>
#include <ciuki/ata.h>
#include <ciuki/i8042.h>
#include <ciuki/fbdev.h>
#include <ciuki/ac97.h>
#include <ciuki/net.h>
#include <ciuki/usb.h>
#include <ciuki/process.h>
#include <ciuki/uaddr.h>
#include <ciuki/com.h>
#pragma weak com_snapshot
#include <ciuki/firmware.h>
#include <ciuki/thinkpad_ec.h>
#pragma weak thinkpad_ec_smbios_snapshot
#include <ciuki/bootlog.h>
#pragma weak bootlog_report_snapshot
#pragma weak firmware_inventory_page
#ifndef CIUKI_BUILD_ID
#define CIUKI_BUILD_ID "unknown"
#endif

/* Like proc_list: UP, non-preemptible kernel, no waits between fill and copy.
 * Keeping these out of the 16 KiB task stack also avoids heap size limits. */
static struct registry_inventory_copy inventory;
static struct ciuki_devices_info query_buffer;
static struct ciuki_firmware_page firmware_buffer;
static struct ciuki_hardware_info hardware_buffer;
extern bool storage_readonly_info(unsigned, char *, unsigned) __attribute__((weak));
/* f9-01: Setup mode (core/setup_mode.c); absent in standalone harnesses. */
extern bool setup_mode(void) __attribute__((weak));
extern bool savage2d_summary(bool *, bool *, const char **) __attribute__((weak));

static void string(char *out, unsigned size, const char *in)
{
    unsigned i = 0;
    if (in) for (; i + 1 < size && in[i]; i++) out[i] = in[i];
    out[i] = 0;
}
static bool equal(const char *a, const char *b)
{ return a && b && !strncmp(a, b, strlen(b) + 1); }
/* f9-01: "Setup mode; writes=<n>" for the zero-write assertion on request. */
static const char *setup_writes(char out[48], uint64_t value)
{
    static const char prefix[] = "Setup mode; writes=";
    char digits[21]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    memcpy(out, prefix, sizeof(prefix) - 1);
    unsigned at = sizeof(prefix) - 1;
    while (n) out[at++] = digits[--n];
    out[at] = 0;
    return out;
}
static struct ciuki_device_record *append(struct ciuki_devices_info *out, unsigned id,
                                         unsigned class_id, unsigned bus, const char *name,
                                         const char *driver, unsigned state, const char *reason)
{
    if (out->count == CIUKI_DEVICES_MAX) { out->flags |= CIUKI_DEVICES_TRUNCATED; return 0; }
    struct ciuki_device_record *d = &out->devices[out->count++];
    d->id = id; d->class_id = class_id; d->bus_type = bus; d->state = state;
    string(d->name, sizeof(d->name), name); string(d->driver, sizeof(d->driver), driver);
    string(d->reason, sizeof(d->reason), reason);
    return d;
}
static void resource(struct ciuki_device_record *d, unsigned type, uint64_t start, uint64_t end, unsigned flags)
{
    if (!d) return;
    for (unsigned k = 0; k < d->resource_count; k++) {
        struct ciuki_device_resource *r = &d->resources[k];
        if (r->type == type && r->start == start) {
            if (!(flags & CIUKI_RESOURCE_UNKNOWN_SIZE)) *r = (struct ciuki_device_resource){type, flags, start, end};
            return;
        }
    }
    if (d->resource_count == CIUKI_DEVICE_RESOURCES) { d->flags |= CIUKI_DEVICES_TRUNCATED; return; }
    d->resources[d->resource_count++] = (struct ciuki_device_resource){type, flags, start, end};
}
static void owner_resources(struct ciuki_device_record *d, const char *owner)
{
    if (!d) return;
    for (unsigned k = 0; k < inventory.resource_count; k++) {
        const struct resource *r = &inventory.resources[k];
        if (r->state == RS_RELEASED || !equal(r->owner, owner)) continue;
        resource(d, r->type, r->start, r->end, 0);
        if (r->generation > d->generation) d->generation = r->generation;
        if (r->state == RS_QUARANTINED) {
            d->state = CIUKI_DEVICE_QUARANTINED;
            string(d->reason, sizeof(d->reason), "Resource lease quarantined; retained until reboot.");
        }
    }
}
static bool com_devices(struct ciuki_devices_info *out)
{
    if (!com_snapshot) return false;
    bool logger=false;
    static const char *const chips[]={"unprobed", "8250", "16450", "16550", "16550A", "16650", "16750"};
    for (unsigned i=0;i<CIUKI_COM_MAX;i++) {
        struct ciuki_com_status s; const char *reason;
        if (!com_snapshot(i,&s,&reason)) continue;
        logger|=!!(s.flags&CIUKI_COM_LOGGER);
        char name[64];
        ksnprintf(name,sizeof(name),"COM%u %s (%u-byte capacity)%s",s.number,
            s.chip<ARRAY_SIZE(chips) ? chips[s.chip] : "unknown",s.fifo_depth,
            s.flags&CIUKI_COM_LOGGER ? " kernel log" : "");
        struct ciuki_device_record *d=append(out,0x70000+i,CIUKI_DEVICE_PORT,
            s.origin==CIUKI_COM_PCI ? CIUKI_BUS_PCI : CIUKI_BUS_LEGACY,name,"uart",
            s.flags&CIUKI_COM_QUARANTINED ? CIUKI_DEVICE_QUARANTINED :
            s.flags&(CIUKI_COM_READY|CIUKI_COM_LOGGER) ? CIUKI_DEVICE_READY : CIUKI_DEVICE_FIRMWARE_FIRST,reason);
        resource(d,RES_PORT,s.base,s.base+8,0);
        if (s.irq && s.irq<16) resource(d,RES_IRQ,s.irq,s.irq+1,0);
        if (d) {
            d->generation=s.generation;
            if (s.flags&CIUKI_COM_OPEN) string(d->reason,sizeof(d->reason),"Exclusive native COM file description owns this port.");
        }
    }
    return logger;
}
static void activation(struct ciuki_device_record *d, const char *name)
{
    if (!d) return;
    for (unsigned i = 0; i < drivers_activation_count(); i++) {
        const struct activation_entry *e = drivers_activation_get(i);
        if (e && e->kind == ACTIVATION_DEVICE && equal(e->device, name)) {
            d->activation_seq = e->seq;
            string(d->reason, sizeof(d->reason), e->reason);
            break;
        }
    }
}
static unsigned pci_class(const struct pci_func *p)
{
    if (p->class_code == 3) return CIUKI_DEVICE_DISPLAY;
    if (p->class_code == 2) return CIUKI_DEVICE_NETWORK;
    if (p->class_code == 4) return CIUKI_DEVICE_SOUND;
    if (p->class_code == 7) return CIUKI_DEVICE_PORT;
    return CIUKI_DEVICE_SYSTEM;
}
static void cpu_info(struct ciuki_devices_info *out)
{
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    uint32_t max = a;
    memcpy(out->cpu_vendor, &b, 4); memcpy(out->cpu_vendor + 4, &d, 4); memcpy(out->cpu_vendor + 8, &c, 4);
    if (max >= 1) {
        cpuid(1, &a, &b, &c, &d);
        unsigned family = (a >> 8) & 15;
        out->cpu_family = family == 15 ? family + ((a >> 20) & 255) : family;
        out->cpu_model = (a >> 4) & 15;
        if (family == 6 || family == 15) out->cpu_model |= ((a >> 16) & 15) << 4;
        out->cpu_stepping = a & 15;
    }
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) for (unsigned k = 0; k < 3; k++) {
        cpuid(0x80000002u + k, &a, &b, &c, &d);
        memcpy(out->cpu_brand + k * 16, &a, 4); memcpy(out->cpu_brand + k * 16 + 4, &b, 4);
        memcpy(out->cpu_brand + k * 16 + 8, &c, 4); memcpy(out->cpu_brand + k * 16 + 12, &d, 4);
    }
    /* Vendor-defined bytes must not inject UI control characters. */
    for (unsigned k = 0; k < 48 && out->cpu_brand[k]; k++)
        if ((unsigned char)out->cpu_brand[k] < 32 || (unsigned char)out->cpu_brand[k] > 126) out->cpu_brand[k] = '?';
}
static const char *driver_for(const char *owner)
{
    if (equal(owner, "timer") || equal(owner, "timer/speaker")) return "pit";
    if (equal(owner, "rtc")) return "rtc";
    if (equal(owner, "pic") || equal(owner, "pic-cascade")) return "pic";
    if (equal(owner, "pci")) return "pci-inventory";
    if (equal(owner, "uart")) return "uart";
    if (equal(owner, "dma") || equal(owner, "dma-page") || equal(owner, "dma-cascade")) return "isa-dma-guard";
    return "loader";
}
static const char *legacy_name(const char *owner)
{
    if (equal(owner, "timer")) return "8254 programmable interval timer";
    if (equal(owner, "rtc")) return "Real-time clock (CMOS calendar)";
    if (equal(owner, "pic")) return "8259 interrupt controllers";
    if (equal(owner, "uart")) return "16550 serial port";
    return owner;
}
/* No driver callbacks or hardware access: only pinned boot NIC contexts.
 * IRQ quarantine updates status, so copy the fields under the same IRQ lock. */
static void network_binding(struct ciuki_device_record *d, const struct pci_func *p)
{
    uint32_t flags = irq_save();
    for (unsigned i = 0; i < net_count() && i < NETIF_MAX; i++) {
        const struct netif *n = net_get(i);
        if (!n || !n->driver || p->bus != n->pci.bus || p->dev != n->pci.dev ||
            p->fn != n->pci.fn || p->vendor != n->pci.vendor || p->device != n->pci.device) continue;
        string(d->driver, sizeof(d->driver), n->driver->name);
        d->state = n->quarantined ? CIUKI_DEVICE_QUARANTINED :
                   n->ready && n->running ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT;
        string(d->reason, sizeof(d->reason), n->reason);
        if (n->owner[0]) owner_resources(d, n->owner);
        if (n->generation) d->generation = n->generation;
        break;
    }
    irq_restore(flags);
}
static void usb_binding(struct ciuki_device_record *d, const struct pci_func *p)
{
    for (unsigned i = 0; i < usb_count(); i++) {
        const struct usb_host *h = usb_get(i);
        if (!h || h->pci.bus != p->bus || h->pci.dev != p->dev || h->pci.fn != p->fn ||
            h->pci.vendor != p->vendor || h->pci.device != p->device) continue;
        string(d->driver, sizeof(d->driver), "usb-host");
        d->state = h->quarantined ? CIUKI_DEVICE_QUARANTINED :
            h->ready ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT;
        string(d->reason, sizeof(d->reason), h->reason);
        if (h->inventory.parent_locator) {
            uint32_t parent = h->inventory.parent_locator;
            ksnprintf(d->reason, sizeof(d->reason), "%s; companion of %02x:%02x.%u",
                      h->reason ? h->reason : "discovered", (parent >> 16) & 255,
                      (parent >> 8) & 255, parent & 255);
        }
        if (h->owner[0]) owner_resources(d, h->owner);
        d->generation = h->generation;
    }
}
static void usb_devices(struct ciuki_devices_info *out)
{
    for (unsigned i = 0; i < usb_count(); i++) {
        const struct usb_host *h = usb_get(i); if (!h) continue;
        for (unsigned k = 0; k < h->device_count; k++) {
            const struct usb_device *u = &h->devices[k];
            unsigned class_id = CIUKI_DEVICE_SYSTEM;
            const char *name = u->class_code == 9 ? "USB hub" : "USB device";
            for (unsigned j = 0; j < USB_HID_MAX; j++) if (u->hid[j].endpoint) {
                if (u->hid[j].protocol == 1) { class_id = CIUKI_DEVICE_KEYBOARD; name = "USB HID keyboard"; break; }
                class_id = CIUKI_DEVICE_MOUSE; name = "USB HID mouse";
            }
            if (u->storage.bound) { class_id=CIUKI_DEVICE_DISK; name="USB mass storage"; }
            struct ciuki_device_record *d = append(out, 0x60000 + i * USB_DEVICE_MAX + k,
                class_id, CIUKI_BUS_USB, name, u->storage.bound ? "usb-storage" : "usb-host",
                u->quarantined || h->quarantined ? CIUKI_DEVICE_QUARANTINED :
                u->present && u->configured && !u->storage.ejected ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT, u->reason);
            if (!d) continue;
            if (u->high_speed) ksnprintf(d->reason, sizeof(d->reason), "%s; high speed", u->reason ? u->reason : "discovered");
            d->bus = i; d->slot = u->address; d->function = u->port;
            d->subclass = u->parent; d->prog_if = u->low_speed;
            d->vendor = u->vendor; d->device = u->product; d->generation = u->generation;
        }
    }
}
static void drivers(struct ciuki_devices_info *out)
{
    static const struct { const char *name, *policy; } builtins[] = {
        {"loader", "Firmware reservations retained"}, {"pic", "Core IRQ dispatcher"},
        {"pit", "Core scheduler timer"}, {"rtc", "Native CMOS calendar; IRQ8 remains masked"},
        {"uart", "Serial diagnostics; boot policy"}, {"isa-dma-guard", "Inherited ISA DMA masked"},
        {"pci-inventory", "Boot configuration only; no BAR sizing"},
        {"fbdev", "Loader-selected VBE framebuffer"},
        {"i8042", "Native PS/2 only when selected at boot"},
        {"firmware-input", "Sole firmware-first PS/2 controller lease"},
        {"ata-pio", "Legacy ATA/ATAPI; disabled in safe mode"},
        {"ac97", "Intel ICH, SiS 7012 and nForce AC97; safe mode disables activation"},
        {"e100", "Retained PCI binding; safe mode disables NIC activation"},
        {"rtl8139", "Retained PCI binding; safe mode disables NIC activation"},
        {"ne2000", "Retained PCI binding; safe mode disables NIC activation"},
        {"usb-host", "UHCI/OHCI/EHCI; HS hubs: TT unsupported; firmware-first"},
        {"e1000", "Retained PCI binding; safe mode disables NIC activation"},
        {"pcnet", "Retained PCI binding; safe mode disables NIC activation"},
        {"tulip", "Retained PCI binding; safe mode disables NIC activation"},
        {"3c905", "Retained PCI binding; safe mode disables NIC activation"},
        {"rhine", "Retained PCI binding; safe mode disables NIC activation"},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(builtins); i++) {
        bool detected = false;
        for (unsigned k = 0; k < out->count; k++)
            if (equal(out->devices[k].driver, builtins[i].name)) detected = true;
        /* Preserve the existing catalogue order. The fixed ABI has sixteen
         * rows; new NIC rows are included when detected, replacing an
         * unbound older NIC row if necessary. At most NETIF_MAX NICs are admitted. */
        if (i >= 16 && !detected) continue;
        unsigned slot = out->driver_count;
        if (slot == CIUKI_DRIVERS_MAX) {
            for (unsigned k = 12; k < out->driver_count; k++)
                if (!out->drivers[k].bound_devices && out->drivers[k].state == CIUKI_DEVICE_ABSENT) { slot = k; break; }
            if (slot == CIUKI_DRIVERS_MAX) { out->flags |= CIUKI_DEVICES_TRUNCATED; continue; }
        } else out->driver_count++;
        struct ciuki_driver_record *r = &out->drivers[slot]; memset(r, 0, sizeof(*r));
        /* The audio slot reports the selected transport (f4-11). */
        bool selected_audio=equal(builtins[i].name,"ac97") && audio.backend_name;
        string(r->name, sizeof(r->name), selected_audio ? audio.backend_name : builtins[i].name);
        string(r->build, sizeof(r->build), CIUKI_BUILD_ID);
        string(r->policy, sizeof(r->policy), selected_audio ? "Selected PCM transport; safe mode disables activation" : builtins[i].policy);
        r->state = CIUKI_DEVICE_ABSENT;
        for (unsigned k = 0; k < out->count; k++) {
            const struct ciuki_device_record *d = &out->devices[k];
            if (!equal(d->driver, r->name) || d->state == CIUKI_DEVICE_ABSENT) continue;
            r->bound_devices++;
            if (r->state != CIUKI_DEVICE_QUARANTINED) r->state = d->state;
        }
    }
}
void devices_snapshot(struct ciuki_devices_info *out)
{
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_DEVICES_VERSION; out->size = sizeof(*out);
    registry_inventory(&inventory);
    out->generation = inventory.generation;
    if (inventory.truncated) out->flags |= CIUKI_DEVICES_TRUNCATED;
    out->boot_flags = g_boot.flags; out->input_policy = g_boot.input_policy;
    string(out->boot_options, sizeof(out->boot_options), g_boot.options);
    uint32_t f = irq_save(); out->uptime_ms = g_ticks; irq_restore(f);
    cpu_info(out);
    /* Preserve the fixed ABI: read-only volumes use ordinary inventory rows.
     * Reasons come from the live retained cause, not the boot ledger. */
    if (storage_readonly_info) for (unsigned drive=2;drive<26;drive++) {
        char reason[80], name[]="C: read-only volume";
        if (!storage_readonly_info(drive,reason,sizeof(reason))) continue;
        name[0]=(char)('A'+drive);
        append(out,0x41000+drive,CIUKI_DEVICE_DISK,CIUKI_BUS_LOADER,name,"fat",
            CIUKI_DEVICE_QUARANTINED,reason);
    }

    struct drivers_state state; drivers_snapshot(&state);
    struct i8042_stats input; i8042_snapshot(&input);
    for (unsigned k = 0; k < inventory.resource_count; k++) {
        const struct resource *r = &inventory.resources[k];
        if (r->state == RS_RELEASED) continue;
        struct ciuki_device_lease *l = &out->leases[out->lease_count++];
        string(l->owner, sizeof(l->owner), r->owner);
        l->type = r->type; l->state = r->state; l->generation = r->generation;
        l->start = r->start; l->end = r->end;
    }
    for (unsigned k = 0; k < inventory.function_count; k++) {
        const struct pci_func *p = &inventory.functions[k];
        unsigned id = 0x10000u | ((unsigned)p->bus << 8) | ((unsigned)p->dev << 3) | p->fn;
        const struct pci_func *sound_pci=audio.backend_pci.vendor ? &audio.backend_pci : &ac97.pci;
        const char *sound_driver=audio.backend_name ? audio.backend_name : "ac97";
        bool bound = sound_pci->vendor && p->bus == sound_pci->bus && p->dev == sound_pci->dev && p->fn == sound_pci->fn &&
                     p->vendor == sound_pci->vendor && p->device == sound_pci->device &&
                     (audio.backend_touched || ac97.touched || audio.ready || audio.quarantined || ac97.quarantine);
        struct ciuki_device_record *d = append(out, id, pci_class(p), CIUKI_BUS_PCI,
            "PCI function", bound ? sound_driver : "", bound ? (audio.quarantined ? CIUKI_DEVICE_QUARANTINED :
            audio.ready ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT) : CIUKI_DEVICE_FIRMWARE_FIRST,
            bound ? audio.reason : "No native driver bound; firmware configuration retained.");
        if (!d) continue;
        d->bus = p->bus; d->slot = p->dev; d->function = p->fn;
        d->vendor = p->vendor; d->device = p->device; d->sub_vendor = p->sub_vendor; d->sub_device = p->sub_device;
        d->pci_class = p->class_code; d->subclass = p->subclass; d->prog_if = p->prog_if; d->revision = p->revision;
        for (unsigned bar = 0; bar < 6; bar++) {
            uint32_t v = p->bar[bar];
            if (!v || v == UINT32_MAX) continue;
            unsigned type = (v & 1) ? RES_PORT : RES_MMIO;
            uint64_t base = v & ((v & 1) ? ~3u : ~15u);
            if (!(v & 1) && (v & 6) == 4) {
                if (bar == 5) continue;
                base |= (uint64_t)p->bar[++bar] << 32;
            }
            if (base) resource(d, type, base, base, CIUKI_RESOURCE_UNKNOWN_SIZE);
        }
        if (p->irq_pin && p->irq_line < 16) resource(d, RES_IRQ, p->irq_line, p->irq_line + 1, 0);
        if (p->class_code == 2) network_binding(d, p);
        if (p->class_code == 0x0c && p->subclass == 3) usb_binding(d, p);
        if (bound) { owner_resources(d, sound_driver); d->generation = audio.caps.generation ? audio.caps.generation : ac97.generation; }
    }
    bool com_logger=com_devices(out);
    /* Registry owners become legacy logical devices. Special owners below
     * are represented once by their actual backend rather than inferred PCI bindings. */
    for (unsigned k = 0; k < inventory.resource_count; k++) {
        const struct resource *r = &inventory.resources[k];
        if (r->state == RS_RELEASED || (com_logger && equal(r->owner,"uart")) ||
            (com_snapshot && !strncmp(r->owner,"com",3)) || equal(r->owner, "input") || equal(r->owner, "i8042") ||
            equal(r->owner, "firmware-input") || equal(r->owner, "boot-framebuffer") || equal(r->owner, "fbdev") ||
            !strncmp(r->owner, "usb-", 4) || !strncmp(r->owner, "net-", 4) || equal(r->owner, "ata0") || equal(r->owner, "ata1") || equal(r->owner, "ac97") || equal(r->owner, "ac97-dma") || equal(r->owner, "es137x") || equal(r->owner, "via82xx") || equal(r->owner, "sb16")) continue;
        bool seen = false;
        for (unsigned j = 0; j < k; j++) if (inventory.resources[j].state != RS_RELEASED && equal(inventory.resources[j].owner, r->owner)) seen = true;
        if (seen) continue;
        const char *driver = driver_for(r->owner);
        bool core = equal(driver, "pit") || equal(driver, "pic") || equal(driver, "rtc") || equal(driver, "isa-dma-guard");
        bool uart = equal(driver, "uart") && (g_boot.flags & CBI_F_SERIAL_LOG);
        struct ciuki_device_record *d = append(out, 0x20000u + k, equal(r->owner, "uart") ? CIUKI_DEVICE_PORT : CIUKI_DEVICE_SYSTEM,
            CIUKI_BUS_LEGACY, legacy_name(r->owner), driver,
            core || uart || r->state == RS_ACTIVE ? CIUKI_DEVICE_READY :
            r->state == RS_FIRMWARE ? CIUKI_DEVICE_FIRMWARE_FIRST : CIUKI_DEVICE_ABSENT,
            core || uart ? "Core driver initialized before process startup." :
            r->state == RS_FIRMWARE ? "Boot reservation retained." : "Lease retained; no active driver recorded.");
        owner_resources(d, r->owner);
    }
    bool uart_found = false;
    for (unsigned k = 0; k < inventory.resource_count; k++)
        if (inventory.resources[k].state != RS_RELEASED && equal(inventory.resources[k].owner, "uart")) uart_found = true;
    if (!uart_found) append(out, 0x200ffu, CIUKI_DEVICE_PORT, CIUKI_BUS_LEGACY, "Serial port", "uart",
                            CIUKI_DEVICE_ABSENT, "No UART reservation from the loader.");
    struct ciuki_device_record *d = append(out, 0x30000, CIUKI_DEVICE_DISPLAY, CIUKI_BUS_LOADER,
        "VBE framebuffer", "fbdev", fbdev_get()->present ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT, "No framebuffer.");
    activation(d, "framebuffer"); owner_resources(d, "fbdev"); owner_resources(d, "boot-framebuffer");
    bool accelerated = false, accel_used = false; const char *accel_reason = 0;
    /* f1-97; f2-92: READY reads "accelerated" only once the engine has drawn
     * a presenter's request, "engine_ok_drawing_software" before. */
    if (savage2d_summary && savage2d_summary(&accelerated, &accel_used, &accel_reason))
        append(out, 0x30010, CIUKI_DEVICE_DISPLAY, CIUKI_BUS_LOADER, "2D accelerator", "savage2d",
               accelerated ? CIUKI_DEVICE_READY : CIUKI_DEVICE_QUARANTINED,
               !accelerated ? accel_reason : accel_used ? "accelerated" : "engine_ok_drawing_software");
    unsigned input_state = input.quarantined ? CIUKI_DEVICE_QUARANTINED :
                           !state.initialized || state.input_error || (!state.firmware && !input.active) ? CIUKI_DEVICE_ABSENT :
                           state.firmware ? CIUKI_DEVICE_FIRMWARE_FIRST : CIUKI_DEVICE_READY;
    for (unsigned k = 0; k < 3; k++) {
        d = append(out, 0x30001 + k, k == 0 ? CIUKI_DEVICE_SYSTEM : k == 1 ? CIUKI_DEVICE_KEYBOARD : CIUKI_DEVICE_MOUSE,
            CIUKI_BUS_LEGACY, k == 0 ? "8042 PS/2 controller" : k == 1 ? "PS/2 keyboard" : "PS/2 mouse",
            state.firmware ? "firmware-input" : "i8042", input_state, "Input backend.");
        activation(d, "input"); owner_resources(d, "i8042"); owner_resources(d, "firmware-input"); owner_resources(d, "input");
    }
    for (unsigned c = 0; c < ATA_CHANNELS; c++) {
        const char *owner = c ? "ata1" : "ata0";
        bool present = false;
        for (unsigned unit = 0; unit < ATA_DEVICES; unit++) if (ata_inventory_get(c, unit)) present = true;
        d = append(out, 0x40000 + c, CIUKI_DEVICE_SYSTEM, CIUKI_BUS_LEGACY,
            c ? "Secondary ATA channel" : "Primary ATA channel", "ata-pio",
            present ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT, "No identified units.");
        activation(d, "ata"); owner_resources(d, owner);
        for (unsigned unit = 0; unit < ATA_DEVICES; unit++) {
            const struct ata_device *disk = ata_inventory_get(c, unit);
            if (!disk) continue;
            char writes[48];
            bool setup = !disk->atapi && setup_mode && setup_mode();
            if (setup) klog("[setup] writes disk=%u count=%llu", c * 2 + unit, (unsigned long long)disk->write_commands);
            d = append(out, 0x40100 + c * 2 + unit, disk->atapi ? CIUKI_DEVICE_CDROM : CIUKI_DEVICE_DISK,
                CIUKI_BUS_LEGACY, disk->model, "ata-pio", disk->block.quarantined || disk->channel->quiescing ?
                CIUKI_DEVICE_QUARANTINED : CIUKI_DEVICE_READY, disk->atapi ?
                disk->media==ATAPI_NO_MEDIUM ? "no_medium_or_ejected" : disk->media==ATAPI_READY ? "media_ready" : "media_unavailable" :
                setup ? setup_writes(writes, disk->write_commands) : "Identified ATA/ATAPI unit.");
            if (d) { d->slot = c; d->function = unit; d->generation = disk->generation; }
            activation(d, "ata"); owner_resources(d, owner);
        }
    }
    if (audio.backend_name && equal(audio.backend_name,"sb16") && audio.backend_touched) {
        struct ciuki_device_record *d=append(out,0x50001,CIUKI_DEVICE_SOUND,CIUKI_BUS_LEGACY,
            "Creative Sound Blaster 16","sb16",audio.quarantined ? CIUKI_DEVICE_QUARANTINED :
            audio.ready ? CIUKI_DEVICE_READY : CIUKI_DEVICE_ABSENT,audio.reason);
        if (d) d->generation=audio.caps.generation;
        owner_resources(d,"sb16");
    }
    bool sound = false;
    for (unsigned k = 0; k < out->count; k++) if (out->devices[k].class_id == CIUKI_DEVICE_SOUND) sound = true;
    if (!sound) append(out, 0x50000, CIUKI_DEVICE_SOUND, CIUKI_BUS_LEGACY, "Audio output", audio.backend_name ? audio.backend_name : "ac97",
                       CIUKI_DEVICE_ABSENT, audio.reason ? audio.reason : "No supported audio device.");
    usb_devices(out);
    drivers(out);
}
static void hardware_snapshot(struct ciuki_hardware_info *out)
{
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_HARDWARE_VERSION; out->size = sizeof(*out);
    out->memmap_source = g_boot.memmap_source; out->memmap_count = g_boot.e820_count;
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    if (a >= 1) { cpuid(1, &a, &b, &c, &d); out->cpuid_ecx = c; out->cpuid_edx = d; }
    for (unsigned i = 0; i < g_boot.e820_count && i < ARRAY_SIZE(g_boot.e820); i++) {
        unsigned type = g_boot.e820[i].type;
        if (type > 5) type = 0;
        out->memory_bytes[type] += g_boot.e820[i].length;
    }
    struct thinkpad_ec_smbios identity;
    memset(&identity, 0, sizeof(identity));
    if (thinkpad_ec_smbios_snapshot) thinkpad_ec_smbios_snapshot(&identity);
    if (identity.valid) {
        out->smbios_valid = 1;
        string(out->vendor, sizeof(out->vendor), identity.vendor);
        string(out->product, sizeof(out->product), identity.product);
        string(out->bios_vendor, sizeof(out->bios_vendor), identity.bios_vendor);
        string(out->bios_version, sizeof(out->bios_version), identity.bios_version);
        string(out->bios_date, sizeof(out->bios_date), identity.bios_date);
    }
    string(out->build_id, sizeof(out->build_id), CIUKI_BUILD_ID);
    if (bootlog_report_snapshot) {
        size_t n=0;
        out->log_active=bootlog_report_snapshot(&out->log_position,&out->log_dropped,
            out->log_pending,sizeof(out->log_pending),&n);
        out->log_pending_bytes=(uint32_t)n;
        out->log_ring_start=BOOTLOG_PROTECTED;
        out->log_limit=BOOTLOG_LIMIT;
    }
}
int devices_query(uint32_t address, uint32_t bytes)
{
    if (bytes == sizeof(hardware_buffer)) {
        struct process *p = proc_current();
        if (!p || address > UINT32_MAX - bytes || !ua_range(p->memory, address, bytes, PROT_READ | PROT_WRITE)) return -EFAULT;
        uint32_t header[2];
        int error = copy_from_user(header, address, sizeof(header));
        if (error) return error;
        if (header[0] != CIUKI_HARDWARE_VERSION || header[1] != bytes) return -EINVAL;
        hardware_snapshot(&hardware_buffer);
        return copy_to_user(address, &hardware_buffer, sizeof(hardware_buffer));
    }
    if (bytes == sizeof(firmware_buffer)) {
        struct process *p = proc_current();
        if (!p || address > UINT32_MAX - bytes || !ua_range(p->memory, address, bytes, PROT_READ | PROT_WRITE)) return -EFAULT;
        if (!firmware_inventory_page) return -ENOSYS;
        int error = copy_from_user(&firmware_buffer, address, 5 * sizeof(uint32_t));
        if (!error) error = firmware_inventory_page(&firmware_buffer);
        return error ? error : copy_to_user(address, &firmware_buffer, sizeof(firmware_buffer));
    }
    if (bytes != sizeof(query_buffer)) return -EINVAL;
    struct process *p = proc_current();
    if (!p || address > UINT32_MAX - bytes || !ua_range(p->memory, address, bytes, PROT_READ | PROT_WRITE)) return -EFAULT;
    uint32_t header[2];
    int err = copy_from_user(header, address, sizeof(header));
    if (err) return err;
    if (header[0] != CIUKI_DEVICES_VERSION || header[1] != sizeof(query_buffer)) return -EINVAL;
    devices_snapshot(&query_buffer);
    /* An interrupt may quarantine a resource while formatting. Retry once
     * from retained state, then refuse an incoherent snapshot before copy. */
    if (registry_generation() != query_buffer.generation) devices_snapshot(&query_buffer);
    if (registry_generation() != query_buffer.generation) return -EAGAIN;
    return copy_to_user(address, &query_buffer, sizeof(query_buffer));
}
