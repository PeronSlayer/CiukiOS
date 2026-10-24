/* SPDX-License-Identifier: MIT. Compiled once, into model.c. */
#include "../../sdk/win32/include/w32/server.h"

uint32_t (*fm_registry_exchange)(const void *, uint32_t, void *, uint32_t, uint32_t *);

static int op_read(const struct fm_vfs *fs, const char *path, uint8_t **out, unsigned limit, unsigned *used)
{
    struct fm_meta m; int e=fs->stat_path(fs->ctx,path,&m);
    if (e) return e;
    if (m.type!=FM_FILE || m.size>limit) return -EOVERFLOW;
    uint8_t *p=malloc((size_t)m.size+1); if (!p) return -ENOMEM;
    int fd=fs->open(fs->ctx,path,0); if (fd<0) { free(p); return fd; }
    unsigned at=0;
    while (at<m.size) { int n=fs->read(fs->ctx,fd,p+at,(unsigned)m.size-at); if (n==-EINTR) continue; if (n<=0) { e=n<0?n:-EIO; break; } at+=(unsigned)n; }
    int closed=fs->close(fs->ctx,fd); if (!e) e=closed;
    if (e) { free(p); return e; }
    p[at]=0; *out=p; *used=at; return 0;
}
int fm_write_new(const struct fm_vfs *fs,const char *path,const void *bytes,unsigned count)
{
    int fd=fs->open(fs->ctx,path,1); if (fd<0) return fd;
    unsigned at=0; int e=0;
    while (at<count) { int n=fs->write(fs->ctx,fd,(const uint8_t *)bytes+at,count-at); if (n==-EINTR) continue; if (n<=0) { e=n<0?n:-EIO; break; } at+=(unsigned)n; }
    if (!e) e=fs->sync(fs->ctx,fd);
    int closed=fs->close(fs->ctx,fd); if (!e) e=closed;
    if (e) fs->unlink(fs->ctx,path);
    return e;
}
static int op_publish(const struct fm_vfs *fs,const char *path,const void *bytes,unsigned count)
{
    static unsigned serial;
    char *temp=malloc(FM_PATH); if (!temp) return -ENOMEM;
    int e=-EEXIST;
    for (unsigned i=0;i<32 && e==-EEXIST;i++) {
        unsigned id=__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED);
        int n=snprintf(temp,FM_PATH,"%s.%u.tmp",path,id);
        if (n<0 || n>=FM_PATH) { e=-ENAMETOOLONG; break; }
        e=fm_write_new(fs,temp,bytes,count);
    }
    if (!e) { e=fs->rename(fs->ctx,temp,path); if (e) fs->unlink(fs->ctx,temp); }
    free(temp); return e;
}
/* Metadata plus contents for every descendant. Allocation follows FM_DEPTH,
 * never recursion on the native 4 KiB stack. Enumeration is sorted so its
 * physical FAT slot order cannot change the identity. */
