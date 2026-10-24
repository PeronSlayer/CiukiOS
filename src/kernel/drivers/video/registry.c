#include <ciuki/system_settings.h>
/* Read-only PCI nomination, lifecycle metadata and policy.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>
extern void console_splash_accel(bool,bool,const char *) __attribute__((weak));
extern const struct display_driver mach64_driver; /* declarative registration */
extern const struct display_driver rage128_driver; /* declarative registration */
extern const struct display_driver radeon2d_driver; /* declarative registration */
extern const struct display_driver sis300_driver; /* declarative registration */
extern const struct display_driver sis315_driver; /* declarative registration */
extern const struct display_driver cirrus_driver; /* declarative registration */
extern const struct display_driver trident_blade_driver; /* declarative registration */
extern const struct display_driver chipshiqv_driver; /* declarative registration */
extern const struct display_driver matrox_driver; /* declarative registration */
extern const struct display_driver nv2d_driver; /* declarative registration */
extern const struct display_driver riva128_driver; /* declarative registration */
extern const struct display_driver neomagic_driver; /* declarative registration */
extern const struct display_driver unichrome_driver; /* declarative registration */
extern const struct display_driver tdfx_driver; /* declarative registration */
extern const struct display_driver vmwaresvga_driver; /* declarative registration */
extern const struct display_driver s3savage_driver; /* declarative registration */
extern const struct display_driver s3trio64_driver; /* declarative registration */
extern const struct display_driver s3virge_driver; /* declarative registration */
extern const struct display_driver i740_driver; /* declarative registration */
extern const struct display_driver i810_driver; /* declarative registration */
extern const struct display_driver intel2d_gen2_driver; /* declarative registration */
extern const struct display_driver intel2d_gen3_driver; /* declarative registration */
/* Exact native matches have equal specificity; ties use PCI vendor/family order. */
static const struct display_driver *const drivers[] = {
    &mach64_driver,
    &rage128_driver,
    &radeon2d_driver,
    &cirrus_driver,
    &trident_blade_driver,
    &matrox_driver,
    &chipshiqv_driver,
    &sis300_driver,
    &sis315_driver,
    &neomagic_driver,
    &nv2d_driver,
    &unichrome_driver,
    &tdfx_driver,
    &riva128_driver,
    &vmwaresvga_driver,
    &s3savage_driver,
    &s3trio64_driver,
    &s3virge_driver,
    &i740_driver,
    &i810_driver,
    &intel2d_gen2_driver,
    &intel2d_gen3_driver,
};
static const struct display_driver *bound;
static const struct pci_func *owner;
static const char *software_reason = "pending";
static struct ciuki_accel_info cached_snapshot;
static void driver_text(char *out, unsigned size, const char *in)
{
    unsigned i=0;
    if (in) for (; i+1<size && in[i]; i++) out[i]=in[i];
    out[i]=0;
}
static uint64_t native_count[CIUKI_ENGINE_OPERATIONS], software_count[CIUKI_ENGINE_OPERATIONS];
static uint64_t command_native_count[14], command_software_count[14];

