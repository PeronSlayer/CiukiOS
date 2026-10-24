/* CiukiOS backup stream v1. No QIC interoperability is claimed.
 * SPDX-License-Identifier: MIT */
#include "backup.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define BK_HEADER 32u
#define BK_RECORD 32u
#define BK_CHUNK 32768u
struct bk_entry { char path[FM_PATH]; uint64_t size; int64_t time; uint32_t crc, type; };
struct bk_set { struct bk_entry *entry; unsigned count; uint32_t id, base, mode; };
struct bk_work {
    const struct fm_vfs *fs; const struct bk_callbacks *cb;
    struct bk_set set;
    uint8_t *buffer;
    const char *archive, *base;
    struct bk_result *result;
};
static uint32_t rd32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p)|(uint64_t)rd32(p+4)<<32; }
static void wr32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static void wr64(uint8_t *p,uint64_t n) { wr32(p,(uint32_t)n); wr32(p+4,(uint32_t)(n>>32)); }
static uint32_t crc32(uint32_t crc,const void *data,size_t n)
{
    const uint8_t *p=data;
    for(size_t i=0;i<n;i++) { crc^=p[i]; for(unsigned b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1))); }
    return crc;
}
static int cancelled(struct bk_work *w) { return w->cb && w->cb->cancel && w->cb->cancel(w->cb->ctx) ? -ECANCELED : 0; }
static void progress(struct bk_work *w,const char *path)
{ if(w->cb && w->cb->progress)w->cb->progress(w->cb->ctx,path,w->result); }
/* No dot components, device trees, embedded NULs, backslashes, colons or
 * duplicate separators. Archive paths are absolute, canonical native paths. */
static int safe_path(const char *p)
{
    if(!p || p[0]!='/' || !p[1] || strlen(p)>=FM_PATH || strpbrk(p,"\\:"))return 0;
    if(!strncmp(p,"/dev",4) && (!p[4] || p[4]=='/'))return 0;
    if(!strncmp(p,"/proc",5) && (!p[5] || p[5]=='/'))return 0;
    if(!strncmp(p,"/sys",4) && (!p[4] || p[4]=='/'))return 0;
    for(const char *s=p+1;*s;) {
        size_t n=strcspn(s,"/"); if(!n || (n==1 && s[0]=='.') || (n==2 && s[0]=='.' && s[1]=='.'))return 0;
        s+=n; if(*s && !*++s)return 0;
    }
    return 1;
}
/* lstat adapter is required on hosts. Native CiukiOS has no symlinks.
 * Validate every existing component, not only the final path. */
