/* G29D: bounded actual image-read hints. Included by fat.c.
 * The log is advisory: bad CRC/identity means no placement, never a disk address.
 * SPDX-License-Identifier: GPL-2.0-only */
#define START_LOG "CIUKIST.LOG"
#define START_FILES 24u
#define START_READS 16u
struct startup_record {
    uint32_t parent, index, first, size, uses, recent;
    uint16_t date, time;
    uint8_t count;
    char alias[13];
    uint8_t pad[2];
    uint32_t order[START_READS];
};
struct startup_log {
    uint32_t magic, serial, clock, crc;
    struct startup_record files[START_FILES];
};
_Static_assert(sizeof(struct startup_log)<=4096,"startup log page");
static uint32_t startup_crc(struct startup_log *log)
{
    uint32_t saved=log->crc, crc=~0u; log->crc=0;
    const uint8_t *b=(const uint8_t *)log;
    for (unsigned i=0;i<sizeof(*log);i++) {
        crc^=b[i];
        for (unsigned j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    log->crc=saved; return ~crc;
}
static bool startup_match(const struct fat_volume *v,const struct startup_record *r,const struct fat_entry *f)
{
    return r->uses && r->parent==(f->parent ? f->parent : v->type==32 ? v->root : 0) && r->index==f->index &&
        r->first==f->first && r->size==f->size && r->date==f->times.write_date &&
        r->time==f->times.write_time && !strncmp(r->alias,f->alias,sizeof(r->alias));
}
static void startup_validate(struct fat_volume *v,struct startup_log *log,size_t bytes)
{
    if (bytes!=sizeof(*log) || log->magic!=0x314c5343 || log->serial!=v->volume_serial ||
        log->crc!=startup_crc(log)) memset(log,0,sizeof(*log));
    for (unsigned i=0;i<START_FILES;i++) {
        struct startup_record *r=&log->files[i];
        if (r->count>START_READS || r->alias[sizeof(r->alias)-1]) { r->uses=0; continue; }
        uint32_t clusters=r->size/((uint32_t)v->spc*512)+(r->size%((uint32_t)v->spc*512)!=0);
        for (unsigned j=0;j<r->count;j++) {
            if (r->order[j]>=clusters) r->uses=0;
            for (unsigned k=0;k<j;k++) if (r->order[k]==r->order[j]) r->uses=0;
        }
    }
}
static int startup_load(struct fat_volume *v,struct startup_log *log,struct fat_entry *file)
{
    memset(log,0,sizeof(*log));
    int e=fat_lookup(v,v->type==32 ? v->root : 0,START_LOG,file);
    if (e==-FS_ENOENT) { memset(file,0,sizeof(*file)); return 0; }
    if (e) return e;
    if (file->size==sizeof(*log) && !(file->attr&FAT_ATTR_DIR)) {
        size_t done=0; e=fat_read(v,file,0,log,sizeof(*log),&done);
        if (e) return e;
        if (done==sizeof(*log)) { startup_validate(v,log,done); return 0; }
    }
    memset(log,0,sizeof(*log)); return 0;
}
/* Also used by the VFS path: serialization performs no device I/O. */
size_t fat_startup_size(void) { return sizeof(struct startup_log); }
int fat_startup_prepare(struct fat_volume *v,const struct fat_entry *file,const uint32_t *order,
                        unsigned count,void *buffer,size_t bytes)
{
    if (!v || !v->mounted || !v->spc || !file || !order || !count || count>START_READS || !file->size ||
        (file->attr&FAT_ATTR_DIR) || path_equal(file->alias,START_LOG) || !buffer) return -FS_EINVAL;
    struct startup_log *log=buffer;
    startup_validate(v,log,bytes);
    unsigned slot=0;
    for (unsigned i=0;i<START_FILES;i++) {
        if (startup_match(v,&log->files[i],file)) { slot=i; break; }
        if (log->files[i].recent<log->files[slot].recent) slot=i;
    }
    {
        struct startup_record *r=&log->files[slot];
        uint32_t uses=startup_match(v,r,file) ? r->uses : 0;
        memset(r,0,sizeof(*r)); r->parent=file->parent ? file->parent : v->type==32 ? v->root : 0; r->index=file->index;
        r->first=file->first; r->size=file->size; r->uses=uses==UINT32_MAX ? uses : uses+1;
        if (log->clock==UINT32_MAX) { /* age without reversing the LRU order */
            for (unsigned i=0;i<START_FILES;i++) log->files[i].recent/=2;
            log->clock/=2;
        }
        r->recent=++log->clock; r->date=file->times.write_date; r->time=file->times.write_time;
        memcpy(r->alias,file->alias,sizeof(r->alias)-1);
        uint32_t clusters=file->size/((uint32_t)v->spc*512)+(file->size%((uint32_t)v->spc*512)!=0);
        for (unsigned i=0;i<count;i++) {
            if (order[i]>=clusters) return -FS_EINVAL;
            bool duplicate=false;
            for (unsigned j=0;j<r->count;j++) if (r->order[j]==order[i]) duplicate=true;
            if (!duplicate) r->order[r->count++]=order[i];
        }
        log->magic=0x314c5343; log->serial=v->volume_serial; log->crc=startup_crc(log);
    }
    return 0;
}

static int startup_save(struct fat_volume *v,struct startup_log *log,struct fat_entry *file)
{
    int e=0;
    if (!file->first && !file->size) {
        e=fat_lookup(v,v->type==32 ? v->root : 0,START_LOG,file);
        if (e==-FS_ENOENT) e=fat_create(v,v->type==32 ? v->root : 0,START_LOG,FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM,file);
    }
    if (e) return e;
    if ((file->attr&(FAT_ATTR_DIR|FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM))!=(FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM)) return -FS_EACCES;
    log->magic=0x314c5343; log->serial=v->volume_serial; log->crc=startup_crc(log);
    size_t done=0; e=fat_write(v,file,0,log,sizeof(*log),&done);
    if (!e && done!=sizeof(*log)) e=-FS_EIO;
    if (!e && file->size>sizeof(*log)) e=fat_truncate(v,file,sizeof(*log));
    return e ? e : fat_commit(v);
}
int fat_startup_record(struct fat_volume *v,const struct fat_entry *file,const uint32_t *order,unsigned count)
{
    if (!v || !v->mounted || !v->spc || !file || !order || !count || count>START_READS || !file->size ||
        (file->attr&FAT_ATTR_DIR) || path_equal(file->alias,START_LOG)) return -FS_EINVAL;
    int e=writable(v); if (e) return e;
    struct startup_log *log=fs_page_alloc(); if (!log) return -FS_ENOMEM;
    struct fat_entry saved={0}; e=startup_load(v,log,&saved);
    if (!e) e=fat_startup_prepare(v,file,order,count,log,sizeof(*log));
    if (!e) e=startup_save(v,log,&saved);
    fs_page_free(log); return e;
}
/* Physical offset for logical cluster i. Observed clusters precede the rest. */
static uint32_t startup_offset(const struct startup_record *r,uint32_t i)
{
    if (!r) return i;
    uint32_t before=0;
    for (unsigned j=0;j<r->count;j++) {
        if (r->order[j]==i) return j;
        if (r->order[j]<i) before++;
    }
    return r->count+i-before;
}