static uint64_t op_hash(uint64_t h,const void *bytes,size_t n)
{ const uint8_t *p=bytes; while(n--) { h^=*p++; h*=UINT64_C(1099511628211); } return h; }
static int op_entry_order(const void *a,const void *b)
{ return compare(((const struct fm_entry *)a)->name,((const struct fm_entry *)b)->name); }
static int op_stamp(const struct fm_vfs *fs,const char *path,uint64_t *stamp,int contents)
{
    /* Moving/restoring an edited object is safe; its identity must still be
     * the one recorded. Only Undo Copy removes data, so it checks the tree
     * and contents. Rename never rereads a large file on the input thread. */
    if (!contents) {
        struct fm_meta m; int e=fs->stat_path(fs->ctx,path,&m); if (e) return e;
        uint64_t h=UINT64_C(1469598103934665603);
        h=op_hash(h,&m.dev,sizeof(m.dev)); h=op_hash(h,&m.ino,sizeof(m.ino));
        *stamp=op_hash(h,&m.type,sizeof(m.type)); return 0;
    }
    struct fm_frame *frames=calloc(FM_DEPTH,sizeof(*frames));
    uint8_t *bytes=malloc(FM_CHUNK); if (!frames || !bytes) { free(frames); free(bytes); return -ENOMEM; }
    strcpy(frames[0].source,path); unsigned depth=1; int e=0;
    uint64_t hash=UINT64_C(1469598103934665603);
    while (depth && !e) {
        struct fm_frame *f=&frames[depth-1];
        if (!f->phase) {
            e=fs->stat_path(fs->ctx,f->source,&f->meta); if (e) break;
            hash=op_hash(hash,f->source,strlen(f->source)+1);
            hash=op_hash(hash,&f->meta.type,sizeof(f->meta.type));
            hash=op_hash(hash,&f->meta.ino,sizeof(f->meta.ino));
            hash=op_hash(hash,&f->meta.size,sizeof(f->meta.size));
            hash=op_hash(hash,&f->meta.modified,sizeof(f->meta.modified));
            if (f->meta.type==FM_FILE) {
                int fd=fs->open(fs->ctx,f->source,0); if (fd<0) { e=fd; break; }
                uint64_t remaining=f->meta.size;
                while (remaining && !e) {
                    unsigned request=remaining>FM_CHUNK?FM_CHUNK:(unsigned)remaining;
                    int n=fs->read(fs->ctx,fd,bytes,request);
                    if (n==-EINTR) continue;
                    if (n<=0) e=n<0?n:-EIO; else { hash=op_hash(hash,bytes,(unsigned)n); remaining-=(unsigned)n; }
                }
                int close_error=fs->close(fs->ctx,fd); if (!e) e=close_error;
                free_frame(&frames[--depth]); continue;
            }
            if (f->meta.type!=FM_DIR) { e=-ENOTSUP; break; }
            struct listing l={0}; e=fs->list(fs->ctx,f->source,FM_LIST_METADATA,collect,&l);
            f->entries=l.entries; f->count=l.count; f->phase=1;
            if (l.count>1) qsort(l.entries,l.count,sizeof(*l.entries),op_entry_order);
        }
        if (!e && f->next<f->count) {
            if (depth==FM_DEPTH) { e=-ENAMETOOLONG; break; }
            e=fm_join(frames[depth].source,f->source,f->entries[f->next++].name); if (!e) depth++;
        } else if (!e) free_frame(&frames[--depth]);
    }
    for (unsigned i=0;i<depth;i++) free_frame(&frames[i]);
    free(frames); free(bytes); if (!e) *stamp=hash; return e;
}
int fm_undo_supported(const struct fm_vfs *fs)
{ return fs && fs->stat_path && fs->list && fs->mkdir && fs->rename && fs->unlink && fs->rmdir && fs->open && fs->read && fs->write && fs->sync && fs->close; }
void fm_undo_free(struct fm_undo_log *log) { free(log->entries); memset(log,0,sizeof(*log)); }
static int undo_save(const struct fm_vfs *fs,const struct fm_undo_log *log)
{
    int directory=fs->mkdir(fs->ctx,"/system/settings"); if (directory && directory!=-EEXIST) return directory;
    size_t size=16;
    for (unsigned i=0;i<log->count;i++) size+=16+strlen(log->entries[i].source)+strlen(log->entries[i].target);
    uint8_t *p=calloc(1,size); if (!p) return -ENOMEM;
    memcpy(p,"CIUKUNDO",8); desk_link_put32(p+8,log->kind); desk_link_put32(p+12,log->count); unsigned at=16;
    for (unsigned i=0;i<log->count;i++) {
        const struct fm_undo_entry *v=&log->entries[i]; unsigned a=(unsigned)strlen(v->source),b=(unsigned)strlen(v->target);
        desk_link_put32(p+at,a); desk_link_put32(p+at+4,b); desk_link_put32(p+at+8,(uint32_t)v->stamp); desk_link_put32(p+at+12,(uint32_t)(v->stamp>>32)); at+=16;
        memcpy(p+at,v->source,a); at+=a; memcpy(p+at,v->target,b); at+=b;
    }
    int e=op_publish(fs,FM_UNDO_FILE,p,(unsigned)size); free(p); return e;
}
int fm_undo_load(const struct fm_vfs *fs,struct fm_undo_log *log)
{
    memset(log,0,sizeof(*log)); uint8_t *p; unsigned n;
    int e=op_read(fs,FM_UNDO_FILE,&p,16+FM_ENTRIES*(16+2*FM_PATH),&n); if (e) return e;
    if (n<16 || memcmp(p,"CIUKUNDO",8) || desk_link_get32(p+8)>FM_DELETE || desk_link_get32(p+12)>FM_ENTRIES) { free(p); return -EINVAL; }
    log->kind=desk_link_get32(p+8); log->count=desk_link_get32(p+12); unsigned at=16;
    log->entries=calloc(log->count?log->count:1,sizeof(*log->entries)); if (!log->entries) e=-ENOMEM;
    for (unsigned i=0;!e && i<log->count;i++) {
        if (n-at<16) { e=-EINVAL; break; }
        unsigned a=desk_link_get32(p+at),b=desk_link_get32(p+at+4); uint64_t stamp=((uint64_t)desk_link_get32(p+at+8)|((uint64_t)desk_link_get32(p+at+12)<<32)); at+=16;
        if (!a || !b || a>=FM_PATH || b>=FM_PATH || a>n-at || b>n-at-a || memchr(p+at,0,a+b)) { e=-EINVAL; break; }
        struct fm_undo_entry *v=&log->entries[i]; memcpy(v->source,p+at,a); at+=a; memcpy(v->target,p+at,b); at+=b; v->stamp=stamp;
        if (v->source[0]!='/' || v->target[0]!='/') e=-EINVAL;
    }
    if (!e && at!=n) e=-EINVAL;
    free(p); if (e) fm_undo_free(log); return e;
}
static void undo_root(struct fm_job *j,const struct fm_frame *f)
{
    if (j->kind==FM_PROPERTIES || j->undoing || j->undo_error) return;
    const char *target=f->target;
    if (j->kind==FM_DELETE) {
        if (!j->bin || j->bin->mode!=RB_RECYCLE || !j->bin->target || !j->bin->target[j->next-1]) { j->undo_unsafe=1; return; }
        target=j->bin->target[j->next-1];
    }
    if (!*target) return;
    if (!j->undo) {
        j->undo=calloc(1,sizeof(*j->undo)); if (!j->undo) { j->undo_error=-ENOMEM; return; }
        j->undo->kind=j->kind; j->undo->entries=calloc(j->count,sizeof(*j->undo->entries));
        if (!j->undo->entries) { j->undo_error=-ENOMEM; return; }
    }
    struct fm_undo_entry *v=&j->undo->entries[j->undo->count++];
    strcpy(v->source,f->source); strcpy(v->target,target);
}
static void undo_finish(struct fm_job *j)
{
    if (j->undo_saved || (!j->record_undo && !j->undoing)) return;
    j->undo_saved=1;
    if (!fm_undo_supported(&j->fs)) { j->undo_error=-ENOSYS; return; }
    if (j->undoing) { if (j->state==FM_DONE && !j->skipped && !j->kept && !j->kept_folders) j->fs.unlink(j->fs.ctx,FM_UNDO_FILE); return; }
    if (!j->completed) return;
    /* A replacement/merge, partial tree, or kept source has no safe inverse. */
    if (j->undo_error || j->undo_unsafe || j->skipped || j->kept || j->kept_folders || j->depth) { j->fs.unlink(j->fs.ctx,FM_UNDO_FILE); return; }
    if (!j->undo) { j->fs.unlink(j->fs.ctx,FM_UNDO_FILE); return; }
    for (unsigned i=0;i<j->undo->count && !j->undo_error;i++)
        j->undo_error=op_stamp(&j->fs,j->undo->entries[i].target,&j->undo->entries[i].stamp,j->kind==FM_COPY);
    if (!j->undo_error) j->undo_error=undo_save(&j->fs,j->undo);
    if (j->undo_error) j->fs.unlink(j->fs.ctx,FM_UNDO_FILE);
}
int fm_rename_undo(struct fm_model *m,unsigned index,const char *name)
{
    if (!fm_undo_supported(&m->fs)) return fm_rename(m,index,name);
    struct fm_undo_log log={.kind=FM_MOVE,.count=1}; log.entries=calloc(1,sizeof(*log.entries)); if (!log.entries) return -ENOMEM;
    int e=fm_entry_path(m,index,log.entries[0].source);
    if (!e) e=fm_parent(log.entries[0].target,log.entries[0].source);
    if (!e) e=fm_join(log.entries[0].target,log.entries[0].target,name);
    int same=!e && !strcmp(log.entries[0].source,log.entries[0].target);
    if (!e) e=fm_rename(m,index,name);
    if (!e && !same) {
        int journal=op_stamp(&m->fs,log.entries[0].target,&log.entries[0].stamp,0);
        if (!journal) journal=undo_save(&m->fs,&log);
        if (journal) m->fs.unlink(m->fs.ctx,FM_UNDO_FILE);
    }
    fm_undo_free(&log); return e;
}
int fm_undo_start(struct fm_job *j,const struct fm_vfs *fs)
{
    if (!fm_undo_supported(fs)) return -ENOSYS;
    struct fm_undo_log log; int e=fm_undo_load(fs,&log); if (e) return e;
    if (!log.count) { fm_undo_free(&log); return -ENOENT; }
    char **paths=calloc(log.count,sizeof(*paths)); if (!paths) { fm_undo_free(&log); return -ENOMEM; }
    for (unsigned i=0;i<log.count && !e;i++) {
        uint64_t stamp; e=op_stamp(fs,log.entries[i].target,&stamp,log.kind==FM_COPY);
        if (!e && stamp!=log.entries[i].stamp) e=-EBUSY;
        struct fm_meta meta;
        if (!e && log.kind!=FM_COPY && !fm_same_path(log.entries[i].source,log.entries[i].target)) {
            int exists=fs->stat_path(fs->ctx,log.entries[i].source,&meta);
            if (!exists) e=-EEXIST; else if (exists!=-ENOENT) e=exists;
        }
        paths[i]=log.entries[i].target;
    }
    if (!e) e=log.kind==FM_DELETE ? rb_restore_job(j,fs,paths,log.count) :
        fm_job_start(j,fs,log.kind==FM_COPY ? FM_DELETE : FM_MOVE,paths,log.count,"/");
    if (!e && log.kind==FM_MOVE) {
        j->targets=calloc(log.count,sizeof(*j->targets)); if (!j->targets) e=-ENOMEM;
        for (unsigned i=0;!e && i<log.count;i++) {
            j->targets[i]=malloc(strlen(log.entries[i].source)+1);
            if (!j->targets[i]) e=-ENOMEM; else strcpy(j->targets[i],log.entries[i].source);
        }
        if (e) fm_job_destroy(j);
    }
    if (!e) j->undoing=1;
    free(paths); fm_undo_free(&log); return e;
}

