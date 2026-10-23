/* Installation transaction over libdisk and a generation-aware VFS boundary.
 * Ordering: per-file commit -> volume flush -> raw verification -> marker;
 * libdisk seals the volume and publishes the canonical MBR LAST.
 * SPDX-License-Identifier: MIT */
#include "internal.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Private mismatch status; CiukiOS intentionally has no ESTALE errno. */
#define ST_IDENTITY_MISMATCH (-10000)
static const struct st_platform *installed;
static struct disk_list inventory;
static struct {
    const struct st_platform *io;
    struct disk_plan disk;
    struct setup_plan plan;
    struct st_manifest manifest;
    struct setup_error error;
    struct setup_progress progress;
    struct setup_callbacks cb;
    struct setup_log log;
    void *ctx;
    uint64_t start, copy_bytes;
    uint32_t generation;
    char release_hash[65], manifest_hash[65], dest;
    unsigned files;
    bool prepared, executing;
} T;
/* Transfer storage is allocated once. libdisk has two 64 KiB static buffers;
 * this buffer + its buffers + ring + streaming reader remain <256 KiB. */
static uint8_t buffer[ST_BUFFER_BYTES];
void st_set_platform(const struct st_platform *p) { if (!T.executing) { installed=p; T.prepared=false; } }
static uint64_t now(void) { return T.io->milliseconds(T.io->ctx); }
static const char *phase_name(enum setup_phase p)
{
    static const char *const names[]={"plan","partition","format","copy","configure","publish","complete"};
    return p<=SETUP_PHASE_COMPLETE ? names[p] : "unknown";
}
static void event(const char *what, const char *path)
{
    char line[512];
    snprintf(line,sizeof(line),"%llu %s %s %s",(unsigned long long)now(),phase_name(T.progress.phase),what,path ? path : "");
    setup_log_append(&T.log,line);
    if (T.cb.log) T.cb.log(T.ctx,line);
}
static void import_log(const struct setup_log *log)
{
    if(!log)return;
    /* The UI ring's lines are not necessarily in the durable log grammar.
     * Prefix them here. Suppress callbacks while importing so an app-owned
     * ring cannot change under the reader when its log callback appends. */
    void (*callback)(void *,const char *)=T.cb.log; T.cb.log=NULL;
    char line[400]; unsigned n=0;
    for(size_t i=0;i<log->length;i++) {
        char c=log->text[(log->start+i)%SETUP_LOG_BYTES];
        if(c=='\n' || n==sizeof(line)-1) {
            line[n]=0; event("ui",line); n=0;
            if(c=='\n')continue;
        }
        line[n++]=c;
    }
    if(n) { line[n]=0; event("ui",line); }
    T.cb.log=callback;
}
static int fail(enum setup_error_class cls, int err, const char *path, uint64_t lba, uint64_t count, unsigned attempt)
{
    T.error=(struct setup_error){.cls=cls,.err=err<0 ? -err : err,.phase=T.progress.phase,.lba=lba,.count=count,.attempt=attempt,
        .retryable=cls==SETUP_ERR_SOURCE_READ || cls==SETUP_ERR_MEDIA_REMOVED};
    if (path) snprintf(T.error.path,sizeof(T.error.path),"%s",path);
    char msg[96]; snprintf(msg,sizeof(msg),"error=%u errno=%d attempt=%u",(unsigned)cls,T.error.err,attempt);
    if (T.io) event(msg,path);
    return -(int)cls;
}
static int disk_result(int e)
{
    if (!e) return 0;
    const struct disk_error_info *d=disk_last_error();
    /* Keep the library boundary explicit, even though f9-02 currently
     * assigns error codes to match the frozen setup enum. */
    enum setup_error_class c;
    switch(e) {
    case DISK_EIO: c=SETUP_ERR_DEST_WRITE; break;
    case DISK_EFLUSH: c=SETUP_ERR_DEST_FLUSH; break;
    case DISK_EFULL: c=SETUP_ERR_DEST_FULL; break;
    case DISK_ECHANGED: c=SETUP_ERR_DEST_CHANGED; break;
    case DISK_ELEASE: c=SETUP_ERR_LEASE; break;
    case DISK_ECONSISTENCY: c=SETUP_ERR_CONSISTENCY; break;
    case DISK_ECANCELLED: c=SETUP_ERR_CANCELLED; break;
    case DISK_ENOMEM: c=SETUP_ERR_MEMORY; break;
    default: c=SETUP_ERR_INTERNAL; break;
    }
    return fail(c,d->err,d->what,d->lba,d->count,0);
}
static int destination(int e, const char *path, bool sync)
{
    if (!e) return 0;
    return fail(e==-ENOSPC ? SETUP_ERR_DEST_FULL : e==-ENOMEM ? SETUP_ERR_MEMORY : sync ? SETUP_ERR_DEST_FLUSH : SETUP_ERR_DEST_WRITE,e,path,0,0,0);
}
static void progress(const char *step)
{
    T.progress.step=step;
    uint64_t elapsed=now()-T.start;
    T.progress.elapsed_ms=elapsed>UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    uint64_t kbps=elapsed ? T.progress.bytes_done*1000/elapsed/1024 : 0;
    T.progress.throughput_kbps=kbps>UINT32_MAX ? UINT32_MAX : (uint32_t)kbps;
    uint64_t eta=T.progress.bytes_done && T.progress.bytes_total>T.progress.bytes_done ?
        elapsed*(T.progress.bytes_total-T.progress.bytes_done)/T.progress.bytes_done : 0;
    T.progress.eta_ms=eta>UINT32_MAX ? UINT32_MAX : (uint32_t)eta;
    if (T.cb.progress) T.cb.progress(T.ctx,&T.progress);
}
static bool cancelled(void *ctx) { (void)ctx; return T.cb.cancel_requested && T.cb.cancel_requested(T.ctx); }
static int cancel_point(void) { return cancelled(NULL) ? fail(SETUP_ERR_CANCELLED,0,NULL,0,0,0) : 0; }
static void disk_progress(void *ctx, const struct disk_progress *p)
{
    (void)ctx;
    T.progress.bytes_done=p->done*512; T.progress.bytes_total=p->total*512;
    progress(p->step);
}
static void phase(enum setup_phase p)
{
    T.progress=(struct setup_progress){.phase=p};
    event("begin",NULL); progress("begin");
}
static int plain_hash(char drive, const char *path, char hex[65])
{
    int h=T.io->open(T.io->ctx,drive,path,false); if(h<0)return h;
    struct disk_sha256 s; disk_sha256_init(&s); int e=0;
    for (;;) {
        int n=T.io->read(T.io->ctx,h,buffer,sizeof(buffer));
        if(n<0) { e=n; break; } if(!n)break;
        if ((unsigned)n>sizeof(buffer)) { e=-EIO; break; }
        disk_sha256_update(&s,buffer,(unsigned)n);
    }
    int c=T.io->close(T.io->ctx,h); if(!e)e=c;
    if(!e) { uint8_t hash[32]; disk_sha256_final(&s,hash); st_hex_encode(hash,hex); }
    return e;
}
static int identities(bool compare)
{
    char release[65], manifest[65];
    int e=plain_hash('C',ST_RELEASE,release); if(!e)e=plain_hash('D',ST_MANIFEST,manifest);
    if(e)return e;
    if(compare) return strcmp(release,T.release_hash) || strcmp(manifest,T.manifest_hash) ? ST_IDENTITY_MISMATCH : 0;
    memcpy(T.release_hash,release,65); memcpy(T.manifest_hash,manifest,65); return 0;
}
static enum setup_error_class source_class(int e)
{ return e==-ENOMEM ? SETUP_ERR_MEMORY : e==-ENXIO || e==-ENODEV || e==-EBADF || e==-EAGAIN ? SETUP_ERR_MEDIA_REMOVED : SETUP_ERR_SOURCE_READ; }
static enum setup_error_class manifest_class(int e)
{ return e==-EINVAL || e==-EOVERFLOW ? SETUP_ERR_SOURCE_MISMATCH : source_class(e); }
static int source_failure(int e,const char *path,unsigned *attempt)
{
    int result=fail(source_class(e),e,path,0,0,*attempt);
    if(!T.error.retryable || *attempt>=SETUP_RETRY_MAX || !T.cb.on_error)return result;
    if(T.cb.on_error(T.ctx,&T.error)!=SETUP_DECISION_RETRY) return fail(SETUP_ERR_CANCELLED,0,path,0,0,*attempt);
    (*attempt)++;
    event("retry",path);
    int id=identities(true);
    if(id==ST_IDENTITY_MISMATCH)return fail(SETUP_ERR_SOURCE_MISMATCH,0,ST_RELEASE,0,0,*attempt);
    /* A removed medium may still be absent. The next read retries/reopens;
     * do not consume an unbounded second retry loop for identity reads. */
    if(id)return source_failure(id,path,attempt);
    T.error=(struct setup_error){0};
    return 0;
}
static int check_source_identity(void)
{
    unsigned attempt=0;
    for(;;) {
        int e=identities(true);
        if(!e)return 0;
        if(e==ST_IDENTITY_MISMATCH)return fail(SETUP_ERR_SOURCE_MISMATCH,0,ST_MANIFEST,0,0,attempt);
        int r=source_failure(e,ST_MANIFEST,&attempt); if(r)return r;
    }
}
static int source_info(char drive,const char *path,struct st_info *out)
{
    unsigned attempt=0;
    for(;;) {
        int e=T.io->info(T.io->ctx,drive,path,out);
        if(!e)return 0;
        int r=source_failure(e,path,&attempt); if(r)return r;
    }
}
static int source_open(char drive,const char *path,uint64_t offset,int *out)
{
    unsigned attempt=0;
    for (;;) {
        int h=T.io->open(T.io->ctx,drive,path,false);
        int e=h<0 ? h : T.io->seek(T.io->ctx,h,offset);
        if(!e) { *out=h; return 0; }
        if(h>=0)T.io->close(T.io->ctx,h);
        int r=source_failure(e,path,&attempt); if(r)return r;
    }
}
/* Every retry discards the old description and seeks a new source handle.
 * offset is explicit so a generation-invalidated fd can never be reused. */