static int parents(struct bk_work *w,const char *path,int create)
{
    char p[FM_PATH]; snprintf(p,sizeof(p),"%s",path);
    for(char *s=p+1;*s;s++) if(*s=='/') {
        *s=0; struct fm_meta m; int e=w->fs->stat_path(w->fs->ctx,p,&m);
        if(e==-ENOENT && create)e=w->fs->mkdir(w->fs->ctx,p);
        else if(!e && m.type!=FM_DIR)e=-ENOTDIR;
        *s='/'; if(e)return e;
    }
    return 0;
}
static int io(struct bk_work *w,int fd,void *data,unsigned n,int write)
{
    uint8_t *p=data;
    while(n) {
        int e=cancelled(w); if(e)return e;
        int done=write ? w->fs->write(w->fs->ctx,fd,p,n) : w->fs->read(w->fs->ctx,fd,p,n);
        if(done<0)return done;
        if(!done || (unsigned)done>n)return -EIO;
        p+=done; n-=(unsigned)done;
    }
    return 0;
}
static void record(uint8_t r[BK_RECORD],const struct bk_entry *e)
{
    memset(r,0,BK_RECORD); wr32(r,(uint32_t)strlen(e->path)); wr32(r+4,e->type);
    wr64(r+8,e->size); wr64(r+16,(uint64_t)e->time); wr32(r+24,e->crc);
    uint32_t crc=crc32(~0u,r,28); crc=crc32(crc,e->path,strlen(e->path)); wr32(r+28,~crc);
}
static int open_read(struct bk_work *w,const char *p)
{
    int e=parents(w,p,0); if(e)return e;
    struct fm_meta m; e=w->fs->stat_path(w->fs->ctx,p,&m);
    if(e)return e;
    return m.type!=FM_FILE ? -EINVAL : w->fs->open(w->fs->ctx,p,0);
}
static int checksum(struct bk_work *w,struct bk_entry *e,int output)
{
    int fd=open_read(w,e->path); if(fd<0)return fd;
    uint64_t left=e->size; uint32_t crc=~0u; int error=0;
    while(left && !error) {
        unsigned n=left<BK_CHUNK ? (unsigned)left : BK_CHUNK;
        error=io(w,fd,w->buffer,n,0);
        if(!error) { crc=crc32(crc,w->buffer,n); if(output>=0)error=io(w,output,w->buffer,n,1); }
        left-=n;
    }
    uint8_t extra;
    if(!error) { int n=w->fs->read(w->fs->ctx,fd,&extra,1); if(n)error=n<0 ? n : -EIO; }
    struct fm_meta m;
    if(!error) { error=w->fs->stat_path(w->fs->ctx,e->path,&m); if(!error && (m.size!=e->size || m.modified!=e->time))error=-EIO; }
    int close=w->fs->close(w->fs->ctx,fd); if(!error)error=close;
    if(!error) { if(output>=0 && e->crc!=~crc)error=-EIO; else e->crc=~crc; }
    return error;
}
static int add(struct bk_work *,const char *,unsigned);
struct bk_walk { struct bk_work *w; const char *parent; unsigned depth; };
static int child(const char *name,const struct fm_meta *m,void *ctx)
{
    (void)m; struct bk_walk *walk=ctx; char path[FM_PATH];
    if(!strcmp(name,".") || !strcmp(name,".."))return 0;
    if(strchr(name,'/'))return -EINVAL;
    int e=fm_join(path,walk->parent,name);
    if(!e && (!safe_path(path) || !strcmp(path,"/mnt")))return 0;
    return e ? e : add(walk->w,path,walk->depth+1);
}
static int add(struct bk_work *w,const char *path,unsigned depth)
{
    int e=cancelled(w); if(e)return e;
    if(!strcmp(path,"/")) { struct bk_walk walk={w,path,depth}; return w->fs->list(w->fs->ctx,path,FM_LIST_METADATA,child,&walk); }
    if(!safe_path(path) || depth>FM_DEPTH)return -EINVAL;
    if(fm_same_path(path,w->archive) || (w->base && fm_same_path(path,w->base)))return 0;
    for(unsigned i=0;i<w->set.count;i++)if(fm_same_path(path,w->set.entry[i].path))return 0;
    if(w->set.count==BK_MAX_FILES)return -E2BIG;
    if((e=parents(w,path,0)))return e;
    struct fm_meta m; e=w->fs->stat_path(w->fs->ctx,path,&m); if(e)return e;
    if(m.type!=FM_FILE && m.type!=FM_DIR)return -EINVAL;
    struct bk_entry *entry=&w->set.entry[w->set.count++];
    snprintf(entry->path,sizeof(entry->path),"%s",path); entry->type=m.type; entry->time=m.modified;
    entry->size=m.type==FM_FILE ? m.size : 0;
    if(m.type==FM_FILE)return checksum(w,entry,-1);
    struct bk_walk walk={w,path,depth};
    return w->fs->list(w->fs->ctx,path,FM_LIST_METADATA,child,&walk);
}
/* Validate header, every path/record/data checksum and exact EOF. The
 * catalogue itself has a CRC, including record order and differential base. */