int fm_bitmap(struct fm_model *m,const char *name)
{
    static const uint8_t bmp[58]={'B','M',58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,1,0,0,0,1,0,0,0,
        1,0,24,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,0};
    char path[FM_PATH]; int e=fm_join(path,m->path,name); if (!e) e=fm_write_new(&m->fs,path,bmp,sizeof(bmp)); return e;
}
int fm_shortcut(struct fm_model *m,const struct desk_launch_record *r,char out[FM_PATH])
{
    uint8_t *bytes=malloc(DESK_LINK_BYTES); char *name=malloc(FM_NAME); if (!bytes || !name) { free(bytes); free(name); return -ENOMEM; }
    unsigned n=desk_link_encode(r,bytes,DESK_LINK_BYTES); int e=n ? 0 : -EINVAL;
    char base[96]; snprintf(base,sizeof(base),"Shortcut to %.63s",r->label);
    for (char *p=base;*p;p++) if (strchr("\\/:*?\"<>|",*p)) *p='_';
    size_t length=strlen(base); while (length && (base[length-1]==' ' || base[length-1]=='.')) base[--length]=0;
    if (!e) e=fm_unique(m,base,".lnk",name);
    if (!e) e=fm_join(out,m->path,name);
    if (!e) e=fm_write_new(&m->fs,out,bytes,n);
    free(bytes); free(name); return e;
}
/* Bounded UTF-16LE strings; ANSI links accept ASCII only rather than guess a
 * Windows code page and launch the wrong path. */