bool display_accel_disabled(const char **reason)
{
    if (system_performance && !system_performance()->graphics) { if (reason) *reason = "performance_setting"; return true; }
    if (!system_admit(CIUKI_DEVICE_ACCELERATOR_ID, NULL)) { if (reason) *reason = "hardware_profile"; return true; }
    if (g_boot.flags & CBI_F_SAFE_MODE) { if (reason) *reason = "safe_mode"; return true; }
    /* G14: selective startup, Step-by-step or the ASD agent (bootstat.h
     * class 0, boot flag bit 24). */
    if (g_boot.flags & (1u << CBI_F_SKIP_SHIFT)) { if (reason) *reason = "startup_choice"; return true; }
    for (unsigned i = 0; i < sizeof(g_boot.options) && g_boot.options[i];) {
        unsigned first = i;
        while (i < sizeof(g_boot.options) && g_boot.options[i] &&
               g_boot.options[i] != ' ' && g_boot.options[i] != '\r' && g_boot.options[i] != '\n') i++;
        if (i-first == 7 && !memcmp(g_boot.options+first,"accel=0",7)) {
            if (reason) *reason = "boot_disabled";
            return true;
        }
        if (i < sizeof(g_boot.options) && g_boot.options[i]) i++;
    }
    return false;
}
static bool match(const struct display_driver *d, const struct pci_func *p)
{
    for (unsigned i = 0; i < d->match_count; i++) {
        const struct display_match *m = &d->matches[i];
        if (p->vendor == m->vendor && p->device == m->device &&
            (p->class_code & m->class_mask) == m->class_code) return true;
    }
    return false;
}
static bool owns(const struct display_driver *d, const struct pci_func *p, const struct fb_device *fb)
{
    unsigned bar = d->framebuffer_bar_for ? d->framebuffer_bar_for(p) : d->framebuffer_bar;
    /* Firmware-configured 32-bit memory BAR plus the validated VBE extent.
     * No destructive PCI BAR sizing, decode changes or resource claims.
     * A BAR base alone is insufficient: reject RAM aliases, overflow,
     * truncated firmware VRAM and any second display function. */
    if (!fb || !fb->present || !fb->size || !fb->pitch ||
        (p->header_type & 0x7f) || p->class_code != 3 || !(p->command & 2u) ||
        bar >= 6 || (p->bar[bar] & 7u) ||
        (p->bar[bar] & ~15u) != fb->phys || fb->phys < 0x100000 ||
        fb->size > UINT32_MAX-fb->phys || (uint64_t)fb->pitch*fb->height > fb->size ||
        !g_boot.display.vram_bytes || fb->size > g_boot.display.vram_bytes) return false;
    /* Tie the PCI BAR to the complete active framebuffer access lease,
     * not just to a caller-supplied address or an unknown-size PCI BAR. */
    if (fb->handle<0 || !registry_valid(fb->handle,fb->generation)) return false;
    const struct resource *lease=registry_get((unsigned)fb->handle);
    if (!lease || lease->type!=RES_MMIO || lease->state!=RS_ACTIVE ||
        lease->start!=fb->phys || lease->end!=fb->phys+fb->size ||
        !lease->owner || strncmp(lease->owner,"fbdev",sizeof("fbdev"))) return false;
    uint64_t end = (uint64_t)fb->phys + fb->size;
    if (fb->phys < g_boot.kernel_end && end > g_boot.kernel_start) return false;
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type == CBI_E820_RAM && fb->phys < e->base+e->length && end > e->base) return false;
    }
    return true;
}
const struct display_driver *display_driver_select(const struct fb_device *fb,
    const struct display_driver *const *table, unsigned count, const struct pci_func **found)
{
    if (found) *found = 0;
    const struct pci_func *p = 0;
    unsigned displays = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *candidate = pci_get(i);
        if (candidate && candidate->class_code == 3) { p = candidate; displays++; }
    }
    if (displays != 1) return 0;
    for (unsigned i = 0; i < count; i++) {
        const struct display_driver *d = table[i];
        if (d && system_admit(CIUKI_DEVICE_ACCELERATOR_ID, d->id) && system_admit(system_pci_id(p->bus, p->dev, p->fn), d->id) && match(d,p) && owns(d,p,fb) && (!d->probe || d->probe(p))) {
            if (found) *found = p;
            return d;
        }
    }
    return 0;
}
const struct display_driver *display_driver_bind(const struct fb_device *fb)
{
    const struct display_driver *next=display_driver_select(fb, drivers, ARRAY_SIZE(drivers), &owner);
    if (next!=bound) memset(&cached_snapshot,0,sizeof(cached_snapshot));
    bound=next;
    return bound;
}
const struct display_driver *display_driver_bound(void) { return bound; }
const struct pci_func *display_driver_owner(void) { return owner; }
int display_driver_mode_bind(const struct fb_device *fb,const struct display_mode_provider **out)
{
    if (!out) return -EINVAL;
    *out=0;
    const struct pci_func *p=0;
    const struct display_driver *d=display_driver_select(fb,drivers,ARRAY_SIZE(drivers),&p);
    if (d && d->modes && d->mode_adapter && d->mode_adapter(p)) {
        *out=d->modes; return 0;
    }
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *candidate=pci_get(i);
        if (!candidate) continue;
        for (unsigned n=0;n<ARRAY_SIZE(drivers);n++) {
            d=drivers[n];
            if (d->modes && d->mode_adapter && match(d,candidate) && d->mode_adapter(candidate)) return -EIO;
        }
    }
    return 0;
}
const struct display_mode_provider *display_driver_modes(const struct fb_device *fb)
{
    const struct display_mode_provider *p=0;
    return display_driver_mode_bind(fb,&p) ? 0 : p;
}

