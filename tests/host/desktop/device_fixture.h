/* Synthetic review snapshot, not a hardware qualification claim.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DEVICE_FIXTURE_H
#define CIUKI_DEVICE_FIXTURE_H
static void device_fixture(struct ciuki_devices_info *s)
{
    memset(s, 0, sizeof(*s)); s->version = CIUKI_DEVICES_VERSION; s->size = sizeof(*s); s->generation = 42;
    s->boot_flags = 2; s->input_policy = 1; s->uptime_ms = 7323000;
    s->cpu_family = 6; s->cpu_model = 11; s->cpu_stepping = 1;
    strcpy(s->cpu_vendor, "GenuineIntel"); strcpy(s->cpu_brand, "Intel Pentium III");
    strcpy(s->boot_options, "safe=0 serial=1 input=firmware");
    static const struct { unsigned cls, state; const char *name, *driver, *reason; } defs[] = {
        {0, 0, "8254 programmable interval timer", "pit", "Core timer"},
        {1, 0, "VBE framebuffer", "fbdev", "boot_console"},
        {2, 3, "PS/2 keyboard", "firmware-input", "Firmware-first input policy"},
        {3, 3, "PS/2 mouse", "firmware-input", "Firmware-first input policy"},
        {4, 0, "ATA disk (primary master)", "ata-pio", "native_discovery"},
        {5, 0, "ATAPI CD-ROM (secondary master)", "ata-pio", "native_discovery"},
        {6, 2, "PCI audio controller", "ac97", "IRQ lease quarantined: idle proof failed; retained until reboot."},
        {7, 3, "PCI Ethernet controller", "", "No native driver bound; firmware configuration retained."},
        {8, 0, "16550 serial port", "uart", "Serial diagnostics enabled"},
    };
    for (unsigned k = 0; k < sizeof(defs) / sizeof(defs[0]); k++) {
        struct ciuki_device_record *d = &s->devices[s->count++];
        d->id = k + 1; d->class_id = defs[k].cls; d->state = defs[k].state;
        strcpy(d->name, defs[k].name); strcpy(d->driver, defs[k].driver); strcpy(d->reason, defs[k].reason);
        d->generation = k + 10; d->activation_seq = k < 2 ? 2 : k < 4 ? 3 : 4;
        d->resource_count = 1; d->resources[0] = (struct ciuki_device_resource){0, 0, 0x60, 0x61};
    }
    struct ciuki_device_record *audio = &s->devices[6];
    audio->bus_type = 1; audio->slot = 5; audio->vendor = 0x8086; audio->device = 0x2415;
    audio->sub_vendor = 0x8086; audio->sub_device = 0x2415; audio->pci_class = 4; audio->subclass = 1;
    audio->resource_count = 3;
    audio->resources[0] = (struct ciuki_device_resource){0, 0, 0xc000, 0xc100};
    audio->resources[1] = (struct ciuki_device_resource){1, 0, 11, 12};
    audio->resources[2] = (struct ciuki_device_resource){3, 1, 0xfe000000, 0xfe000000};
    s->devices[7].bus_type = 1; s->devices[7].vendor = 0x10ec; s->devices[7].device = 0x8139;
    s->driver_count = 4;
    static const char *const names[] = {"fbdev", "firmware-input", "ata-pio", "ac97"};
    static const char *const policies[] = {"Loader-selected VBE framebuffer", "Sole firmware-first PS/2 lease", "Native ATA/ATAPI PIO", "Qualified Intel ICH AC97"};
    for (unsigned k = 0; k < 4; k++) {
        strcpy(s->drivers[k].name, names[k]); strcpy(s->drivers[k].build, "host-preview");
        strcpy(s->drivers[k].policy, policies[k]); s->drivers[k].bound_devices = k == 1 || k == 2 ? 2 : 1;
        s->drivers[k].state = k == 3 ? 2 : k == 1 ? 3 : 0;
    }
    s->lease_count = 4;
    for (unsigned k = 0; k < 4; k++) {
        struct ciuki_device_lease *l = &s->leases[k]; strcpy(l->owner, k < 2 ? "firmware-input" : k == 2 ? "boot-framebuffer" : "ac97");
        l->type = k == 2 ? 3 : 0; l->start = k == 2 ? 0xe0000000 : 0x60 + k * 4; l->end = l->start + (k == 2 ? 1024 * 768 * 4 : 1);
        l->generation = 10 + k; l->state = k == 3 ? 5 : k == 2 ? 0 : 2;
    }
}
#endif