static int op_string(const uint8_t *p,unsigned n,int wide,char *out,unsigned cap)
{
    unsigned at=0;
    for (unsigned i=0;i<n;i++) {
        uint32_t c=wide ? p[i*2]|(uint32_t)p[i*2+1]<<8 : p[i];
        if (!c || (!wide && c>=128)) return -EINVAL;
        if (c>=0xd800 && c<=0xdbff) {
            if (++i==n) return -EINVAL;
            uint32_t low=p[i*2]|(uint32_t)p[i*2+1]<<8; if (low<0xdc00 || low>0xdfff) return -EINVAL;
            c=0x10000+((c-0xd800)<<10)+(low-0xdc00);
        } else if (c>=0xdc00 && c<=0xdfff) return -EINVAL;
        unsigned bytes=c<128?1:c<2048?2:c<65536?3:4;
        if (at+bytes>=cap) return -ENAMETOOLONG;
        if (bytes==1) out[at++]=(char)c;
        else { out[at++]=(char)((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(c>>(6*(bytes-1)))); for (unsigned b=bytes-1;b;b--) out[at++]=(char)(0x80|((c>>(6*(b-1)))&63)); }
    }
    out[at]=0; return 0;
}
static int op_cstring(const uint8_t *p,unsigned n,unsigned offset,int wide,char *out,unsigned cap)
{
    if (offset>=n) return -EINVAL;
    unsigned units=0,step=wide?2:1;
    while (offset+units*step+step<=n) {
        const uint8_t *s=p+offset+units*step;
        if (!s[0] && (!wide || !s[1])) return op_string(p+offset,units,wide,out,cap);
        units++;
    }
    return -EINVAL;
}
static int op_relative(const char *base,const char *relative,char *out,unsigned cap)
{
    char *path=malloc(FM_PATH),*tail=malloc(FM_PATH); if (!path || !tail) { free(path); free(tail); return -ENOMEM; }
    int e=fm_posix_path(path,base);
    if (strlen(relative)>=FM_PATH) e=-ENAMETOOLONG;
    if (!e) {
        strcpy(tail,relative); for (char *p=tail;*p;p++) if (*p=='\\') *p='/';
        char *p=tail;
        while (*p && !e) {
            while (*p=='/') p++;
            if (!*p) break;
            char *end=strchr(p,'/'); if (end) *end=0;
            if (!strcmp(p,"..")) { if (!fm_drive_root(path)) e=fm_parent(path,path); }
            else if (strcmp(p,".")) e=fm_join(path,path,p);
            if (!end) break;
            p=end+1;
        }
        if (!e && strlen(path)>=cap) e=-ENAMETOOLONG;
        if (!e) strcpy(out,path);
    }
    free(path); free(tail); return e;
}
int fm_link_decode(const uint8_t *p,unsigned n,const char *source,struct desk_launch_record *r)
{
    if (!p || !r || !source) return -EINVAL;
    if (desk_link_decode(p,n,r)) return 0;
    static const uint8_t clsid[16]={1,0x14,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    if (n<76 || n>DESK_LINK_BYTES || desk_link_get32(p)!=76 || memcmp(p+4,clsid,16)) return -EINVAL;
    memset(r,0,sizeof(*r)); r->show=desk_link_get32(p+60); r->icon_index=(int32_t)desk_link_get32(p+56);
    if (r->show!=1 && r->show!=3 && r->show!=7) r->show=1;
    uint32_t flags=desk_link_get32(p+20); unsigned at=76; int e=0;
    if (flags&1) { if (n-at<2) return -EINVAL; unsigned size=p[at]|(unsigned)p[at+1]<<8; at+=2; if (size>n-at) return -EINVAL; at+=size; }
    if (flags&2) {
        if (n-at<28) return -EINVAL;
        unsigned size=desk_link_get32(p+at),header=desk_link_get32(p+at+4);
        if (size<28 || size>n-at || header<28 || header>size) return -EINVAL;
        if (desk_link_get32(p+at+8)&1) {
            unsigned offset=desk_link_get32(p+at+16); int wide=0;
            if (header>=36 && desk_link_get32(p+at+28)) { offset=desk_link_get32(p+at+28); wide=1; }
            if (offset<header) return -EINVAL;
            e=op_cstring(p+at,size,offset,wide,r->target,sizeof(r->target)); if (e) return e;
            /* LocalBasePath may already contain the suffix. Avoid duplicating it. */
            char suffix_path[260]; unsigned tail=header>=36 && desk_link_get32(p+at+32) ? desk_link_get32(p+at+32) : desk_link_get32(p+at+24);
            int tail_wide=header>=36 && desk_link_get32(p+at+32);
            if (tail) { if (tail<header) return -EINVAL; e=op_cstring(p+at,size,tail,tail_wide,suffix_path,sizeof(suffix_path)); if (e) return e;
                size_t a=strlen(r->target),b=strlen(suffix_path);
                if (b && (a<b || compare(r->target+a-b,suffix_path))) {
                    if (a+b+2>sizeof(r->target)) return -ENAMETOOLONG;
                    if (a && r->target[a-1]!='\\' && r->target[a-1]!='/') r->target[a++]='\\';
                    memcpy(r->target+a,suffix_path,b+1);
                }
            }
        }
        at+=size;
    }
    char relative[260]=""; char *fields[]={r->label,relative,r->directory,r->arguments,r->icon};
    unsigned caps[]={sizeof(r->label),sizeof(relative),sizeof(r->directory),sizeof(r->arguments),sizeof(r->icon)};
    for (unsigned i=0;i<5;i++) if (flags&(4u<<i)) {
        if (n-at<2) return -EINVAL;
        unsigned units=p[at]|(unsigned)p[at+1]<<8; at+=2; unsigned bytes=units*((flags&128)?2u:1u);
        if (bytes>n-at) return -EINVAL;
        e=op_string(p+at,units,(flags&128)!=0,fields[i],caps[i]); if (e) return e; at+=bytes;
    }
    if (!r->target[0] && relative[0]) {
        if (relative[0]=='/' || (relative[0] && relative[1]==':')) snprintf(r->target,sizeof(r->target),"%s",relative);
        else {
            char *parent=malloc(FM_PATH),*joined=malloc(FM_PATH); if (!parent || !joined) { free(parent); free(joined); return -ENOMEM; }
            e=fm_parent(parent,source);
            if (!e) e=op_relative(parent,relative,joined,FM_PATH);
            if (!e && strlen(joined)>=sizeof(r->target)) e=-ENAMETOOLONG;
            if (!e) strcpy(r->target,joined);
            free(parent); free(joined); if (e) return e;
        }
    }
    if (!r->label[0]) snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(source));
    return desk_launch_valid(r) ? 0 : -ENOTSUP;
}
int fm_link_read(const struct fm_vfs *fs,const char *path,struct desk_launch_record *r)
{
    uint8_t *p; unsigned n; int e=op_read(fs,path,&p,DESK_LINK_BYTES,&n); if (e) return e;
    e=fm_link_decode(p,n,path,r); free(p); return e;
}
int fm_link_selection(struct fm_model *m,char *const *paths,unsigned count)
{
    struct desk_launch_record *r=calloc(1,sizeof(*r)); char *out=malloc(FM_PATH);
    if (!r || !out) { free(r); free(out); return -ENOMEM; }
    struct fm_model *folder=m->searching?calloc(1,sizeof(*folder)):NULL;
    if (m->searching && !folder) { free(r); free(out); return -ENOMEM; }
    if (folder) fm_init(folder,&m->fs);
    int e=0;
    for (unsigned i=0;!e && i<count;i++) {
        memset(r,0,sizeof(*r)); r->show=1; snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(paths[i]));
        if (strlen(paths[i])>=sizeof(r->target)) { e=-ENAMETOOLONG; break; }
        strcpy(r->target,paths[i]);
        if (folder) e=fm_parent(folder->path,paths[i]);
        if (!e) e=fm_shortcut(folder?folder:m,r,out);
    }
    if (folder) { fm_destroy(folder); free(folder); }
    free(r); free(out); return e;
}

