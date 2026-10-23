/* SPDX-License-Identifier: MIT */
#include "report.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char hr_summary[] =
    "CPU and memory map; SMBIOS and BIOS; PCI and USB; ACPI/PnP;\n"
    "display and modes; audio, network and storage; build identity;\n"
    "privacy-filtered boot log, recent disk records and pending kernel records.";
const char hr_privacy[] =
    "Saved locally only. No user files, network addresses, host names,\n"
    "serial numbers, volume labels or boot options. Log payloads are\n"
    "redacted except numeric hardware counters. Review the exact text below.";
struct text { char *p; size_t n, cap; int error; };
static void add(struct text *t, const char *fmt, ...)
{
    if (t->error) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(t->p + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= t->cap - t->n) { t->error = -ENOSPC; return; }
    t->n += (size_t)n;
}
static void value(struct text *t, const char *key, const char *s, size_t cap)
{
    char safe[256]; size_t i;
    if (cap > sizeof(safe)) cap = sizeof(safe);
    for (i = 0; i + 1 < cap && s[i]; i++) {
        unsigned c = (unsigned char)s[i]; safe[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    safe[i] = 0; add(t, "%s%s\n", key, safe[0] ? safe : "unavailable");
}
static void resources(struct text *t, const struct ciuki_device_resource *r, unsigned n)
{
    if (n > CIUKI_DEVICE_RESOURCES) { add(t,"  Invalid resource count\n"); return; }
    static const char *const kind[] = {"I/O", "IRQ", "DMA", "memory"};
    for (unsigned i=0; i<n; i++)
        add(t,"  %s base=0x%llx end-exclusive=0x%llx flags=0x%x%s\n",
            r[i].type<4 ? kind[r[i].type] : "unknown", (unsigned long long)r[i].start,
            (unsigned long long)r[i].end, r[i].flags,
            r[i].flags & CIUKI_RESOURCE_UNKNOWN_SIZE ? " (extent unknown)" : "");
}
static void binding(struct text *t,const struct ciuki_device_record *r)
{
    /* The live FAT quarantine reason can embed the triggering USER filename. */
    if (r->class_id==CIUKI_DEVICE_DISK && !strncmp(r->driver,"fat",3))
        add(t,"Binding: volume quarantined/read-only; file details excluded\n");
    else value(t,"Binding: ",r->reason,sizeof(r->reason));
}
/* Arbitrary log messages may contain paths, addresses and host names. Only
 * fixed known tags and numeric counters with fixed known keys cross this boundary.
 * Never keep a bare value or unknown key, even from an otherwise trusted tag. */
static void log_text(struct text *t, const char *log, size_t size, int error)
{
    static const char *const tags[]={"[cpu]","[pmm]","[vmm]","[smbios]","[pci]","[usb]",
        "[ata]","[ac97]","[audio]","[acpi]","[pnp]","[bootlog]","[kernel]","[accel]"};
    static const char *const keys[]={"count","bytes","pages","free","usable","reserved",
        "error","status","dropped","generation","irq","width","height","bpp","ms"};
    if (!log) { add(t,"Unavailable (error %d)\n",error); return; }
    unsigned hidden=0;
    for (size_t at=0; at<size;) {
        size_t end=at; while (end<size && log[end]!='\n') end++;
        size_t start=at; int found=-1;
        /* Serial/disk record framing can precede the bracketed event tag. */
        for (size_t k=at;k<end && k-at<96 && found<0;k++) if (log[k]=='[')
            for (unsigned j=0;j<sizeof(tags)/sizeof(*tags);j++) {
                size_t n=strlen(tags[j]);
                if (end-k>=n && !memcmp(log+k,tags[j],n)) { found=(int)j; start=k+n; break; }
            }
        if (found<0) hidden++;
        else {
            add(t,"%s",tags[found]);
            while (start<end) {
                while (start<end && log[start]==' ') start++;
                size_t next=start; while (next<end && log[next]!=' ') next++;
                for (unsigned j=0;j<sizeof(keys)/sizeof(*keys);j++) {
                    size_t n=strlen(keys[j]);
                    if (next-start<=n+1 || memcmp(log+start,keys[j],n) || log[start+n]!='=') continue;
                    size_t v=start+n+1, k=v;
                    if (log[k]=='-') k++;
                    for (;k<next && log[k]>='0' && log[k]<='9';k++) {}
                    if (k==next && k>v && next-v<=10) add(t," %s=%.*s",keys[j],(int)(next-v),log+v);
                }
                start=next;
            }
            add(t," [other payload redacted]\n");
        }
        at=end<size ? end+1 : end;
    }
    if (hidden) add(t,"%u other log records redacted\n",hidden);
}
int hr_filter_log(const char *log,size_t size,char *out,size_t cap,size_t *length)
{
    if (!log || !out || !cap || !length) return -EINVAL;
    *length=0; out[0]=0; struct text t={out,0,cap,0};
    log_text(&t,log,size,0);
    if (t.error) { out[0]=0; return t.error; }
    *length=t.n; return 0;
}
int hr_format(const struct hr_source *s, char *out, size_t cap, size_t *length)
{
    if (!s || !out || !cap || !length) return -EINVAL;
    *length=0; out[0]=0; struct text t={out,0,cap,0};
    const struct ciuki_devices_info *d=s->devices;
    if (d && (d->version!=1 || d->size!=sizeof(*d) || d->count>64 || d->driver_count>16)) return -EINVAL;
    add(&t,"CiukiOS Hardware Report\nCIUKIREP.TXT\n\nSections: %s\n\nPrivacy: %s\n\n[CPU]\n",hr_summary,hr_privacy);
    if (d) {
        value(&t,"Vendor: ",d->cpu_vendor,sizeof(d->cpu_vendor));
        value(&t,"Brand: ",d->cpu_brand,sizeof(d->cpu_brand));
        add(&t,"Family %u model %u stepping %u\n",d->cpu_family,d->cpu_model,d->cpu_stepping);
    } else add(&t,"Device inventory unavailable\n");
    const struct ciuki_hardware_info *h=s->hardware;
    if (h && (h->version!=CIUKI_HARDWARE_VERSION || h->size!=sizeof(*h) || h->log_pending_bytes>sizeof(h->log_pending))) return -EINVAL;
    if (h) add(&t,"CPUID.01 ECX=0x%08x EDX=0x%08x\n",h->cpuid_ecx,h->cpuid_edx);
    else add(&t,"CPUID feature flags unavailable\n");
    add(&t,"\n[Memory map]\n");
    if (h) {
        static const char *const names[]={"unknown","usable","reserved","ACPI reclaimable","ACPI NVS","bad"};
        add(&t,"Source %u ranges %u (boot map; not current free memory)\n",h->memmap_source,h->memmap_count);
        for (unsigned i=0;i<6;i++) add(&t,"%s bytes=%llu\n",names[i],(unsigned long long)h->memory_bytes[i]);
    } else add(&t,"Unavailable\n");
    add(&t,"\n[SMBIOS]\n");
    if (h && h->smbios_valid) {
        value(&t,"Vendor: ",h->vendor,sizeof(h->vendor)); value(&t,"Product: ",h->product,sizeof(h->product));
        value(&t,"BIOS vendor: ",h->bios_vendor,sizeof(h->bios_vendor));
        value(&t,"BIOS version: ",h->bios_version,sizeof(h->bios_version));
        value(&t,"BIOS date: ",h->bios_date,sizeof(h->bios_date));
    } else add(&t,"Unavailable or invalid firmware table\n");
    for (unsigned bus=CIUKI_BUS_PCI;bus<=CIUKI_BUS_USB;bus+=2) {
        add(&t,"\n[%s devices]\n",bus==CIUKI_BUS_PCI ? "PCI" : "USB");
        if (!d) { add(&t,"Unavailable\n"); continue; }
        add(&t,"Inventory generation %u flags=0x%x%s\n",d->generation,d->flags,
            d->flags & CIUKI_DEVICES_TRUNCATED ? " (truncated)" : "");
        unsigned count=0;
        for (unsigned i=0;i<d->count;i++) {
            const struct ciuki_device_record *r=&d->devices[i]; if (r->bus_type!=bus) continue;
            count++; add(&t,"id=%u %02x:%02x.%u %04x:%04x class=%02x:%02x:%02x state=%u flags=0x%x\n",
                r->id,r->bus,r->slot,r->function,r->vendor,r->device,r->pci_class,r->subclass,r->prog_if,r->state,r->flags);
            char name[128]={0};
            if (s->device_name && !s->device_name(s->ctx,bus,r->vendor,r->device,name,sizeof(name))) value(&t,"Name: ",name,sizeof(name));
            else value(&t,"Name: ",r->name,sizeof(r->name));
            value(&t,"Driver: ",r->driver,sizeof(r->driver)); binding(&t,r);
            resources(&t,r->resources,r->resource_count);
            if (bus==CIUKI_BUS_PCI && r->pci_class==1 && r->subclass==1)
                add(&t,"ATA chipset: primary=%s secondary=%s bus-master-capable=%u (capability, not active DMA)\n",
                    r->prog_if&1 ? "native" : "compatibility",r->prog_if&4 ? "native" : "compatibility",!!(r->prog_if&128));
        }
        if (!count) add(&t,"No devices reported\n");
    }
    add(&t,"\n[ACPI/PnP inventory]\n");
    if (!s->firmware) add(&t,"Unavailable\n");
    else {
        uint32_t cursor=0,generation=0; unsigned pages=0;
        do {
            struct ciuki_firmware_page p={.version=CIUKI_FIRMWARE_VERSION,.size=sizeof(p),.cursor=cursor,.generation=generation};
            int e=s->firmware(s->ctx,&p);
            if (e) { add(&t,"Unavailable/incomplete (error %d)\n",e); break; }
            if (p.version!=CIUKI_FIRMWARE_VERSION || p.size!=sizeof(p) || p.count>4 || p.cursor!=cursor ||
                (generation && p.generation!=generation) || (p.next && p.next<=cursor)) { add(&t,"Invalid/stale firmware page\n"); break; }
            generation=p.generation;
            add(&t,"Page cursor=%u total=%u generation=%u flags=0x%x status=%u SCI=%u\n",cursor,p.total,generation,p.flags,p.status,p.sci_enabled);
            for (unsigned i=0;i<p.count;i++) {
                const struct ciuki_firmware_record *r=&p.records[i];
                add(&t,"Device %u parent=%u origin=%u state=%u capabilities=0x%x flags=0x%x owner=%u lease=%u\n",
                    r->id,r->parent,r->origin,r->state,r->capabilities,r->flags,r->logical_owner,r->vm_lease);
                value(&t,"Path: ",r->path,sizeof(r->path)); value(&t,"HID: ",r->hid,sizeof(r->hid));
                if (r->cid_count>4 || r->resource_count>8) { add(&t,"Invalid firmware record\n"); continue; }
                for (unsigned j=0;j<r->cid_count;j++) value(&t,"CID: ",r->cid[j],sizeof(r->cid[j]));
                value(&t,"Binding: ",r->reason,sizeof(r->reason));
                for (unsigned j=0;j<r->resource_count;j++) {
                    struct ciuki_device_resource copy={r->resources[j].type,r->resources[j].flags,r->resources[j].start,r->resources[j].end};
                    resources(&t,&copy,1);
                }
            }
            cursor=p.next;
        } while (cursor && ++pages<256 && !t.error);
        if (cursor) add(&t,"Firmware inventory incomplete (page limit or query failure)\n");
    }
    add(&t,"\n[Display]\n");
    const struct ciuki_display_info *v=s->display;
    if (v && v->mode_count<=32) {
        value(&t,"Adapter: ",v->product,sizeof(v->product)); value(&t,"Vendor: ",v->vendor,sizeof(v->vendor));
        add(&t,"PCI %04x:%04x VBE=%x VRAM=%u current=%ux%ux%u\n",v->pci_vendor,v->pci_device,v->vbe_version,v->vram_bytes,v->width,v->height,v->bpp);
        for (unsigned i=0;i<v->mode_count;i++) add(&t,"Mode %04x %ux%ux%u pitch=%u linear=%u current=%u\n",
            v->modes[i].id,v->modes[i].width,v->modes[i].height,v->modes[i].bpp,v->modes[i].pitch,v->modes[i].linear,v->modes[i].current);
    } else add(&t,"Modes unavailable\n");
    if (s->engine) {
        add(&t,"2D engine driver=%u capabilities=0x%x quarantined=%u generation=%u\n",
            s->engine->driver,s->engine->capabilities,s->engine->quarantined,s->engine->generation);
        add(&t,"Engine: %s; operations=%llu fallbacks=%llu (active presenter path in display binding)\n",
            s->engine->driver==CIUKI_ACCEL_DRIVER_NONE || s->engine->quarantined || !s->engine->capabilities ? "software" :
            "hardware engine available",
            (unsigned long long)s->engine->operations,(unsigned long long)s->engine->fallbacks);
    }
    else add(&t,"2D engine query unavailable; see bindings\n");
    add(&t,"\n[Audio, network and storage bindings]\n");
    if (d) for (unsigned i=0;i<d->count;i++) {
        const struct ciuki_device_record *r=&d->devices[i];
        if (r->class_id!=CIUKI_DEVICE_SOUND && r->class_id!=CIUKI_DEVICE_NETWORK && r->class_id!=CIUKI_DEVICE_DISK &&
            r->class_id!=CIUKI_DEVICE_CDROM && r->class_id!=CIUKI_DEVICE_DISPLAY && strncmp(r->driver,"ata",3)) continue;
        add(&t,"Class %u state=%u\n",r->class_id,r->state);
        value(&t,"Device: ",r->name,sizeof(r->name)); value(&t,"Driver: ",r->driver,sizeof(r->driver));
        binding(&t,r); resources(&t,r->resources,r->resource_count);
    }
    if (s->network) {
        value(&t,"Network interface driver/type: ",s->network->driver,sizeof(s->network->driver));
        add(&t,"Network present=%u link=%u state=%u\n",s->network->present,s->network->link,s->network->state);
    } else add(&t,"Network link state unavailable\n");
    add(&t,"\n[Build identity]\n");
    if (h) value(&t,"Kernel build: ",h->build_id,sizeof(h->build_id));
    add(&t,"Image SHA-256: unavailable (release provenance not supplied)\nBuild manifest: unavailable (host-side only)\n");
    add(&t,"\n[Boot log - privacy filtered, first 64 KiB maximum]\n"); log_text(&t,s->boot_log,s->boot_length>HR_LOG_MAX ? HR_LOG_MAX : s->boot_length,s->boot_error);
    add(&t,"\n[Latest kernel records - privacy filtered disk ring, 64 KiB maximum]\n"); log_text(&t,s->latest_log,s->latest_length>HR_LOG_MAX ? HR_LOG_MAX : s->latest_length,s->latest_error);
    add(&t,"\n[Pending kernel records - privacy filtered RAM snapshot, 4 KiB maximum]\n");
    if (h) {
        add(&t,"Disk sink active=%u write-position=%u dropped-records=%u\n",h->log_active,h->log_position,h->log_dropped);
        log_text(&t,h->log_pending,h->log_pending_bytes,0);
        if (!h->log_pending_bytes) add(&t,"No pending records\n");
    } else add(&t,"Unavailable\n");
    if (t.error) { out[0]=0; return t.error; }
    *length=t.n; return 0;
}
static int save_file(const struct hr_io *io, const char *root, const void *text, size_t n, int logs)
{
    if (!io || !io->open || !io->write || !io->sync || !io->close || !io->rename || !io->remove || !io->present ||
        !root || !root[0] || !text || !n || n>(logs ? HR_FULL_LOG_MAX+1024u : HR_TEXT_MAX)) return -EINVAL;
    char temporary[128],final[128];
    int a=snprintf(temporary,sizeof(temporary),"%s/%s.TMP",root,logs ? "CIUKILOG" : "CIUKIREP");
    int b=snprintf(final,sizeof(final),"%s/%s",root,logs ? "CIUKILOG.GZ" : "CIUKIREP.TXT");
    if (a<0 || b<0 || a>=(int)sizeof(temporary) || b>=(int)sizeof(final)) return -ENAMETOOLONG;
    int e=io->present(io->ctx); if (e) return e;
    int fd=io->open(io->ctx,temporary); if (fd<0) return fd;
    size_t at=0;
    while (at<n) {
        ssize_t wrote=io->write(io->ctx,fd,(const char *)text+at,n-at);
        if (wrote==-EINTR) continue;
        if (wrote<=0 || (size_t)wrote>n-at) { e=wrote<0 ? (int)wrote : -EIO; break; }
        at+=(size_t)wrote;
    }
    if (!e) e=io->sync(io->ctx,fd);
    int close_error=io->close(io->ctx,fd); if (!e) e=close_error;
    if (!e) e=io->present(io->ctx);
    if (!e) e=io->rename(io->ctx,temporary,final);
    if (e) (void)io->remove(io->ctx,temporary);
    return e;
}
int hr_save(const struct hr_io *io,const char *root,const char *text,size_t n)
{ return save_file(io,root,text,n,0); }
int hr_save_logs(const struct hr_io *io,const char *root,const void *data,size_t n)
{ return save_file(io,root,data,n,1); }
unsigned hr_default_drive(uint32_t writable, uint32_t usb, int rescue)
{
    for (unsigned i=0;i<26;i++) if (writable & usb & (1u<<i)) return i;
    if (rescue) return 26;
    for (unsigned i=0;i<2;i++) if (writable & (1u<<i)) return i;
    for (unsigned i=2;i<26;i++) if (writable & (1u<<i)) return i;
    return 26;
}
