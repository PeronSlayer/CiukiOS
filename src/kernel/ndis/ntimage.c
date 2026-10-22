/* SPDX-License-Identifier: GPL-2.0-only */
#include "ntimage.h"
#include "scan.h"
static int refuse(struct ntimage *d,const char *why) {
    pe_fail(&d->pe,why); return 0;
}
bool ntimage_executable(const struct ntimage *d,uint32_t address) {
    const struct pe_image *p=&d->pe;
    if(address<p->base || address-p->base>=p->size) return false;
    uint32_t rva=address-p->base;
    for(unsigned i=0;i<p->section_count;i++) {
        const struct pe_section *s=&p->sections[i];
        if((s->flags&0x20000000) && rva>=s->rva && rva-s->rva<s->span) return true;
    }
    return false;
}
static int policy(void *ctx,struct pe_image *p) {
    struct ntimage *d=ctx;
    if(p->subsystem!=1) return refuse(d,"kernel-requires-native-subsystem");
    if(!p->entry || !ntimage_executable(d,p->base+p->entry)) return refuse(d,"driver-entry-not-executable");
    if(p->dirs[9].size) return refuse(d,"driver-tls-unsupported");
    if(p->dirs[13].size) return refuse(d,"driver-delay-imports-unsupported");
    if(!d->platform.image_budget || d->platform.image_budget>NDIS_IMAGE_LIMIT ||
       p->size>d->platform.image_budget) return refuse(d,"network-image-budget");
    for(unsigned i=0;i<p->section_count;i++) {
        struct pe_section *s=&p->sections[i];
        if(s->raw && (s->offset>p->file_size || s->raw>p->file_size-s->offset))
            return refuse(d,"driver-section-truncated");
        if(s->raw && s->offset<p->headers) return refuse(d,"driver-raw-header-overlap");
        for(unsigned j=0;j<i;j++) {
            const struct pe_section *a=&p->sections[j];
            if(s->raw && a->raw && s->offset<a->offset+a->raw && a->offset<s->offset+s->raw)
                return refuse(d,"driver-raw-overlap");
        }
    }
    return 1;
}
static void *allocate(void *ctx,uint32_t preferred,uint32_t size,uint32_t *base) {
    struct ntimage *d=ctx; const struct pe_ops *m=&d->platform.memory;
    void *v=m->allocate(m->context,preferred,size,base);
    if(v && (!d->platform.resident(m->context,v,size,*base) || *base<0xc0000000u || (*base&4095))) {
        m->release(m->context,v,size); refuse(d,"image-not-supervisor-pinned"); return 0;
    }
    return v;
}
static void release(void *ctx,void *v,uint32_t n) {
    struct ntimage *d=ctx; const struct pe_ops *m=&d->platform.memory;
    m->release(m->context,v,n);
}
static int module(void *ctx,const struct pe_import *i) {
    struct ntimage *d=ctx;
    for(size_t n=0;n<d->platform.export_count;n++) {
        const struct ndis_export *e=&d->platform.exports[n];
        if(ndis_export_find(e,1,i->module,e->name,0)) return 1;
    }
    if(d->platform.record) d->platform.record(d->platform.memory.context,d->file,i->module,
                                              "<module>",0,"unknown-module");
    return refuse(d,"unknown-module");
}
static int symbol(void *ctx,const struct pe_import *i) {
    struct ntimage *d=ctx; const char *why;
    const struct ndis_export *e=ndis_export_find(d->platform.exports,d->platform.export_count,
                                               i->module,i->name,i->ordinal);
    uint32_t address=ndis_export_address(e,&why);
    if(!address) {
        if(d->platform.record) d->platform.record(d->platform.memory.context,d->file,i->module,
                                                 i->name,i->ordinal,why);
        return refuse(d,why);
    }
    pe_put32(d->pe.image+i->iat,address); return 1;
}
int ntimage_load(struct ntimage *d,const void *file,uint32_t n,const char *name,
                 const struct ntimage_platform *platform) {
    /* Caller provides a fresh zeroed module; never overwrite a live pin token. */
    if(d->loaded || d->pe.image || d->entered || d->quarantined) return 0;
    if(!platform || !platform->memory.allocate || !platform->memory.release || !platform->resident) return 0;
    d->scanned=false; d->scan_rva=0;
    d->platform=*platform; d->file=name?name:"<unnamed>";
#ifdef CIUKI_NDIS_HOST_MODEL
    if(!d->platform.exports) d->platform.exports=ndis_exports(&d->platform.export_count);
#else
    d->platform.exports=ndis_exports(&d->platform.export_count);
#endif
    d->ops=(struct pe_ops){.context=d,.admit=policy,.allocate=allocate,.release=release};
    if(!pe_parse(&d->pe,file,n,&d->error,&d->ops)) goto failed;
    if(!pe_allocate_image(&d->pe)) goto failed;
    /* All non-certificate directories must cover real sections/headers,
     * including currently unconsumed directories. Never accept a hole. */
    for(unsigned i=0;i<16;i++) if(i!=4 && d->pe.dirs[i].size &&
        !pe_span(&d->pe,d->pe.dirs[i].rva,d->pe.dirs[i].size)) goto failed;
    if(!pe_walk_imports(&d->pe,d,module,symbol)) goto failed;
    struct ndis_scan_error scan;
    if(!ndis_scan_image(&d->pe,&scan)) {
        d->scan_rva=scan.rva;
        if(d->platform.scan_record) d->platform.scan_record(d->platform.memory.context,d->file,
                                                           scan.rva,scan.reason);
        refuse(d,scan.reason); goto failed;
    }
    d->scanned=true;
    d->loaded=true;
    /* Borrowed file bytes are no longer needed; no dangling input pointer. */
    d->pe.file=0; d->pe.file_size=0;
    return 1;
failed:
    if(d->pe.image) { release(d,d->pe.image,d->pe.size); d->pe.image=0; }
    if(d->platform.record) d->platform.record(d->platform.memory.context,d->file,0,0,0,
                                              d->error.reason?d->error.reason:"load-refused");
    return 0;
}
int ntimage_unload(struct ntimage *d) {
    if(d->entered || d->quarantined) return 0; /* no uncertain callback/DMA reuse */
    if(d->pe.image) { release(d,d->pe.image,d->pe.size); d->pe.image=0; }
    d->loaded=false; d->scanned=false; return 1;
}
bool ndis_execution_available(void) { return false; }
int ntimage_enter(struct ntimage *d,uint32_t driver_object,uint32_t registry_path) {
    (void)driver_object; (void)registry_path;
    /* No foreign call can bypass the ND12 cross-review/admission gate. */
    if(d->platform.record) d->platform.record(d->platform.memory.context,d->file,0,0,0,
                                              "independent-callback-containment-unavailable");
    refuse(d,"independent-callback-containment-unavailable");
    ntimage_unload(d);
    return 0;
}