/* Registry requests use the Win32 service's existing records verbatim. */
static int op_reg_error(uint32_t e)
{ switch(e) { case 0:return 0; case 2:case 3:case 259:return -ENOENT; case 5:return -EACCES; case 8:return -ENOMEM; case 234:return -EOVERFLOW; case 1062:return -EPIPE; default:return -EINVAL; } }
static int op_reg_typed(unsigned type,unsigned op,uint32_t key,const char *path,const char *name,unsigned index,const uint8_t *data,unsigned bytes,struct w32r_reply **out)
{
    if (!fm_registry_exchange) return -ENOSYS;
    size_t a=strlen(path?path:"")+1,b=strlen(name?name:"")+1;
    if (a>W32R_PATH || b>W32R_NAME || bytes>W32R_DATA) return -E2BIG;
    unsigned size=(unsigned)(sizeof(struct w32r_request)+a+b+bytes);
    uint8_t *wire=calloc(1,size); struct w32r_reply *reply=malloc(W32R_MAX);
    if (!wire || !reply) { free(wire); free(reply); return -ENOMEM; }
    struct w32r_request request={.version=W32R_VERSION,.op=op,.key=key,.access=op==W32R_CREATE?0x2001fu:0x20019u,.index=index,.type=op==W32R_SET?type:0,.path_bytes=(uint32_t)a,.name_bytes=(uint32_t)b,.data_bytes=bytes};
    memcpy(wire,&request,sizeof(request)); memcpy(wire+sizeof(request),path?path:"",a); memcpy(wire+sizeof(request)+a,name?name:"",b);
    if (bytes) memcpy(wire+sizeof(request)+a+b,data,bytes);
    uint32_t used=0,error=fm_registry_exchange(wire,size,reply,W32R_MAX,&used); free(wire);
    int e=op_reg_error(error);
    if (!e && (used<sizeof(*reply) || reply->version!=W32R_VERSION || reply->data_bytes>W32R_DATA || reply->name_bytes>=W32R_NAME ||
        used!=sizeof(*reply)+(reply->name_bytes?reply->name_bytes+1:0)+reply->data_bytes)) e=-EINVAL;
    if (!e) e=op_reg_error(reply->error);
    if (e) free(reply); else *out=reply;
    return e;
}
static int op_reg(unsigned op,uint32_t key,const char *path,const char *name,unsigned index,const uint8_t *data,unsigned bytes,struct w32r_reply **out)
{ return op_reg_typed(1,op,key,path,name,index,data,bytes,out); }
static void op_reg_close(uint32_t key)
{ struct w32r_reply *r=NULL; (void)op_reg(W32R_CLOSE,key,"","",0,NULL,0,&r); free(r); }
int fm_registry_root_get(uint32_t root,const char *key,const char *name,char *out,unsigned cap)
{
    struct w32r_reply *r=NULL; int e=op_reg(W32R_OPEN,root,key,"",0,NULL,0,&r); if (e) return e;
    uint32_t handle=r->key; free(r); r=NULL;
    e=op_reg(W32R_QUERY,handle,"",name,0,NULL,0,&r);
    if (!e) {
        const uint8_t *p=(const uint8_t *)(r+1);
        if (r->type!=1 || r->name_bytes || r->data_bytes<2 || (r->data_bytes&1) || p[r->data_bytes-1] || p[r->data_bytes-2]) e=-EINVAL;
        else e=op_string(p,r->data_bytes/2-1,1,out,cap);
    }
    free(r); op_reg_close(handle); return e;
}
int fm_registry_root_set(uint32_t root,const char *key,const char *name,const char *value)
{
    /* Native UI commands are UTF-8; encode into the same UTF-16LE REG_SZ
     * bytes used by RegSetValueExW. No private association store. */
    unsigned length=(unsigned)strlen(value); if (!desk_link_utf8((const uint8_t *)value,length)) return -EINVAL;
    if (length>(W32R_DATA-2)/2) return -E2BIG;
    uint8_t *data=calloc(length*2+2,1); if (!data) return -ENOMEM;
    unsigned at=0;
    for (unsigned i=0;i<length;) {
        uint32_t c=(uint8_t)value[i++]; unsigned extra=0;
        if (c>=0xc2 && c<0xe0) { extra=1; c&=31; }
        else if (c>=0xe0 && c<0xf0) { extra=2; c&=15; }
        else if (c>=0xf0 && c<=0xf4) { extra=3; c&=7; }
        else if (c>=128) { free(data); return -EINVAL; }
        if (extra>length-i) { free(data); return -EINVAL; }
        while (extra--) { unsigned b=(uint8_t)value[i++]; if ((b&192)!=128) { free(data); return -EINVAL; } c=(c<<6)|(b&63); }
        if (c>0x10ffff || (c>=0xd800 && c<=0xdfff)) { free(data); return -EINVAL; }
        if (c>=0x10000) { unsigned high=0xd800+((c-0x10000)>>10); data[at++]=(uint8_t)high; data[at++]=(uint8_t)(high>>8); c=0xdc00+((c-0x10000)&1023); }
        data[at++]=(uint8_t)c; data[at++]=(uint8_t)(c>>8);
    }
    struct w32r_reply *r=NULL; int e=op_reg(W32R_CREATE,root,key,"",0,NULL,0,&r);
    if (!e) { uint32_t handle=r->key; free(r); r=NULL; e=op_reg(W32R_SET,handle,"",name,0,data,at+2,&r); free(r); op_reg_close(handle); }
    free(data); return e;
}
int fm_registry_get(const char *key,const char *name,char *out,unsigned cap)
{ return fm_registry_root_get(W32R_HKCR,key,name,out,cap); }
int fm_registry_set(const char *key,const char *name,const char *value)
{ return fm_registry_root_set(W32R_HKCR,key,name,value); }
int fm_registry_enum(const char *key,unsigned index,char *out,unsigned cap)
{
    struct w32r_reply *r=NULL; int e=op_reg(W32R_OPEN,W32R_HKCR,key,"",0,NULL,0,&r); if (e) return e;
    uint32_t handle=r->key; free(r); r=NULL;
    e=op_reg(W32R_ENUM_KEY,handle,"","",index,NULL,0,&r);
    if (!e) {
        const char *p=(const char *)(r+1);
        if (!r->name_bytes || r->name_bytes>=cap || p[r->name_bytes] || memchr(p,0,r->name_bytes) || !desk_link_utf8((const uint8_t *)p,r->name_bytes)) e=-EINVAL;
        else memcpy(out,p,r->name_bytes+1);
    }
    free(r); op_reg_close(handle); return e;
}
static int op_extension(const char *path,char out[64])
{
    const char *p=strrchr(fm_basename(path),'.'); if (!p || !p[1] || strlen(p)>=64) return -ENOENT;
    for (unsigned i=0;p[i];i++) out[i]=(char)fold((uint8_t)p[i]);
    out[strlen(p)]=0; return 0;
}
/* One query per distinct extension per explicit scan, never per repaint. */
void fm_registered_types(struct fm_entry *entries,unsigned count)
{
    if (!fm_registry_exchange) return;
    struct type_name { char extension[64],label[80]; };
    struct type_name *cache=calloc(128,sizeof(*cache)); if (!cache) return;
    unsigned used=0;
    for (unsigned i=0;i<count;i++) {
        struct fm_entry *e=&entries[i]; e->registered_type[0]=0;
        if (e->meta.type!=FM_FILE) continue;
        char ext[64]; if (op_extension(e->name,ext)) continue;
        unsigned k=0; while (k<used && compare(ext,cache[k].extension)) k++;
        if (k==used) {
            if (used==128) continue;
            strcpy(cache[k].extension,ext); used++;
            char cls[260];
            if (!fm_registry_get(ext,"",cls,sizeof(cls)) && cls[0] && !strchr(cls,'\\') && !strchr(cls,'/'))
                (void)fm_registry_get(cls,"",cache[k].label,sizeof(cache[k].label));
        }
        memcpy(e->registered_type,cache[k].label,sizeof(e->registered_type));
    }
    free(cache);
}
int fm_associate(const char *path,const char *program)
{
    char ext[64],cls[96],key[160]; int e=op_extension(path,ext); if (e) return e;
    if (program[0]!='/' && !(program[0] && program[1]==':')) return -EINVAL;
    if (strchr(program,'"') || strlen(program)>260) return -EINVAL;
    snprintf(cls,sizeof(cls),"CiukiOS%s",ext); snprintf(key,sizeof(key),"%s\\shell\\open\\command",cls);
    char value[300]; snprintf(value,sizeof(value),"\"%s\" \"%%1\"",program);
    e=fm_registry_set(key,"",value);
    if (!e) { snprintf(key,sizeof(key),"%s\\shell",cls); e=fm_registry_set(key,"","open"); }
    if (!e) e=fm_registry_set(cls,"",ext+1);
    if (!e) e=fm_registry_set(ext,"",cls);
    return e;
}
int fm_command_record(const char *command,const char *file,struct desk_launch_record *r)
{
    memset(r,0,sizeof(*r)); r->show=1;
    snprintf(r->label,sizeof(r->label),"%.63s",!strncasecmp(file,"http://",7) || !strncasecmp(file,"https://",8)?"Web page":fm_basename(file));
    const char *p=command; while (*p==' ' || *p=='\t') p++;
    int quoted=*p=='"'; if (quoted) p++; unsigned at=0;
    while (*p && (quoted ? *p!='"' : *p!=' ' && *p!='\t')) {
        if (at+1>=sizeof(r->target) || *p=='%') return -EINVAL;
        r->target[at++]=*p++;
    }
    if (quoted && *p++!='"') return -EINVAL;
    if (*p && *p!=' ' && *p!='\t') return -EINVAL;
    while (*p==' ' || *p=='\t') p++;
    at=0;
    while (*p) {
        const char *s=p; unsigned n=1;
        if (*p=='%') {
            p++; if (*p=='1' || *p=='l' || *p=='L') { s=file; n=(unsigned)strlen(file); }
            else if (*p=='%') s="%";
            else if (*p=='*') { s=""; n=0; }
            else return -EINVAL;
        }
        if (at+n>=sizeof(r->arguments)) return -E2BIG;
        memcpy(r->arguments+at,s,n); at+=n; p++;
    }
    return desk_launch_valid(r) ? 0 : -EINVAL;
}
int fm_browser_default(void)
{
    /* The command is installed before the extension points to it. Every
     * mutation goes through G8's shared HKCR service, including Win32 reads. */
    int e=fm_associate("page.htm","/Programs/netsurf");
    if(!e)e=fm_associate("page.html","/Programs/netsurf");
    static const char *const protocols[]={"http","https"};
    for(unsigned i=0;!e && i<2;i++) {
        char key[80];snprintf(key,sizeof(key),"%s\\shell\\open\\command",protocols[i]);
        e=fm_registry_set(key,"","\"/Programs/netsurf\" \"%1\"");
        if(!e) { snprintf(key,sizeof(key),"%s\\shell",protocols[i]);e=fm_registry_set(key,"","open"); }
        if(!e)e=fm_registry_set(protocols[i],"URL Protocol","");
    }
    return e;
}
static int op_url_association(const char *url,struct desk_launch_record *r)
{
    char command[1536];
    const char *key=!strncasecmp(url,"https://",8)?"https\\shell\\open\\command":"http\\shell\\open\\command";
    int e=fm_registry_get(key,"",command,sizeof(command));
    if(e==-ENOENT || e==-ENOSYS || e==-EPIPE)strcpy(command,"\"/Programs/netsurf\" \"%1\"");
    else if(e)return e;
    return fm_command_record(command,url,r);
}
int fm_association(const char *path,const char *verb,struct desk_launch_record *r)
{
    char ext[64],cls[260],key[400],action[64],command[1536]; int e=op_extension(path,ext); if (e) return e;
    e=fm_registry_get(ext,"",cls,sizeof(cls)); if (e) return e;
    if (!cls[0] || strchr(cls,'\\') || strchr(cls,'/')) return -EINVAL;
    int implicit=0;
    if (!verb || !*verb) {
        snprintf(key,sizeof(key),"%s\\shell",cls); e=fm_registry_get(key,"",action,sizeof(action));
        if (e==-ENOENT || (!e && !action[0])) { strcpy(action,"open"); implicit=1; } else if (e) return e;
        verb=action;
    }
    if (strchr(verb,'\\') || strchr(verb,'/')) return -EINVAL;
    snprintf(key,sizeof(key),"%s\\shell\\%s\\command",cls,verb);
    e=fm_registry_get(key,"",command,sizeof(command));
    if (e==-ENOENT && implicit) {
        snprintf(key,sizeof(key),"%s\\shell",cls); e=fm_registry_enum(key,0,action,sizeof(action));
        if (!e && (strchr(action,'\\') || strchr(action,'/'))) e=-EINVAL;
        if (!e) { snprintf(key,sizeof(key),"%s\\shell\\%s\\command",cls,action); e=fm_registry_get(key,"",command,sizeof(command)); }
    }
    if (e) return e==-ENOENT ? -ENOEXEC : e;
    if (!compare(cls,"txtfile") && !strcmp(command,"ciuknote.exe \"%1\""))
        strcpy(command,"\"/Programs/ciuknote\" \"%1\"");
    return fm_command_record(command,path,r);
}
static int op_pif(const struct fm_vfs *fs,const char *path,struct desk_launch_record *r)
{
    uint8_t *p; unsigned n; int e=op_read(fs,path,&p,DESK_LINK_BYTES,&n); if (e) return e;
    /* Windows PIF's original 369-byte standard block: executable[63] at
     * 0x24, working directory[64] at 0x65, parameters[64] at 0xa5. */
    if (n<369 || !memchr(p+36,0,63) || !memchr(p+101,0,64) || !memchr(p+165,0,64)) { free(p); return -EINVAL; }
    memset(r,0,sizeof(*r)); r->show=1; snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(path));
    memcpy(r->target,p+36,63); memcpy(r->directory,p+101,64); memcpy(r->arguments,p+165,64);
    if (n>369) {
        unsigned at=369,seen[64],count=0;
        if (n-at<22 || memcmp(p+at,"MICROSOFT PIFEX",15)) e=-EINVAL;
        while (!e) {
            if (count==64 || n-at<22) { e=-EINVAL; break; }
            for (unsigned i=0;i<count;i++) if (seen[i]==at) e=-EINVAL;
            if (e) break;
            seen[count++]=at;
            unsigned next=p[at+16]|(unsigned)p[at+17]<<8;
            unsigned data=p[at+18]|(unsigned)p[at+19]<<8,size=p[at+20]|(unsigned)p[at+21]<<8;
            if (!memcmp(p+at,"WINDOWS 386",11)) {
                if (size<104 || data>n || size>n-data || !memchr(p+data+40,0,64)) { e=-EINVAL; break; }
                memcpy(r->arguments,p+data+40,64);
            }
            if (next==0xffff) break;
            if (next<369 || next>n) { e=-EINVAL; break; }
            at=next;
        }
    }
    free(p);
    if (!e && r->target[0]!='/' && !(r->target[0] && r->target[1]==':')) {
        char *base=malloc(FM_PATH),*program=malloc(260); if (!base || !program) e=-ENOMEM;
        else {
            strcpy(program,r->target);
            e=r->directory[0] ? fm_posix_path(base,r->directory) : fm_parent(base,path);
            if (!e) e=op_relative(base,program,r->target,sizeof(r->target));
        }
        free(base); free(program);
    }
    return e ? e : desk_launch_valid(r) ? 0 : -ENOTSUP;
}
int fm_launch_resolve(const struct fm_vfs *fs,const char *path,const char *verb,struct desk_launch_record *r)
{
    char *current=malloc(FM_PATH); struct desk_launch_record *link=calloc(1,sizeof(*link));
    if (!current || !link) { free(current); free(link); return -ENOMEM; }
    int e=0; char arguments[1024]="",directory[260]=""; uint32_t show=1;
    if (strlen(path)>=FM_PATH) e=-ENAMETOOLONG; else strcpy(current,path);
    for (unsigned depth=0;!e && depth<9;depth++) {
        memset(r,0,sizeof(*r)); r->show=show;
        if (!strncasecmp(current,"http://",7) || !strncasecmp(current,"https://",8)) {
            if(strchr(current,'"')) { e=-EINVAL;break; }
            e=op_url_association(current,r);break;
        }
        e=fm_posix_path(current,current); if (e) break;
        struct fm_meta meta; e=fs->stat_path(fs->ctx,current,&meta); if (e) break;
        if (meta.type==FM_DIR) {
            strcpy(r->label,"Files"); strcpy(r->target,"/Programs/files");
            if (strlen(current)+3>sizeof(r->arguments)) { e=-E2BIG; break; }
            snprintf(r->arguments,sizeof(r->arguments),"\"%s\"",current); e=1; break;
        }
        const char *ext=suffix(fm_basename(current));
        if (ext && (!compare(ext,"lnk") || !compare(ext,"pif"))) {
            if (depth==8) { e=-EINVAL; break; }
            e=!compare(ext,"lnk") ? fm_link_read(fs,current,link) : op_pif(fs,current,link); if (e) break;
            if (arguments[0] && link->arguments[0]) { e=-E2BIG; break; }
            if (link->arguments[0]) strcpy(arguments,link->arguments);
            if (link->directory[0]) strcpy(directory,link->directory);
            show=link->show; strcpy(current,link->target); continue;
        }
        int executable=meta.exec || (ext && (!compare(ext,"exe") || !compare(ext,"com") || !compare(ext,"bat")));
        if (!executable && !ext) {
            int fd=fs->open(fs->ctx,current,0); if (fd<0) { e=fd; break; }
            uint8_t h[4]; unsigned used=0;
            while (used<sizeof(h)) {
                int n=fs->read(fs->ctx,fd,h+used,sizeof(h)-used);
                if (n==-EINTR) continue;
                if (n<=0) { if (n<0) e=n; break; }
                used+=(unsigned)n;
            }
            int closed=fs->close(fs->ctx,fd); if (!e) e=closed;
            if (e<0) break;
            executable=used==4 && !memcmp(h,"\177ELF",4);
        }
        if (executable) {
            if (strlen(current)>=sizeof(r->target)) { e=-ENAMETOOLONG; break; }
            snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(current)); strcpy(r->target,current);
        } else {
            e=fm_association(current,verb,r);
            if (e==-ENOENT || e==-ENOSYS || e==-EPIPE) {
                const struct desk_catalog_entry *cat=desk_catalog_file(current);
                struct fm_entry entry={0}; snprintf(entry.name,sizeof(entry.name),"%s",fm_basename(current)); entry.meta=meta;
                const char *program=cat ? cat->name : fm_kind(&entry)==FM_KIND_TEXT || fm_kind(&entry)==FM_KIND_SCRIPT ? "ciuknote" : NULL;
                if (!program) { e=-ENOEXEC; break; }
                snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(current)); snprintf(r->target,sizeof(r->target),"/Programs/%s",program);
                if (strchr(current,'"') || strlen(current)+3>sizeof(r->arguments)) { e=-E2BIG; break; }
                snprintf(r->arguments,sizeof(r->arguments),"\"%s\"",current); e=0;
            }
            if (e) break;
        }
        if (arguments[0]) {
            size_t a=strlen(r->arguments),b=strlen(arguments);
            if (a+b+2>sizeof(r->arguments)) { e=-E2BIG; break; }
            if (a) r->arguments[a++]=' ';
            memcpy(r->arguments+a,arguments,b+1);
        }
        strcpy(r->directory,directory); r->show=show; break;
    }
    if (e==-ENOEXEC && strlen(current)<sizeof(r->target)) strcpy(r->target,current);
    if (e>=0) {
        int converted=fm_posix_path(current,r->target);
        if (!converted && strlen(current)>=sizeof(r->target)) converted=-ENAMETOOLONG;
        if (!converted) strcpy(r->target,current);
        if (!converted && r->directory[0]) {
            converted=fm_posix_path(current,r->directory);
            if (!converted && strlen(current)>=sizeof(r->directory)) converted=-ENAMETOOLONG;
            if (!converted) strcpy(r->directory,current);
        }
        if (converted) e=converted;
    }
    free(current); free(link); return e;
}
int fm_shellnew(struct fm_model *m,const char *ext,char out[FM_PATH])
{
    if (!ext || ext[0]!='.' || !ext[1] || strlen(ext)>=64 || strchr(ext,'/') || strchr(ext,'\\')) return -EINVAL;
    char key[96],template_path[FM_PATH],name[FM_NAME]; snprintf(key,sizeof(key),"%s\\ShellNew",ext);
    int e=fm_registry_get(key,"FileName",template_path,sizeof(template_path));
    uint8_t *data=NULL; unsigned n=0;
    if (e==-ENOENT) { char value[8]; e=fm_registry_get(key,"NullFile",value,sizeof(value)); }
    else if (!e) {
        char path[FM_PATH];
        if (template_path[0]!='/' && !(template_path[0] && template_path[1]==':')) e=fm_join(path,"/Windows/ShellNew",template_path);
        else e=fm_posix_path(path,template_path);
        if (!e) e=op_read(&m->fs,path,&data,1024*1024,&n);
    }
    if (!e) e=fm_unique(m,"New Document",ext,name);
    if (!e) e=fm_join(out,m->path,name);
    if (!e) e=fm_write_new(&m->fs,out,data,n);
    free(data); return e;
}