static int load(struct bk_work *w,const char *path,struct bk_set *set)
{
    int fd=open_read(w,path); if(fd<0)return fd;
    uint8_t h[BK_HEADER]; int e=io(w,fd,h,sizeof(h),0);
    if(!e && (memcmp(h,"CIUKIBK1",8) || rd32(h+8)!=1 || rd32(h+12)>BK_MAX_FILES || rd32(h+24)>1 || rd32(h+28)))e=-EINVAL;
    if(!e) { set->count=rd32(h+12); set->id=rd32(h+16); set->base=rd32(h+20); set->mode=rd32(h+24); }
    uint32_t catalogue=crc32(~0u,h+20,8);
    for(unsigned i=0;!e && i<set->count;i++) {
        struct bk_entry *entry=&set->entry[i]; uint8_t r[BK_RECORD];
        e=io(w,fd,r,sizeof(r),0); if(e)break;
        unsigned n=rd32(r); entry->type=rd32(r+4); entry->size=rd64(r+8); entry->time=(int64_t)rd64(r+16); entry->crc=rd32(r+24);
        if(!n || n>=FM_PATH || entry->type>FM_DIR || (entry->type==FM_DIR && (entry->size || entry->crc))) { e=-EINVAL; break; }
        e=io(w,fd,entry->path,n,0); entry->path[n]=0; if(e)break;
        if(strlen(entry->path)!=n || !safe_path(entry->path)) { e=-EINVAL; break; }
        for(unsigned j=0;j<i;j++)if(fm_same_path(entry->path,set->entry[j].path))e=-EINVAL;
        uint32_t crc=crc32(~0u,r,28); crc=crc32(crc,entry->path,n);
        if(~crc!=rd32(r+28))e=-EIO;
        catalogue=crc32(catalogue,r,sizeof(r)); catalogue=crc32(catalogue,entry->path,n);
        uint64_t left=entry->size; crc=~0u;
        while(left && !e) { unsigned bytes=left<BK_CHUNK ? (unsigned)left : BK_CHUNK;
            e=io(w,fd,w->buffer,bytes,0); if(!e)crc=crc32(crc,w->buffer,bytes); left-=bytes; }
        if(!e && entry->type==FM_FILE && ~crc!=entry->crc)e=-EIO;
    }
    if(!e && ~catalogue!=set->id)e=-EIO;
    uint8_t extra;
    if(!e) { int n=w->fs->read(w->fs->ctx,fd,&extra,1); if(n)e=n<0 ? n : -EIO; }
    int close=w->fs->close(w->fs->ctx,fd); return e ? e : close;
}
static int init(struct bk_work *w,const struct fm_vfs *fs,const struct bk_callbacks *cb,struct bk_result *result)
{
    if(!fs || !result)return -EINVAL;
    memset(w,0,sizeof(*w)); memset(result,0,sizeof(*result)); w->fs=fs; w->cb=cb; w->result=result;
    w->buffer=malloc(BK_CHUNK); w->set.entry=calloc(BK_MAX_FILES,sizeof(*w->set.entry));
    if(!w->buffer || !w->set.entry) { free(w->buffer); free(w->set.entry); return -ENOMEM; }
    return 0;
}
static void destroy(struct bk_work *w) { free(w->buffer); free(w->set.entry); }
int bk_verify(const struct fm_vfs *fs,const char *archive,const struct bk_callbacks *cb,struct bk_result *result)
{
    struct bk_work w; int e=init(&w,fs,cb,result); if(e)return e;
    e=load(&w,archive,&w.set);
    for(unsigned i=0;!e && i<w.set.count;i++) { struct bk_entry *b=&w.set.entry[i];
        if(b->type==FM_DIR)result->folders++; else { result->files++; result->bytes+=b->size; } progress(&w,b->path); }
    destroy(&w); return e;
}
/* Temp path is in the destination directory, exclusive-create only. */
static int temporary(struct bk_work *w,const char *target,char path[FM_PATH])
{
    for(unsigned n=0;n<100;n++) {
        int len=snprintf(path,FM_PATH,"%s.cbk%u",target,n); if(len<0 || len>=FM_PATH)return -ENAMETOOLONG;
        int fd=w->fs->open(w->fs->ctx,path,1); if(fd!=-EEXIST)return fd;
    }
    return -EEXIST;
}
int bk_create(const struct fm_vfs *fs,const char *archive,const char *const *sources,unsigned count,
              const char *base,const struct bk_callbacks *cb,struct bk_result *result)
{
    if(!safe_path(archive) || (base && (!safe_path(base) || fm_same_path(base,archive))) || (!base && (!sources || !count)))return -EINVAL;
    struct bk_work w; int e=init(&w,fs,cb,result); if(e)return e;
    w.archive=archive; w.base=base;
    e=parents(&w,archive,0);
    if(!e && base) {
        e=load(&w,base,&w.set); if(!e && w.set.mode)e=-EINVAL; unsigned kept=0; w.set.base=w.set.id;
        for(unsigned i=0;!e && i<w.set.count;i++) {
            struct bk_entry current=w.set.entry[i]; struct fm_meta m;
            if(fm_same_path(current.path,archive)) { e=-EINVAL; break; }
            int status=fs->stat_path(fs->ctx,current.path,&m);
            if(status==-ENOENT)continue;
            if(status) { e=status; break; }
            if(m.type!=current.type) { e=-EINVAL; break; }
            uint32_t old=current.crc; current.size=m.type==FM_FILE ? m.size : 0; current.time=m.modified;
            if(m.type==FM_FILE) { e=checksum(&w,&current,-1); if(e)break; }
            if(current.type==FM_DIR || current.crc!=old || current.size!=w.set.entry[i].size || current.time!=w.set.entry[i].time)
                w.set.entry[kept++]=current;
        }
        w.set.count=kept;
    } else for(unsigned i=0;!e && i<count;i++)e=add(&w,sources[i],0);
    /* An output already present must be a regular file, never a directory
     * or symlink. The previous successful archive survives every failure. */
    struct fm_meta dest;
    if(!e) { int status=fs->stat_path(fs->ctx,archive,&dest); if(!status && dest.type!=FM_FILE)e=-EINVAL; else if(status && status!=-ENOENT)e=status; }
    char temp[FM_PATH]={0}; int fd=-1;
    if(!e) { fd=temporary(&w,archive,temp); if(fd<0)e=fd; }
    uint8_t h[BK_HEADER]={0}; memcpy(h,"CIUKIBK1",8); wr32(h+8,1); wr32(h+12,w.set.count); wr32(h+20,w.set.base); wr32(h+24,base ? 1 : 0);
    uint32_t catalogue=crc32(~0u,h+20,8);
    for(unsigned i=0;!e && i<w.set.count;i++) { uint8_t r[BK_RECORD]; record(r,&w.set.entry[i]); catalogue=crc32(catalogue,r,sizeof(r)); catalogue=crc32(catalogue,w.set.entry[i].path,strlen(w.set.entry[i].path)); }
    wr32(h+16,~catalogue);
    if(!e)e=io(&w,fd,h,sizeof(h),1);
    for(unsigned i=0;!e && i<w.set.count;i++) {
        struct bk_entry *b=&w.set.entry[i]; uint8_t r[BK_RECORD]; record(r,b);
        e=io(&w,fd,r,sizeof(r),1); if(!e)e=io(&w,fd,b->path,(unsigned)strlen(b->path),1);
        if(!e && b->type==FM_FILE)e=checksum(&w,b,fd);
        if(!e) { if(b->type==FM_DIR)result->folders++; else { result->files++; result->bytes+=b->size; } progress(&w,b->path); }
    }
    if(!e)e=fs->sync(fs->ctx,fd);
    if(fd>=0) { int close=fs->close(fs->ctx,fd); if(!e)e=close; }
    if(!e)e=cancelled(&w);
    if(!e)e=fs->rename(fs->ctx,temp,archive);
    if(e && fd>=0)fs->unlink(fs->ctx,temp);
    destroy(&w); return e;
}
int bk_restore(const struct fm_vfs *fs,const char *archive,const char *root,enum bk_replace policy,
               const struct bk_callbacks *cb,struct bk_result *result)
{
    if((root && !safe_path(root)) || policy>BK_ASK)return -EINVAL;
    struct bk_work w; int e=init(&w,fs,cb,result); if(e)return e;
    e=load(&w,archive,&w.set); /* no destination changes until complete verification */
    int fd=e ? -1 : open_read(&w,archive); if(!e && fd<0)e=fd;
    uint8_t h[BK_HEADER]; if(!e)e=io(&w,fd,h,sizeof(h),0);
    if(!e && (rd32(h+12)!=w.set.count || rd32(h+16)!=w.set.id))e=-EIO;
    for(unsigned i=0;!e && i<w.set.count;i++) {
        struct bk_entry *b=&w.set.entry[i]; uint8_t expected[BK_RECORD],r[BK_RECORD]; record(expected,b);
        e=io(&w,fd,r,sizeof(r),0); if(!e && memcmp(expected,r,sizeof(r)))e=-EIO;
        char name[FM_PATH]; if(!e)e=io(&w,fd,name,(unsigned)strlen(b->path),0);
        if(!e && memcmp(name,b->path,strlen(b->path)))e=-EIO;
        char target[FM_PATH]; int n=snprintf(target,sizeof(target),"%s%s",root ? root : "",b->path);
        if(!e && (n<0 || n>=FM_PATH || !safe_path(target) || fm_same_path(target,archive)))e=-EINVAL;
        if(!e)e=parents(&w,target,1);
        struct fm_meta m; int exists=e ? e : fs->stat_path(fs->ctx,target,&m), skip=0;
        if(!e && exists && exists!=-ENOENT)e=exists;
        if(!e && !exists) {
            if((b->type==FM_FILE && m.type!=FM_FILE) || (b->type==FM_DIR && m.type!=FM_DIR))e=-EINVAL;
            else if(b->type==FM_FILE) {
                skip=policy==BK_NEVER || (policy==BK_OLDER && m.modified>=b->time);
                if(policy==BK_ASK) { int answer=cb && cb->replace ? cb->replace(cb->ctx,target) : -ECANCELED;
                    if(answer<0)e=answer; else skip=!answer; }
            }
        }
        if(!e && b->type==FM_DIR) {
            if(exists==-ENOENT)e=fs->mkdir(fs->ctx,target);
            if(!e)result->folders++;
        }
        char temp[FM_PATH]={0}; int output=-1;
        if(!e && b->type==FM_FILE && !skip) { output=temporary(&w,target,temp); if(output<0)e=output; }
        uint64_t left=b->size; uint32_t crc=~0u;
        while(left && !e) { unsigned bytes=left<BK_CHUNK ? (unsigned)left : BK_CHUNK;
            e=io(&w,fd,w.buffer,bytes,0); if(!e) { crc=crc32(crc,w.buffer,bytes); if(output>=0)e=io(&w,output,w.buffer,bytes,1); } left-=bytes; }
        if(!e && b->type==FM_FILE && ~crc!=b->crc)e=-EIO;
        if(!e && output>=0)e=fs->sync(fs->ctx,output);
        if(output>=0) { int close=fs->close(fs->ctx,output); if(!e)e=close; }
        if(!e && output>=0 && fs->metadata) { struct fm_meta info={.type=FM_FILE,.size=b->size,.modified=b->time}; e=fs->metadata(fs->ctx,temp,&info); }
        if(!e)e=cancelled(&w);
        if(!e && output>=0)e=fs->rename(fs->ctx,temp,target);
        if(e && output>=0)fs->unlink(fs->ctx,temp);
        if(!e && b->type==FM_FILE) { if(skip)result->skipped++; else { result->files++; result->bytes+=b->size; } }
        if(!e)progress(&w,target);
    }
    if(fd>=0) { int close=fs->close(fs->ctx,fd); if(!e)e=close; }
    destroy(&w); return e;
}
