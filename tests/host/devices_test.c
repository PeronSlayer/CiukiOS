/* F2-41: production inventory and syscall marshalling, synthetic retained
 * hardware/registry state. Never device I/O or guest evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CIUKI_CPU_H
#include <stdint.h>
static uint32_t irq_save(void);
static void irq_restore(uint32_t);
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/process.h>
#include <ciuki/uaddr.h>
#include <ciuki/ata.h>
#include <ciuki/i8042.h>
#include <ciuki/fbdev.h>
#include <ciuki/ac97.h>
#include <ciuki/net.h>
#include <ciuki/usb.h>
static struct usb_host usb_context;
static unsigned usb_hosts;
unsigned usb_count(void) { return usb_hosts; }
const struct usb_host *usb_get(unsigned i) { return i < usb_hosts ? &usb_context : NULL; }
#include <ciuki/init.h>
static unsigned writes, copies, ranges, cpu_calls;
static uint32_t flags = 0x200;
static uint32_t irq_save(void) { uint32_t old = flags; flags = 0; return old; }
static void irq_restore(uint32_t f) { flags = f; }
static uint32_t read_eflags(void) { return flags; }
static void outb(uint16_t p, uint8_t v) { (void)p; (void)v; writes++; }
static void outl(uint16_t p, uint32_t v) { (void)p; (void)v; writes++; }
static uint32_t inl(uint16_t p) { (void)p; assert(!"unexpected PCI read"); return 0; }
static int extended;
static void (*cpu_hook)(void);
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    if (cpu_hook) cpu_hook();
    cpu_calls++; *a = *b = *c = *d = 0;
    if (leaf == 0) { *a = 1; memcpy(b, "Genu", 4); memcpy(d, "ineI", 4); memcpy(c, "ntel", 4); }
    else if (leaf == 1) *a = 0x6b1;
    else if (leaf == 0x80000000u) *a = extended ? 0x80000004u : 0x80000000u;
    else {
        assert(extended && leaf >= 0x80000002u && leaf <= 0x80000004u);
        const char brand[48] = "Synthetic Pentium III CPU"; unsigned off = (leaf - 0x80000002u) * 16;
        memcpy(a, brand + off, 4); memcpy(b, brand + off + 4, 4); memcpy(c, brand + off + 8, 4); memcpy(d, brand + off + 12, 4);
    }
}
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
struct ac97_device ac97;
struct audio_state audio;
static struct netif nic_contexts[NETIF_MAX];
#define nic_context nic_contexts[0]
static unsigned nic_count;
unsigned net_count(void) { return nic_count; }
struct netif *net_get(unsigned i) { return i < nic_count ? &nic_contexts[i] : NULL; }
bool gen_matches(gen_t a, gen_t b) { return a && a == b; }
void klog(const char *s, ...) { (void)s; }
#include "../../src/kernel/core/registry.c"

static struct drivers_state driver_state;
static struct i8042_stats input_state;
static struct fb_device framebuffer;
static struct ata_channel channel;
static struct ata_device disk;
static struct activation_entry ledger[] = {
    {.kind = ACTIVATION_DEVICE, .device = "framebuffer", .seq = 2, .reason = "boot_console"},
    {.kind = ACTIVATION_DEVICE, .device = "input", .seq = 3, .reason = "firmware"},
    {.kind = ACTIVATION_DEVICE, .device = "ata", .seq = 4, .reason = "native_discovery"},
};
void drivers_snapshot(struct drivers_state *out) { *out = driver_state; }
unsigned drivers_activation_count(void) { return 3; }
const struct activation_entry *drivers_activation_get(unsigned i) { return i < 3 ? &ledger[i] : NULL; }
void i8042_snapshot(struct i8042_stats *out) { *out = input_state; }
const struct fb_device *fbdev_get(void) { return &framebuffer; }
const struct ata_device *ata_inventory_get(unsigned c, unsigned u) { return c == 0 && u == 0 && disk.identified ? &disk : NULL; }
static struct process process;
struct process *proc_current(void) { return &process; }
static struct ciuki_devices_info user;
static int accessible = 1, copy_error;
bool ua_range(const struct uaddr *u, uint32_t a, uint32_t n, uint32_t prot)
{ (void)u; ranges++; assert(n == sizeof(user) && prot == (PROT_READ | PROT_WRITE)); return accessible && a == 0x1000; }
int copy_from_user(void *out, uint32_t a, uint32_t n)
{ assert(a == 0x1000 && n == 8); memcpy(out, &user, n); return 0; }
int copy_to_user(uint32_t a, const void *in, uint32_t n)
{ assert(a == 0x1000 && n == sizeof(user)); copies++; if (copy_error) return copy_error; memcpy(&user, in, n); return 0; }
#include "../../src/kernel/core/devices.c"

static unsigned changes;
static void change_generation(void)
{
    if (changes) { inventory_generation++; changes--; }
}
static struct ciuki_devices_info snapshot;
static const struct ciuki_device_record *device(unsigned id)
{
    for (unsigned i = 0; i < snapshot.count; i++) if (snapshot.devices[i].id == id) return &snapshot.devices[i];
    assert(!"missing device"); return NULL;
}
static void second_nic_catalogue(void)
{
    static const struct netif_driver new_drivers[] = {
        {.name="e1000"}, {.name="pcnet"}, {.name="tulip"}, {.name="3c905"}, {.name="rhine"}
    };
    struct pci_func saved[2] = {pci[0],pci[1]};
    for (unsigned n = 0; n < 5; n++) {
        nic_context.driver = &new_drivers[n]; nic_context.pci = pci[0];
        nic_context.ready = nic_context.running = true; nic_context.quarantined = false;
        nic_context.reason = "ready"; nic_count = 1;
        devices_snapshot(&snapshot);
        assert(snapshot.driver_count == CIUKI_DRIVERS_MAX);
        assert(!strcmp(device(0x10018)->driver, new_drivers[n].name));
        { bool seen = false; /* the new NIC takes the first unbound NIC row (usb-host now owns row 15) */
          for (unsigned j = 12; j < snapshot.driver_count; j++) if (!strcmp(snapshot.drivers[j].name, new_drivers[n].name)) { seen = true; assert(snapshot.drivers[j].bound_devices == 1); }
          assert(seen); }
    }
    /* Four new bindings must fit the existing sixteen-row public ABI;
     * replacing unbound catalogue rows must not hide active NICs. */
    npci = nic_count = 4;
    for (unsigned i = 0; i < 4; i++) {
        pci[i] = saved[0]; pci[i].dev = 3 + i;
        nic_contexts[i] = (struct netif){.pci=pci[i],.driver=&new_drivers[i],.ready=true,.running=true,.reason="ready"};
    }
    devices_snapshot(&snapshot); assert(snapshot.driver_count == CIUKI_DRIVERS_MAX && !(snapshot.flags & CIUKI_DEVICES_TRUNCATED));
    for (unsigned i = 0; i < 4; i++) {
        bool found = false;
        for (unsigned j = 0; j < snapshot.driver_count; j++) if (!strcmp(snapshot.drivers[j].name, new_drivers[i].name)) {
            found = true; assert(snapshot.drivers[j].bound_devices == 1);
        }
        assert(found);
    }
    pci[0] = saved[0]; pci[1] = saved[1]; npci = 2; nic_count = 0;
}
int main(void)
{
    g_ticks = 123456; g_boot.flags = CBI_F_SERIAL_LOG; g_boot.input_policy = CBI_INPUT_FIRMWARE;
    strcpy(g_boot.options, "safe=0 serial=1 input=firmware");
    driver_state.initialized = true; driver_state.firmware = true; framebuffer.present = true;
    disk.identified = true; disk.block.quarantined = true; disk.channel = &channel; disk.generation = 27; strcpy(disk.model, "Retained_disk");
    nres = 4; npci = 2; inventory_generation = 5;
    res[0] = (struct resource){RES_PORT, 0x60, 0x61, "firmware-input", 11, RS_ACTIVE, false};
    res[1] = (struct resource){RES_PORT, 0x64, 0x65, "firmware-input", 12, RS_ACTIVE, false};
    res[2] = (struct resource){RES_PORT, 0x1f0, 0x1f8, "ata0", 27, RS_QUARANTINED, false};
    res[3] = (struct resource){RES_IRQ, 14, 15, "old", 28, RS_RELEASED, false};
    pci[0] = (struct pci_func){.vendor = 0x10ec, .device = 0x8139, .class_code = 2, .dev = 3, .irq_line = 11, .irq_pin = 1, .bar = {0xc001}};
    pci[1] = (struct pci_func){.vendor = 0xabcd, .device = 1, .class_code = 3, .dev = 4, .bar = {0x12345004, 1}};
    memset(&snapshot, 0xa5, sizeof(snapshot)); devices_snapshot(&snapshot);
    assert(snapshot.version == 1 && snapshot.size == 34696 && snapshot.generation == 5 && snapshot.lease_count == 3);
    assert(snapshot.boot_flags == CBI_F_SERIAL_LOG && snapshot.uptime_ms == 123456);
    assert(!strcmp(snapshot.cpu_vendor, "GenuineIntel") && !snapshot.cpu_brand[0]);
    assert(snapshot.cpu_family == 6 && snapshot.cpu_model == 11 && snapshot.cpu_stepping == 1 && cpu_calls == 3);
    const struct ciuki_device_record *nic = device(0x10018);
    assert(nic->class_id == CIUKI_DEVICE_NETWORK && nic->vendor == 0x10ec && nic->device == 0x8139 && !nic->driver[0]);
    assert(nic->state == CIUKI_DEVICE_FIRMWARE_FIRST && nic->resource_count == 2);
    assert(nic->resources[0].start == 0xc000 && nic->resources[0].end == 0xc000 && nic->resources[0].flags == CIUKI_RESOURCE_UNKNOWN_SIZE);
    assert(device(0x10020)->resources[0].start == 0x112345000ull);
    assert(device(0x30002)->state == CIUKI_DEVICE_FIRMWARE_FIRST && device(0x30002)->activation_seq == 3);
    assert(device(0x30002)->generation == 12 && device(0x30002)->resource_count == 2);
    assert(device(0x40100)->state == CIUKI_DEVICE_QUARANTINED && !strcmp(device(0x40100)->name, "Retained_disk"));
    assert(snapshot.drivers[9].bound_devices == 3 && snapshot.drivers[10].state == CIUKI_DEVICE_QUARANTINED);
    assert(!writes && flags == 0x200);
    for (unsigned i = snapshot.count; i < 64; i++) assert(!snapshot.devices[i].id && !snapshot.devices[i].name[0]);
    static const struct netif_driver nic_driver = {.name = "rtl8139"};
    nic_context = (struct netif){.pci = pci[0], .driver = &nic_driver, .ready = true,
        .running = true, .generation = 31, .reason = "ready"};
    strcpy(nic_context.owner, "net-31"); nic_count = 1;
    res[nres++] = (struct resource){RES_PORT, 0xc000, 0xc100, "net-31", 31, RS_ACTIVE, false};
    devices_snapshot(&snapshot); nic = device(0x10018);
    assert(nic->state == CIUKI_DEVICE_READY && !strcmp(nic->driver, "rtl8139") && nic->generation == 31);
    assert(nic->resources[0].end == 0xc100 && !nic->resources[0].flags);
    assert(snapshot.driver_count == 16 && snapshot.drivers[13].bound_devices == 1);
    for (unsigned i = 0; i < snapshot.count; i++) assert(strcmp(snapshot.devices[i].name, "net-31"));
    nic_context.quarantined = true; nic_context.ready = nic_context.running = false; nic_context.reason = "tx_timeout";
    devices_snapshot(&snapshot); nic = device(0x10018);
    assert(nic->state == CIUKI_DEVICE_QUARANTINED && !strcmp(nic->reason, "tx_timeout"));
    assert(snapshot.drivers[13].state == CIUKI_DEVICE_QUARANTINED);
    nic_context.quarantined = false; nic_context.reason = "invalid_bar";
    devices_snapshot(&snapshot); assert(device(0x10018)->state == CIUKI_DEVICE_ABSENT);
    nic_context.pci.fn = 1;
    devices_snapshot(&snapshot); assert(!device(0x10018)->driver[0]);
    nic_count = 0;
    usb_hosts = 1;
    usb_context.pci = pci[0]; usb_context.ready = true; usb_context.generation = 41;
    usb_context.reason = "ready_polling"; strcpy(usb_context.owner, "usb-41");
    usb_context.device_count = 1;
    usb_context.devices[0] = (struct usb_device){.address = 2, .vendor = 0x1234, .product = 1,
        .configured = true, .present = true, .generation = 42, .reason = "hid_ready"};
    usb_context.devices[0].hid[0] = (struct usb_hid){.endpoint = 1, .protocol = 1};
    pci[0].class_code = 0x0c; pci[0].subclass = 3;
    res[nres++] = (struct resource){RES_PORT, 0xc000, 0xc020, "usb-41", 41, RS_ACTIVE, false};
    devices_snapshot(&snapshot);
    assert(!strcmp(device(0x10018)->driver, "usb-host") && device(0x10018)->state == CIUKI_DEVICE_READY);
    assert(device(0x60000)->bus_type == CIUKI_BUS_USB && device(0x60000)->vendor == 0x1234 && device(0x60000)->generation == 42);
    usb_context.devices[0].quarantined = true; usb_context.devices[0].reason = "hid_stall";
    devices_snapshot(&snapshot); assert(device(0x60000)->state == CIUKI_DEVICE_QUARANTINED && !strcmp(device(0x60000)->reason, "hid_stall"));
    usb_hosts = 0; pci[0].class_code = 2; pci[0].subclass = 0; /* pci[0] is the NIC again for the later catalogue checks */
    pci[2]=(struct pci_func){.vendor=0x1274,.device=0x5000,.class_code=4,.subclass=1,.dev=5,.irq_line=11,.irq_pin=1,.bar={0xd001}};
    npci=3; audio.backend_pci=pci[2]; audio.backend_name="es137x"; audio.backend_touched=true; audio.ready=true;
    audio.reason="ready"; audio.caps.generation=41;
    res[nres++]=(struct resource){RES_PORT,0xd000,0xd040,"es137x",41,RS_ACTIVE,false};
    devices_snapshot(&snapshot);
    assert(device(0x10028)->class_id==CIUKI_DEVICE_SOUND && !strcmp(device(0x10028)->driver,"es137x"));
    assert(device(0x10028)->generation==41 && device(0x10028)->state==CIUKI_DEVICE_READY && device(0x10028)->resources[0].end==0xd040);
    assert(snapshot.driver_count==16 && !strcmp(snapshot.drivers[11].name,"es137x") && snapshot.drivers[11].bound_devices==1);
    for (unsigned i=0;i<snapshot.count;i++) assert(strcmp(snapshot.devices[i].name,"es137x"));
    audio.ready=false; audio.quarantined=true; audio.reason="stop_unproved";
    devices_snapshot(&snapshot); assert(device(0x10028)->state==CIUKI_DEVICE_QUARANTINED && snapshot.drivers[11].state==CIUKI_DEVICE_QUARANTINED);
    audio.backend_pci=(struct pci_func){0}; audio.backend_name="sb16"; audio.ready=true; audio.quarantined=false;
    devices_snapshot(&snapshot); assert(device(0x50001)->bus_type==CIUKI_BUS_LEGACY && !strcmp(device(0x50001)->name,"Creative Sound Blaster 16"));
    assert(!strcmp(snapshot.drivers[11].name,"sb16") && snapshot.drivers[11].bound_devices==1);
    memset(&audio,0,sizeof(audio)); npci=2; nres--;
    second_nic_catalogue();
    extended = 1; devices_snapshot(&snapshot); assert(!strcmp(snapshot.cpu_brand, "Synthetic Pentium III CPU"));
    /* Count cap and zeroed tail: more unique owners/PCI than device slots. */
    static char owners[64][16]; nres = 64; npci = 32;
    for (unsigned i = 0; i < 64; i++) { snprintf(owners[i], sizeof(owners[i]), "owner-%u", i); res[i] = (struct resource){RES_PORT, i * 2, i * 2 + 1, owners[i], i + 1, RS_CLAIMED, false}; }
    for (unsigned i = 0; i < 32; i++) pci[i] = (struct pci_func){.vendor = 1, .device = 1, .dev = i};
    devices_snapshot(&snapshot); assert(snapshot.count == 64 && snapshot.lease_count == 64 && snapshot.flags & CIUKI_DEVICES_TRUNCATED);
    assert(!registry_activate(0, 1)); devices_snapshot(&snapshot); assert(snapshot.generation == 6);
    /* Validate arguments/full writable extent before changing any user byte. */
    user.version = 1; user.size = sizeof(user); unsigned c0 = copies, r0 = ranges;
    assert(devices_query(0x1000, sizeof(user) - 1) == -EINVAL && copies == c0 && ranges == r0);
    assert(devices_query(UINT32_MAX - 10, sizeof(user)) == -EFAULT && copies == c0);
    accessible = 0; assert(devices_query(0x1000, sizeof(user)) == -EFAULT && copies == c0); accessible = 1;
    user.version = 2; assert(devices_query(0x1000, sizeof(user)) == -EINVAL && copies == c0);
    user.version = 1; user.size = 8; assert(devices_query(0x1000, sizeof(user)) == -EINVAL && copies == c0);
    user.size = sizeof(user); assert(!devices_query(0x1000, sizeof(user)) && copies == c0 + 1 && user.count == 64);
    copy_error = -EFAULT; assert(devices_query(0x1000, sizeof(user)) == -EFAULT);
    copy_error = 0; cpu_hook = change_generation; changes = 1;
    assert(!devices_query(0x1000, sizeof(user)) && !changes && user.generation == registry_generation());
    changes = 100; c0 = copies;
    assert(devices_query(0x1000, sizeof(user)) == -EAGAIN && copies == c0);
    assert(!writes);
    puts("PASS devices: retained registry, PCI/64-bit BARs, NIC binding/readiness/quarantine/failure, quarantine, firmware leases, CPUID, zeroed marshalling and bounds");
    return 0;
}