int fm_filetype_get(struct fm_filetype *t)
{
    char key[400]; int e=fm_registry_get(t->extension,"",t->progid,sizeof(t->progid));
    if (e) return e;
    if (!t->progid[0] || strchr(t->progid,'\\') || strchr(t->progid,'/')) return -EINVAL;
    t->description[0]=t->icon[0]=0; strcpy(t->verb,"open"); t->edit_flags=0;
    e=fm_registry_get(t->progid,"",t->description,sizeof(t->description)); if (e && e!=-ENOENT) return e;
    snprintf(key,sizeof(key),"%s\\DefaultIcon",t->progid);
    e=fm_registry_get(key,"",t->icon,sizeof(t->icon)); if (e && e!=-ENOENT) return e;
    snprintf(key,sizeof(key),"%s\\shell",t->progid);
    e=fm_registry_get(key,"",t->verb,sizeof(t->verb)); if (e && e!=-ENOENT) return e;
    static const char *const verbs[]={"open","edit","print"};
    for (unsigned i=0;i<3;i++) {
        t->commands[i][0]=0; snprintf(key,sizeof(key),"%s\\shell\\%s\\command",t->progid,verbs[i]);
        e=fm_registry_get(key,"",t->commands[i],sizeof(t->commands[i])); if (e && e!=-ENOENT) return e;
    }
    struct w32r_reply *r=NULL; e=op_reg(W32R_OPEN,W32R_HKCR,t->progid,"",0,NULL,0,&r);
    if (e) return e;
    uint32_t handle=r->key; free(r); r=NULL;
    e=op_reg(W32R_QUERY,handle,"","EditFlags",0,NULL,0,&r);
    if (!e) { if ((r->type!=4 && r->type!=3) || r->data_bytes!=4 || r->name_bytes) e=-EINVAL; else t->edit_flags=desk_link_get32((const uint8_t *)(r+1)); }
    free(r); op_reg_close(handle); return e==-ENOENT?0:e;
}
int fm_filetype_set(const struct fm_filetype *t)
{
    if (t->extension[0]!='.' || !t->extension[1] || strchr(t->extension,'\\') || strchr(t->extension,'/') ||
        !t->progid[0] || strchr(t->progid,'\\') || strchr(t->progid,'/') || !t->verb[0] || strchr(t->verb,'\\') || strchr(t->verb,'/')) return -EINVAL;
    char key[400]; int e=fm_registry_set(t->progid,"",t->description);
    snprintf(key,sizeof(key),"%s\\DefaultIcon",t->progid); if (!e) e=fm_registry_set(key,"",t->icon);
    snprintf(key,sizeof(key),"%s\\shell",t->progid); if (!e) e=fm_registry_set(key,"",t->verb);
    static const char *const verbs[]={"open","edit","print"};
    for (unsigned i=0;i<3 && !e;i++) {
        snprintf(key,sizeof(key),"%s\\shell\\%s\\command",t->progid,verbs[i]); e=fm_registry_set(key,"",t->commands[i]);
    }
    struct w32r_reply *r=NULL;
    if (!e) e=op_reg(W32R_CREATE,W32R_HKCR,t->progid,"",0,NULL,0,&r);
    if (!e) {
        uint32_t handle=r->key; free(r); r=NULL; uint8_t flags[4]; desk_link_put32(flags,t->edit_flags);
        e=op_reg_typed(3,W32R_SET,handle,"","EditFlags",0,flags,4,&r); free(r); op_reg_close(handle);
    }
    if (!e) e=fm_registry_set(t->extension,"",t->progid);
    return e;
}