void display_driver_software(const char *reason)
{
    software_reason=reason ? reason : "pending";
    if (reason && console_splash_accel) console_splash_accel(false,false,reason);
}
void display_driver_record(void) { if (bound && bound->record) bound->record(); }
void display_driver_count(unsigned operation, bool native)
{
    if (operation < CIUKI_ENGINE_OPERATIONS) (native ? native_count : software_count)[operation]++;
}
void display_driver_command_count(unsigned opcode, bool native)
{
    if (opcode>=1 && opcode<=14) (native ? command_native_count : command_software_count)[opcode-1]++;
}
void display_driver_command_counters(struct ciuki_command_info *out)
{
    memcpy(out->native,command_native_count,sizeof(command_native_count));
    memcpy(out->software,command_software_count,sizeof(command_software_count));
}
bool display_driver_summary(bool *ready, bool *used, const char **reason)
{
    *ready = *used = false; *reason = software_reason;
    if (display_accel_disabled(reason)) return true;
    if (bound && bound->summary) (void)bound->summary(ready,used,reason);
    if (*reason && !strncmp(*reason,"pending",sizeof("pending"))) *reason=software_reason;
    /* DM8: a retained attempt is the registry's own status. The driver never
     * started this boot, so its reason cannot override it. */
    if (!strncmp(software_reason,DISPLAY_ATTEMPT_RETAINED,sizeof(DISPLAY_ATTEMPT_RETAINED))) {
        *ready=false; *reason=software_reason;
    }
    /* Driver setup and diagnostic draws do not establish presenter usage. */
    *used = false;
    for (unsigned i=0;i<CIUKI_ENGINE_OPERATIONS;i++) if (native_count[i]) *used=true;
    for (unsigned i=0;i<9;i++) if (command_native_count[i]) *used=true;
    return true;
}
bool display_driver_refresh(void)
{
    memset(&cached_snapshot,0,sizeof(cached_snapshot));
    if (bound && bound->snapshot) bound->snapshot(&cached_snapshot);
    return cached_snapshot.quarantined!=0;
}
void display_driver_info(struct ciuki_display_engine_info *out)
{
    memset(out,0,sizeof(*out));
    bool ready, used; const char *reason;
    display_driver_summary(&ready,&used,&reason);
    out->version = CIUKI_DISPLAY_ENGINE_VERSION; out->size = sizeof(*out);
    out->generation=fbdev_get()->generation;
    out->capabilities=fbdev_native_capabilities();
    if (ready && !(out->capabilities&1u)) reason="pending";
    ready=ready && (out->capabilities&1u);
    out->ready = ready; out->native_used = used;
    out->kind = ready ? CIUKI_ENGINE_NATIVE : fbdev_get()->present ? CIUKI_ENGINE_SOFTWARE : CIUKI_ENGINE_NONE;
    out->software_operations = CIUKI_ENGINE_SOFTWARE_OPERATIONS;
    out->cpu_ready = fbdev_cpu_ready();
    driver_text(out->driver_id,sizeof(out->driver_id),bound ? bound->id : "swdib");
    driver_text(out->display_name,sizeof(out->display_name),bound ? bound->name : "Software renderer");
    driver_text(out->reason,sizeof(out->reason),reason ? reason : "unknown");
    if (owner) { out->pci_vendor = owner->vendor; out->pci_device = owner->device; }
    out->quarantined=cached_snapshot.quarantined;
    memcpy(out->native,native_count,sizeof(native_count));
    memcpy(out->software,software_count,sizeof(software_count));
}