static int source_read(int *h,char drive,const char *path,uint64_t offset,unsigned bytes)
{
    unsigned attempt=0;
    for (;;) {
        bool removed=false; uint32_t generation=0;
        int e=T.io->media(T.io->ctx,false,&removed,&generation);
        if(!e && (removed || generation!=T.generation))e=-EAGAIN;
        if(!e && *h<0) {
            *h=T.io->open(T.io->ctx,drive,path,false);
            e=*h<0 ? *h : T.io->seek(T.io->ctx,*h,offset);
        }
        if(!e) {
            int n=T.io->read(T.io->ctx,*h,buffer,bytes);
            if(n==(int)bytes)return 0;
            e=n<0 ? n : -EIO;
        }
        if(*h>=0)T.io->close(T.io->ctx,*h);
        *h=-1;
        int r=source_failure(e,path,&attempt); if(r)return r;
        e=T.io->media(T.io->ctx,false,&removed,&generation);
        if(!e && !removed) T.generation=generation;
    }
}
static const char *relative(const char *p) { return *p=='/' ? p+1 : p; }
static bool generated(const char *path)
{
    const char *p=relative(path);
    return !strcasecmp(p,"SYSTEM/BOOT.CFG") || !strcasecmp(p,"SYSTEM/SETUP.STATE") ||
        !strcasecmp(p,"SYSTEM/SETUP.LOG") || !strcasecmp(p,"SYSTEM/HARDWARE.TXT") || !strcasecmp(p,"SYSTEM/PAGEFILE.SWP");
}
static int boot_options(char out[128]);
static int prepare_record(void *ctx,const struct st_record *f)
{
    struct disk_payload *payload=ctx;
    if(!(T.plan.components&f->component))return 0;
    const char *path=relative(f->path);
    struct st_info info;
    int e=source_info(f->drive,path,&info);
    if(e)return e;
    if((!f->directory && info.size!=f->size) || (!!(info.attr&16)!=f->directory) || (f->has_attr && info.attr!=f->attr))
        return fail(SETUP_ERR_SOURCE_MISMATCH,0,path,0,0,0);
    if(f->directory) { disk_payload_add_directory(payload); return 0; }
    if(generated(path))return 0;
    if(f->size>UINT32_MAX || payload->bytes>UINT64_MAX-f->size || payload->files==UINT32_MAX)
        return fail(SETUP_ERR_DEST_FULL,EOVERFLOW,path,0,0,0);
    disk_payload_add_file(payload,f->size); T.files++;
    /* Older f9-01 manifests omit the directory inventory. Reserve every
     * ancestor conservatively; mkdir is idempotent during execution. */
    for(const char *p=path; *p;p++)if(*p=='/')disk_payload_add_directory(payload);
    return 0;
}
static bool terminated(const char *s,size_t n) { return memchr(s,0,n)!=NULL; }
static int validate(const struct setup_answers *a)
{
    if(!a || !terminated(a->user_name,sizeof(a->user_name)) || !terminated(a->computer_name,sizeof(a->computer_name)) ||
        !terminated(a->keyboard,sizeof(a->keyboard)) || !terminated(a->disk_model,sizeof(a->disk_model)) ||
        !terminated(a->disk_serial,sizeof(a->disk_serial)) || !a->accept_licences || a->accept_licences>1 ||
        a->erase_disk>1 || a->reinstall>1 || a->unattended>1 ||
        !setup_components_valid(a->components) || (a->lobby && !(a->components&SETUP_COMP_WINDOWS)) ||
        !setup_valid_user(a->user_name) || !setup_valid_computer(a->computer_name) ||
        (strcmp(a->keyboard,"us") && strcmp(a->keyboard,"it")) || a->tz_minutes<SETUP_TZ_MIN || a->tz_minutes>SETUP_TZ_MAX ||
        (a->clock!=SETUP_CLOCK_LOCAL && a->clock!=SETUP_CLOCK_UTC) || a->network<SETUP_NET_DHCP || a->network>SETUP_NET_NONE ||
        a->restart<SETUP_RESTART_EJECT || a->restart>SETUP_RESTART_FORCE || a->ipx>1 || a->lobby>1 ||
        (a->log && (a->log->start>=SETUP_LOG_BYTES || a->log->length>SETUP_LOG_BYTES)) ||
        (a->hardware_report_length && !a->hardware_report) || a->hardware_report_length>65536 ||
        (a->network==SETUP_NET_STATIC && !setup_static_valid(a->ip,a->prefix,a->gateway,a->dns)))return -EINVAL;
    if(a->unattended) { struct setup_inf_status status; if(setup_inf_complete(a,&status))return -EINVAL; }
    return 0;
}
int setup_txn_prepare(const struct setup_answers *a,struct setup_plan *out)
{
    if(T.executing)return -SETUP_ERR_INTERNAL;
    memset(&T,0,sizeof(T)); T.io=installed ? installed : st_native_platform();
    if(!T.io)return -SETUP_ERR_INTERNAL;
    T.start=now();
    if(!out || validate(a))return fail(SETUP_ERR_INTERNAL,EINVAL,"answers",0,0,0);
    memset(out,0,sizeof(*out)); T.plan.answers=*a; T.plan.components=a->components;
    int e=T.io->preflight(T.io->ctx,a); if(e)return fail(SETUP_ERR_INTERNAL,e,"platform preflight",0,0,0);
    struct disk_list *disks=&inventory;
    e=disk_enumerate(disks); if(e)return disk_result(e);
    struct disk_entry *d=&disks->disk[0];
    if(a->disk<SETUP_DISK_ATA0 || a->disk>SETUP_DISK_AUTO || !d->present ||
        (a->disk==SETUP_DISK_MATCH && (strcmp(d->id.model,a->disk_model) || strcmp(d->id.serial,a->disk_serial))))
        return fail(SETUP_ERR_DEST_CHANGED,ENODEV,"ata0",0,0,0);
    if(a->disk==SETUP_DISK_AUTO)for(unsigned i=1;i<DISK_SLOTS;i++)if(disks->disk[i].present)return fail(SETUP_ERR_DEST_CHANGED,EBUSY,"ambiguous disk",0,0,0);
    if((d->table_present || d->partitions || d->volumes) && !a->erase_disk)return fail(SETUP_ERR_INTERNAL,EPERM,"EraseDisk",0,0,0);
    e=identities(false); if(e)return fail(source_class(e),e,ST_MANIFEST,0,0,0);
    e=st_manifest_read(T.io,&T.manifest,NULL,NULL,NULL); if(e)return fail(manifest_class(e),e,ST_MANIFEST,0,0,0);
    /* Bound the same-release reinstall guard even with the short libdisk
     * SETUP.STATE preview: read its mounted volume's complete state. */
    if(!a->reinstall && !d->stub_mbr)for(unsigned i=0;i<d->volumes;i++) {
        int h=T.io->open(T.io->ctx,d->volume[i].letter,"SYSTEM/SETUP.STATE",false);
        if(h>=0) {
            int n=T.io->read(T.io->ctx,h,buffer,4096); T.io->close(T.io->ctx,h);
            if(n>0 && n<=4096) { buffer[n]=0;
                if(strstr((char *)buffer,"phase=installed") && strstr((char *)buffer,T.manifest.image_sha))
                    return fail(SETUP_ERR_DEST_CHANGED,EEXIST,"already_installed",0,0,0);
            } else if(n<0)return fail(SETUP_ERR_DEST_WRITE,n,"SETUP.STATE",0,0,0);
        } else if(h!=-ENOENT)return fail(SETUP_ERR_DEST_WRITE,h,"SETUP.STATE",0,0,0);
    }
    char boot_template[128];
    e=boot_options(boot_template); if(e)return e;
    struct disk_payload payload={0};
    struct st_manifest check;
    e=st_manifest_read(T.io,&check,prepare_record,&payload,NULL);
    if(e)return T.error.cls ? -(int)T.error.cls : fail(manifest_class(e),e,ST_MANIFEST,0,0,0);
    T.copy_bytes=payload.bytes;
    /* Generated records, registry/settings growth, and log/hardware budget
     * are reserved in addition to immutable files. The pager is not copied. */
    disk_payload_add_file(&payload,4096); disk_payload_add_file(&payload,SETUP_LOG_BYTES);
    disk_payload_add_file(&payload,a->hardware_report_length); disk_payload_add_file(&payload,127);
    disk_payload_add_file(&payload,65536); payload.directories+=4;
    e=disk_plan_whole(0,&payload,&T.disk);
    struct setup_plan *p=&T.plan;
    memcpy(p->disk.model,T.disk.id.model,sizeof(p->disk.model)); memcpy(p->disk.serial,T.disk.id.serial,sizeof(p->disk.serial));
    p->disk.sectors=T.disk.id.sectors; memcpy(p->disk.table_fingerprint,T.disk.id.table_fingerprint,32);
    p->partition_start=T.disk.geo.part_start; p->partition_count=T.disk.geo.part_sectors;
    p->version=SETUP_TXN_VERSION; p->payload_bytes=payload.bytes;
    p->required_bytes=T.disk.required_bytes; p->overhead_bytes=T.disk.overhead_bytes;
    p->reserve_bytes=T.disk.reserve_bytes; p->free_after_bytes=T.disk.free_after_bytes;
    p->erased_total=T.disk.erased;
    p->erased_partitions=T.disk.erased<SETUP_ERASED_MAX ? T.disk.erased : SETUP_ERASED_MAX;
    for(unsigned i=0;i<p->erased_partitions;i++) { p->erased[i].type=T.disk.erased_list[i].type; p->erased[i].start=T.disk.erased_list[i].start; p->erased[i].count=T.disk.erased_list[i].count; }
    memcpy(p->source_image_sha,T.manifest.image_sha,65); memcpy(p->source_iso_sha,T.manifest_hash,65);
    *out=*p;
    if(e)return disk_result(e);
    bool removed=false;
    e=T.io->media(T.io->ctx,false,&removed,&T.generation);
    if(e || removed)return fail(SETUP_ERR_MEDIA_REMOVED,e,ST_MANIFEST,0,0,0);
    /* Guard races between the two streaming passes and source probes. */
    e=identities(true); if(e)return fail(e==ST_IDENTITY_MISMATCH ? SETUP_ERR_SOURCE_MISMATCH : source_class(e),e==ST_IDENTITY_MISMATCH ? 0 : e,ST_MANIFEST,0,0,0);
    T.prepared=true; return 0;
}
static int write_all(int h,const void *data,unsigned n,const char *path)
{
    const uint8_t *p=data;
    while(n) {
        int k=T.io->write(T.io->ctx,h,p,n);
        if(k<=0 || (unsigned)k>n)return destination(k<0 ? k : -EIO,path,false);
        p+=k; n-=(unsigned)k;
    }
    return 0;
}
static int write_file(const char *path,const void *data,unsigned n)
{
    int h=T.io->open(T.io->ctx,T.dest,path,true); if(h<0)return destination(h,path,false);
    int e=write_all(h,data,n,path);
    if(!e)e=destination(T.io->sync(T.io->ctx,h),path,true);
    int c=T.io->close(T.io->ctx,h); if(!e)e=destination(c,path,false);
    return e;
}
static int parents(const char *path)
{
    char p[260]; snprintf(p,sizeof(p),"%s",path);
    for(char *s=p;*s;s++)if(*s=='/') {
        *s=0; int e=T.io->mkdir(T.io->ctx,T.dest,p); *s='/';
        if(e && e!=-EEXIST)return destination(e,p,false);
    }
    return 0;
}
static int marker(const char *name)
{
    char text[256];
    int n=snprintf(text,sizeof(text),"phase=%s\nrelease=%s\ndisk_signature=%08x\nfirstboot=pending\ndone=%u\n",
        name,T.plan.source_image_sha,T.disk.disk_signature,T.progress.files_done);
    int e=write_file("SYSTEM/SETUP.STATE",text,(unsigned)n);
    return e ? e : disk_result(disk_flush(0));
}
static int copy_record(void *ctx,const struct st_record *f)
{
    (void)ctx;
    if(!(T.plan.components&f->component))return 0;
    const char *p=relative(f->path);
    int e=cancel_point(); if(e)return e;
    if((e=parents(p)))return e;
    if(f->directory) { e=T.io->mkdir(T.io->ctx,T.dest,p); return e==-EEXIST ? 0 : destination(e,p,false); }
    if(generated(p))return 0;
    int source=-1; e=source_open(f->drive,p,0,&source); if(e)return e;
    int dest=T.io->open(T.io->ctx,T.dest,p,true);
    if(dest<0) { T.io->close(T.io->ctx,source); return destination(dest,p,false); }
    struct disk_sha256 s; disk_sha256_init(&s);
    uint64_t offset=0;
    while(offset<f->size) {
        if((e=cancel_point()))break;
        unsigned n=f->size-offset<sizeof(buffer) ? (unsigned)(f->size-offset) : sizeof(buffer);
        if((e=source_read(&source,f->drive,p,offset,n)))break;
        disk_sha256_update(&s,buffer,n);
        if((e=write_all(dest,buffer,n,p)))break;
        offset+=n; T.progress.bytes_done+=n; progress(p);
    }
    if(source>=0)T.io->close(T.io->ctx,source);
    if(!e) { uint8_t hash[32]; disk_sha256_final(&s,hash); if(memcmp(hash,f->sha,32))e=fail(SETUP_ERR_SOURCE_MISMATCH,0,p,0,0,0); }
    if(!e)e=destination(T.io->sync(T.io->ctx,dest),p,true);
    int c=T.io->close(T.io->ctx,dest); if(!e)e=destination(c,p,false);
    if(!e) {
        struct st_info info;
        int r=source_info(f->drive,p,&info);
        if(r)e=r;
        else e=destination(T.io->attrib(T.io->ctx,T.dest,p,f->has_attr ? f->attr : info.attr),p,false);
    }
    if(!e) { T.progress.files_done++; char count[64]; snprintf(count,sizeof(count),"file done=%u",T.progress.files_done); event(count,p); progress(p); }
    return e;
}
static int finish_directory(void *ctx,const struct st_record *f)
{
    (void)ctx;
    if(!f->directory || !(T.plan.components&f->component))return 0;
    const char *p=relative(f->path);
    struct st_info src;
    int e=source_info(f->drive,p,&src);
    if(e)return e;
    return destination(T.io->attrib(T.io->ctx,T.dest,p,f->has_attr ? f->attr : src.attr),p,false);
}
static int verify_record(void *ctx,const struct st_record *f)
{
    (void)ctx;
    if(!(T.plan.components&f->component) || generated(f->path))return 0;
    const char *p=relative(f->path); int e=cancel_point(); if(e)return e;
    struct st_info src,dst;
    int r=source_info(f->drive,p,&src); if(r)return r;
    r=T.io->info(T.io->ctx,T.dest,p,&dst); if(r)return destination(r,p,false);
    const char *base=strrchr(p,'/'); base=base ? base+1 : p;
    if((!f->directory && dst.size!=f->size) || dst.attr!=(f->has_attr ? f->attr : src.attr) || strcmp(dst.name,base) ||
        (src.alias[0] && strcasecmp(src.alias,dst.alias)))
        return fail(SETUP_ERR_CONSISTENCY,0,p,0,0,0);
    if(f->directory)return 0;
    struct disk_sha256 s; disk_sha256_init(&s);
    uint64_t left=f->size;
    uint32_t index=0;
    while(left) {
        if((e=cancel_point()))return e;
        uint64_t lba=0;
        r=T.io->cluster(T.io->ctx,T.dest,p,index++,&lba);
        if(r)return fail(SETUP_ERR_CONSISTENCY,r,p,0,0,0);
        uint64_t first=T.disk.geo.part_start+T.disk.geo.data_sector;
        uint64_t end=first+(uint64_t)T.disk.geo.clusters*DISK_SPC;
        if(lba<first || lba>end-DISK_SPC || (lba-first)%DISK_SPC)return fail(SETUP_ERR_CONSISTENCY,EINVAL,p,lba,8,0);
        r=disk_read_raw(0,lba,DISK_SPC,buffer); if(r) { e=disk_result(r); snprintf(T.error.path,sizeof(T.error.path),"%s",p); return e; }
        unsigned n=left<4096 ? (unsigned)left : 4096;
        disk_sha256_update(&s,buffer,n); left-=n; progress(p);
    }
    uint64_t extra=0;
    r=T.io->cluster(T.io->ctx,T.dest,p,index,&extra);
    if(r!=-ENOENT)return fail(SETUP_ERR_CONSISTENCY,r,p,extra,8,0);
    uint8_t hash[32]; disk_sha256_final(&s,hash);
    return memcmp(hash,f->sha,32) ? fail(SETUP_ERR_SOURCE_MISMATCH,0,p,0,0,0) : 0;
}
static int manifest_retry(void *ctx,int err,const char *path,unsigned *attempt)
{ (void)ctx; return source_failure(err,path,attempt); }
static int walk(st_record_fn fn)
{
    struct st_manifest m;
    int e=st_manifest_read(T.io,&m,fn,NULL,manifest_retry);
    if(!e)return 0;
    return T.error.cls ? -(int)T.error.cls : fail(manifest_class(e),e,ST_MANIFEST,0,0,0);
}
static int write_log(void)
{
    int h=T.io->open(T.io->ctx,T.dest,"SYSTEM/SETUP.LOG",true); if(h<0)return destination(h,"SYSTEM/SETUP.LOG",false);
    size_t start=T.log.start,length=T.log.length;
    /* Byte rings can begin inside the oldest line after wrapping. Persist
     * complete records only, retaining the required timestamp grammar. */
    if(T.log.dropped)while(length) {
        char c=T.log.text[start]; start=(start+1)%SETUP_LOG_BYTES; length--;
        if(c=='\n')break;
    }
    size_t first=SETUP_LOG_BYTES-start; if(first>length)first=length;
    int e=write_all(h,T.log.text+start,(unsigned)first,"SYSTEM/SETUP.LOG");
    if(!e)e=write_all(h,T.log.text,(unsigned)(length-first),"SYSTEM/SETUP.LOG");
    if(!e)e=destination(T.io->sync(T.io->ctx,h),"SYSTEM/SETUP.LOG",true);
    int c=T.io->close(T.io->ctx,h); return e ? e : destination(c,"SYSTEM/SETUP.LOG",false);
}
static int boot_options(char out[128])
{
    size_t n=strlen(T.manifest.boot_template);
    struct disk_sha256 digest; uint8_t hash[32],expected[32];
    disk_sha256_init(&digest); disk_sha256_update(&digest,T.manifest.boot_template,n);
    disk_sha256_final(&digest,hash);
    if (!n || n>127 || st_hex_decode(T.manifest.boot_template_sha,expected) || memcmp(hash,expected,32) ||
        strstr(T.manifest.boot_template,"setup=1"))
        return fail(SETUP_ERR_SOURCE_MISMATCH,EINVAL,"BOOT.CFG template",0,0,0);
    for(size_t i=0;i<n;i++)if((unsigned char)T.manifest.boot_template[i]<32 && T.manifest.boot_template[i]!='\n' && T.manifest.boot_template[i]!='\r')
        return fail(SETUP_ERR_SOURCE_MISMATCH,EINVAL,"BOOT.CFG template",0,0,0);
    memcpy(out,T.manifest.boot_template,n+1);
    return 0;
}
static int read_loader(uint8_t **out,unsigned *bytes)
{
    const char *path=T.manifest.boot_image;
    int h=-1; int e=source_open('D',path,512,&h); if(e)return e;
    e=source_read(&h,'D',path,512,512);
    unsigned sectors=(unsigned)buffer[6]|(unsigned)buffer[7]<<8;
    if(!e && (memcmp(buffer,"CLDR",4) || !sectors || sectors>DISK_LOADER_SECTORS || T.manifest.boot_image_bytes<512ull+(uint64_t)sectors*512))e=fail(SETUP_ERR_SOURCE_MISMATCH,EINVAL,path,1,sectors,0);
    uint8_t *loader=NULL;
    if(!e) { *bytes=sectors*512; loader=malloc(*bytes); if(!loader)e=fail(SETUP_ERR_MEMORY,ENOMEM,path,0,0,0); }
    if(!e)memcpy(loader,buffer,512);
    for(unsigned offset=512;!e && offset<*bytes;) {
        if((e=cancel_point()))break;
        unsigned n=*bytes-offset<sizeof(buffer) ? *bytes-offset : sizeof(buffer);
        e=source_read(&h,'D',path,512+offset,n);
        if(!e) { memcpy(loader+offset,buffer,n); offset+=n; }
    }
    if(h>=0)T.io->close(T.io->ctx,h);
    if(!e) {
        struct disk_sha256 s; uint8_t hash[32],expected[32];
        disk_sha256_init(&s); disk_sha256_update(&s,loader,*bytes); disk_sha256_final(&s,hash);
        if(st_hex_decode(T.manifest.loader_sha,expected) || memcmp(hash,expected,32))e=fail(SETUP_ERR_SOURCE_MISMATCH,0,path,1,sectors,0);
    }
    if(e)free(loader); else *out=loader;
    return e;
}
int setup_txn_execute(const struct setup_plan *plan,const struct setup_callbacks *cb,void *ctx)
{
    if(T.executing || !T.prepared || !plan || memcmp(plan,&T.plan,sizeof(*plan)))return fail(SETUP_ERR_INTERNAL,EINVAL,"unprepared or modified plan",0,0,0);
    T.executing=true; T.prepared=false; T.ctx=ctx;
    T.cb=cb ? *cb : (struct setup_callbacks){0}; T.error=(struct setup_error){0};
    import_log(plan->answers.log);
    T.start=now();
    int e=cancel_point();
    if(!e)e=check_source_identity();
    struct disk_callbacks dc={disk_progress,cancelled,NULL};
    if(!e) { phase(SETUP_PHASE_PARTITION); e=disk_result(disk_execute_p1(&T.disk,&dc)); }
    if(!e) { phase(SETUP_PHASE_FORMAT); e=disk_result(disk_execute_p2(&T.disk,&dc,&T.dest)); }
    if(!e)e=marker("format");
    if(!e) {
        phase(SETUP_PHASE_COPY); T.progress.files_total=T.files; T.progress.bytes_total=T.copy_bytes;
        e=walk(copy_record);
        if(!e)e=walk(finish_directory);
        if(!e)e=disk_result(disk_flush(0));
        if(!e)e=walk(verify_record);
        if(!e)e=marker("copy");
    }
    if(!e) {
        phase(SETUP_PHASE_CONFIGURE); e=cancel_point();
        if(!e)e=destination(T.io->configure(T.io->ctx,T.dest,&plan->answers),"configuration writers",false);
        if(!e)e=cancel_point();
        if(!e)e=write_file("SYSTEM/HARDWARE.TXT",plan->answers.hardware_report,(unsigned)plan->answers.hardware_report_length);
        if(!e)e=write_log();
        if(!e)e=marker("configure");
    }
    uint8_t *loader=NULL; unsigned bytes=0;
    if(!e) {
        phase(SETUP_PHASE_PUBLISH); e=cancel_point();
        struct disk_check check;
        if(!e)e=disk_result(disk_consistency_check(T.dest,&check));
        if(!e)e=read_loader(&loader,&bytes);
        char cfg[128],state[256];
        if(!e)e=boot_options(cfg);
        if(!e) {
            snprintf(state,sizeof(state),"phase=installed\nrelease=%s\ndisk_signature=%08x\nfirstboot=pending\n",plan->source_image_sha,T.disk.disk_signature);
            event("seal",NULL); e=write_log();
            struct disk_publish pub={loader,bytes,cfg,state};
            if(!e)e=disk_result(disk_execute_p5(&T.disk,&pub,&dc));
        }
        if(!e)phase(SETUP_PHASE_COMPLETE);
    }
    free(loader);
    /* Never write a failure marker after publication: libdisk may have
     * already sealed the volume. Retain the previous durable phase marker. */
    int release=disk_release(0);
    if(!e && release)e=disk_result(release);
    if(T.io==st_native_platform())st_native_cleanup();
    T.executing=false;
    if(!e)T.error=(struct setup_error){0};
    return e;
}
int setup_txn_last_error(struct setup_error *out) { if(!out)return -SETUP_ERR_INTERNAL; *out=T.error; return 0; }
int setup_txn_state(enum setup_phase *p) { if(!p)return -SETUP_ERR_INTERNAL; *p=T.progress.phase; return 0; }
int setup_restart(enum setup_restart policy)
{
    const struct st_platform *io=installed ? installed : st_native_platform();
    if(!io || policy<SETUP_RESTART_EJECT || policy>SETUP_RESTART_FORCE)return -SETUP_ERR_INTERNAL;
    bool removed=false; uint32_t generation=0;
    int e=io->media(io->ctx,policy==SETUP_RESTART_EJECT,&removed,&generation);
    if(e && policy!=SETUP_RESTART_FORCE)return -SETUP_ERR_MEDIA_REMOVED;
    uint64_t start=io->milliseconds(io->ctx);
    for(;;) {
        if(policy!=SETUP_RESTART_FORCE && removed)break;
        if(policy!=SETUP_RESTART_WAIT && io->milliseconds(io->ctx)-start>=60000)break;
        io->sleep_ms(io->ctx,100);
        e=io->media(io->ctx,false,&removed,&generation);
        if(e && policy!=SETUP_RESTART_FORCE)return -SETUP_ERR_MEDIA_REMOVED;
    }
    e=io->restart(io->ctx);
    return e ? -SETUP_ERR_INTERNAL : 0;
}