int fm_launch_arguments(char *text,char **argv,unsigned cap)
{
    char *read=text,*write=text;unsigned count=0;
    while(*read){
        while(*read==' ' || *read=='\t')read++;
        if(!*read)break;
        if(count==cap)return -E2BIG;
        argv[count++]=write;int quoted=0;
        while(*read && (quoted || (*read!=' ' && *read!='\t'))){
            unsigned slashes=0;while(*read=='\\'){slashes++;read++;}
            if(*read=='"'){
                for(unsigned j=0;j<slashes/2;j++)*write++='\\';
                if(slashes%2)*write++='"';else quoted=!quoted;
                read++;
            }else{
                while(slashes--)*write++='\\';
                if(*read)*write++=*read++;
            }
        }
        if(quoted)return -EINVAL;
        while(*read==' ' || *read=='\t')read++;
        *write++=0;
    }
    return (int)count;
}

/* Desktop selection hand-off is independent of both clipboard and drag data.
 * Exclusive names keep two queued Files launches from exchanging selections. */
int fm_shell_selection_write(const struct fm_vfs *fs,char *const *paths,unsigned count,char out[FM_PATH])
{
    static unsigned serial;
    if (!count || count>FM_ENTRIES) return -EINVAL;
    unsigned size=12;
    for (unsigned i=0;i<count;i++) {
        size_t n=strlen(paths[i]); if (!n || n>=FM_PATH || paths[i][0]!='/') return -EINVAL;
        size+=(unsigned)n+4;
    }
    uint8_t *data=malloc(size); if (!data) return -ENOMEM;
    memcpy(data,"CIUKSEL1",8); desk_link_put32(data+8,count); unsigned at=12;
    for (unsigned i=0;i<count;i++) { unsigned n=(unsigned)strlen(paths[i]); desk_link_put32(data+at,n); at+=4; memcpy(data+at,paths[i],n); at+=n; }
    int e=fs->mkdir(fs->ctx,"/system/settings"); if (e==-EEXIST) e=0;
    if (!e) {
        e=-EEXIST;
        for (unsigned attempt=0;e==-EEXIST && attempt<128;attempt++) {
            unsigned id=__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED);
            snprintf(out,FM_PATH,"/system/settings/selection-%u.dat",id); e=fm_write_new(fs,out,data,size);
        }
    }
    free(data); return e;
}
int fm_shell_selection_read(const struct fm_vfs *fs,const char *path,char ***out,unsigned *count)
{
    *out=NULL; *count=0; uint8_t *data; unsigned size;
    int e=op_read(fs,path,&data,12+FM_ENTRIES*(FM_PATH+4),&size); if (e) return e;
    unsigned n=size>=12?desk_link_get32(data+8):0;
    if (size<12 || memcmp(data,"CIUKSEL1",8) || !n || n>FM_ENTRIES) { free(data); return -EINVAL; }
    char **paths=calloc(n,sizeof(*paths)); if (!paths) { free(data); return -ENOMEM; }
    unsigned at=12;
    for (unsigned i=0;i<n && !e;i++) {
        if (size-at<4) { e=-EINVAL; break; }
        unsigned bytes=desk_link_get32(data+at); at+=4;
        if (!bytes || bytes>=FM_PATH || bytes>size-at || data[at]!='/' || memchr(data+at,0,bytes)) { e=-EINVAL; break; }
        paths[i]=malloc(bytes+1); if (!paths[i]) { e=-ENOMEM; break; }
        memcpy(paths[i],data+at,bytes); paths[i][bytes]=0; at+=bytes;
    }
    if (!e && at!=size) e=-EINVAL;
    free(data);
    if (e) { for (unsigned i=0;i<n;i++) free(paths[i]); free(paths); } else { *out=paths; *count=n; }
    return e;
}
