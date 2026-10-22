/* Namespace and export use the same FAT/VFS implementation as the VM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dos_save_store.h"
#include <ciuki/blkpart.h>
#ifdef FS_HOST
#include <stdio.h>
#define save_format snprintf
#else
#include <ciuki/kernel.h>
#define save_format ksnprintf
#endif
static int save_file_read(void *arg,uint32_t at,void *buf,unsigned n)
{
    struct dos_save_file *f=arg; uint64_t pos; size_t done=0;
    int e=vfs_seek(f->table,f->fd,at,VFS_SEEK_SET,&pos);
    if (!e) e=vfs_read(f->table,f->fd,buf,n,&done);
    return e ? e : done==n ? 0 : -FS_EIO;
}
static int save_file_write(void *arg,uint32_t at,const void *buf,unsigned n)
{
    struct dos_save_file *f=arg; uint64_t pos; size_t done=0;
    if (!f->writable) return -FS_EROFS;
    int e=vfs_seek(f->table,f->fd,at,VFS_SEEK_SET,&pos);
    if (!e) e=vfs_write(f->table,f->fd,buf,n,&done);
    return e ? e : done==n ? 0 : -FS_EIO;
}
static int save_file_flush(void *arg)
{ struct dos_save_file *f=arg; return f->writable ? vfs_commit(f->table,f->fd) : 0; }
static int save_file_resize(void *arg,uint32_t size)
{ struct dos_save_file *f=arg; return f->writable ? vfs_truncate(f->table,f->fd,size) : 0; }
void dos_save_file_io(struct dos_save_file *f,struct dos_save_io *io)
{ *io=(struct dos_save_io){save_file_read,save_file_write,save_file_flush,save_file_resize,f}; }
static int save_archive(struct vfs_table *t,const char *path,unsigned game)
{
    uint16_t date,time; uint8_t tenths; fs_timestamp(&date,&time,&tenths);
    char to[128];
    for (unsigned collision=0;collision<4096;collision++) {
        save_format(to,sizeof(to),DOS_SAVE_ROOT "\\G%u-%04u%02u%02u-%02u%02u%02u-%u.BAK",
            game,1980u+(date>>9),(date>>5)&15u,date&31u,time>>11,(time>>5)&63u,(time&31u)*2u,collision);
        struct fat_entry entry; int e=vfs_stat(t,to,&entry);
        if (!e) continue;
        if (e!=-FS_ENOENT) return e;
        return vfs_rename(t,path,to);
    }
    return -FS_ENOSPC;
}
static void save_pattern(char out[80],unsigned game)
{ save_format(out,80,DOS_SAVE_ROOT "\\G%uK*.COW",game); }
int dos_save_store_open(struct dos_save_file *file,struct dos_save *s,
                        const struct dos_save_identity *id,char path[128],unsigned *archived)
{
    struct vfs_table *t=file->table; file->fd=-1; file->absent=false; *archived=0;
    if (id->base[64]) return -FS_EINVAL;
    for (unsigned i=0;i<64;i++) if (!((id->base[i]>='0' && id->base[i]<='9') ||
                                     (id->base[i]>='a' && id->base[i]<='f'))) return -FS_EINVAL;
    save_format(path,128,DOS_SAVE_ROOT "\\G%uK%u%s.COW",id->game,id->kernel,id->base);
    int e=vfs_mkdir(t,DOS_SAVE_ROOT,0);
    if (e && e!=-FS_EEXIST && e!=-FS_EROFS) return e;
    char pattern[80]; save_pattern(pattern,id->game);
    struct vfs_find find; struct fat_entry entry;
    /* Only the matching active identity remains a .COW. Archives are never
     * auto-deleted/reused. Restart a search after every namespace mutation. */
    for (unsigned budget=0;budget<4096;budget++) {
        e=vfs_find_first(t,pattern,0,&find,&entry);
        while (!e) {
            if (strlen(entry.name)>=128-sizeof(DOS_SAVE_ROOT)) return -FS_ENAMETOOLONG;
            char old[128]; save_format(old,sizeof(old),DOS_SAVE_ROOT "\\%s",entry.name);
            if (strncmp(old,path,sizeof(old))) {
                e=save_archive(t,old,id->game); if (e) return e;
                (*archived)++; break;
            }
            e=vfs_find_next(t,&find,&entry);
        }
        if (e==-FS_ENOENT) break;
        if (e) return e;
        if (budget==4095) return -FS_ELOOP;
    }
    uint32_t size=0;
    e=vfs_stat(t,path,&entry);
    if (!e) { if (entry.attr&FAT_ATTR_DIR) return -FS_EIO; size=entry.size; }
    else if (e!=-FS_ENOENT) return e;
    file->absent=e==-FS_ENOENT;
    file->writable=true;
    file->fd=vfs_open(t,path,VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (file->fd==-FS_EROFS && !e) {
        file->writable=false; file->fd=vfs_open(t,path,VFS_READ,VFS_DENY_WRITE,0);
    }
    if (file->fd<0) return file->fd;
    struct dos_save_io io; dos_save_file_io(file,&io);
    e=dos_save_open(s,&io,id,size);
    if (e==DOS_SAVE_MISMATCH) {
        int ce=vfs_close(t,file->fd); file->fd=-1; if (ce) return ce;
        e=save_archive(t,path,id->game); if (e) return e; (*archived)++;
        file->writable=true;
        file->fd=vfs_open(t,path,VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_ALL,0);
        if (file->fd<0) return file->fd;
        e=dos_save_open(s,&io,id,0);
    }
    if (!e && !file->writable) s->readonly=true;
    if (e) { dos_save_release(s); (void)vfs_close(t,file->fd); file->fd=-1; }
    return e;
}
int dos_save_store_query(struct vfs_table *t,unsigned game,uint32_t *bytes)
{
    char pattern[80]; save_pattern(pattern,game); *bytes=0;
    struct vfs_find find; struct fat_entry entry;
    int e=vfs_find_first(t,pattern,0,&find,&entry);
    while (!e) {
        if (entry.size>UINT32_MAX-*bytes) return -FS_EFBIG;
        *bytes+=entry.size; e=vfs_find_next(t,&find,&entry);
    }
    return e==-FS_ENOENT ? 0 : e;
}
int dos_save_store_reset(struct vfs_table *t,unsigned game)
{
    char pattern[80]; save_pattern(pattern,game);
    for (unsigned n=0;n<4096;n++) {
        struct vfs_find find; struct fat_entry entry;
        int e=vfs_find_first(t,pattern,0,&find,&entry);
        if (e==-FS_ENOENT) return 0;
        if (e) return e;
        if (strlen(entry.name)>=128-sizeof(DOS_SAVE_ROOT)) return -FS_ENAMETOOLONG;
        char path[128]; save_format(path,sizeof(path),DOS_SAVE_ROOT "\\%s",entry.name);
        e=vfs_delete(t,path); if (e) return e;
    }
    return -FS_ELOOP;
}
struct save_export {
    struct fat_volume view,base;
    struct blkpart partition[2];
    struct partition_table parts;
    struct vfs_table *target;
    char destination[32],relative[FS_PATH_BYTES],path[FS_PATH_BYTES];
    uint8_t bytes[4096],original[4096];
    uint32_t files,budget;
};
static int save_mount(struct save_export *x,struct fat_volume *v,struct blkdev *d,
                      struct block_cache *cache,unsigned which)
{
    struct blkdev *view=d;
    int e=partition_scan(d,&x->parts); if (e) return e;
    if (x->parts.count) {
        const struct partition *p=0;
        for (unsigned i=0;i<x->parts.count;i++)
            if (x->parts.entries[i].type==6) { p=&x->parts.entries[i]; break; }
        if (!p) return -FS_EIO;
        e=blkpart_init(&x->partition[which],d,p); if (e) return e;
        view=&x->partition[which].block; view->logical=true;
    }
    return fat_mount(v,cache,view,0,view->capacity,false);
}
/* Bounded depth and total entries; no recursion or kernel-stack file buffers. */
static int save_export_walk(struct save_export *x)
{
    struct { uint32_t directory,base,cursor; size_t length; } stack[16];
    unsigned depth=1; memset(stack,0,sizeof(stack));
    stack[0].directory=x->view.root; stack[0].base=x->base.root;
    while (depth) {
        unsigned top=depth-1; struct fat_entry entry,old;
        int e=fat_next(&x->view,stack[top].directory,&stack[top].cursor,&entry);
        if (e==-FS_ENOENT) { depth--; continue; }
        if (e) return e;
        if (++x->budget>65536) return -FS_ELOOP;
        if (!strncmp(entry.name,".",sizeof(entry.name)) || !strncmp(entry.name,"..",sizeof(entry.name)) || (entry.attr&FAT_ATTR_VOLUME)) continue;
        size_t len=stack[top].length,n=strlen(entry.name);
        if (len+n+2>=sizeof(x->relative)) return -FS_ENAMETOOLONG;
        x->relative[len]='\\'; memcpy(x->relative+len+1,entry.name,n+1);
        int found=stack[top].base==UINT32_MAX ? -FS_ENOENT : fat_lookup(&x->base,stack[top].base,entry.name,&old);
        if (found && found!=-FS_ENOENT) return found;
        if (entry.attr&FAT_ATTR_DIR) {
            if (depth==16) return -FS_ELOOP;
            stack[depth].directory=entry.first; stack[depth].base=!found && (old.attr&FAT_ATTR_DIR) ? old.first : UINT32_MAX;
            stack[depth].cursor=0; stack[depth].length=len+n+1; depth++; continue;
        }
        bool changed=found || (old.attr&FAT_ATTR_DIR) || old.size!=entry.size ||
            old.times.create_date!=entry.times.create_date || old.times.create_time!=entry.times.create_time ||
            old.times.access_date!=entry.times.access_date || old.times.write_date!=entry.times.write_date ||
            old.times.write_time!=entry.times.write_time || old.times.create_tenths!=entry.times.create_tenths || old.attr!=entry.attr;
        for (uint32_t at=0;!changed && at<entry.size;) {
            size_t nbytes=entry.size-at,got=0,base_got=0; if (nbytes>4096) nbytes=4096;
            e=fat_read(&x->view,&entry,at,x->bytes,nbytes,&got);
            if (!e) e=fat_read(&x->base,&old,at,x->original,nbytes,&base_got);
            if (e || got!=nbytes || base_got!=nbytes) return e ? e : -FS_EIO;
            changed=memcmp(x->bytes,x->original,nbytes)!=0; at+=(uint32_t)nbytes; fs_service();
        }
        if (!changed) continue;
        int count=save_format(x->path,sizeof(x->path),"%s%s",x->destination,x->relative);
        if (count<0 || (unsigned)count>=sizeof(x->path)) return -FS_ENAMETOOLONG;
        for (char *p=x->path+strlen(x->destination)+1;*p;p++) if (*p=='\\') {
            *p=0; e=vfs_mkdir(x->target,x->path,0); *p='\\';
            if (e && e!=-FS_EEXIST) return e;
            e=0;
        }
        int fd=vfs_open(x->target,x->path,VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_ALL,0);
        if (fd<0) return fd;
        for (uint32_t at=0;!e && at<entry.size;) {
            size_t nbytes=entry.size-at,got=0,done=0; if (nbytes>4096) nbytes=4096;
            e=fat_read(&x->view,&entry,at,x->bytes,nbytes,&got);
            if (!e && got==nbytes) e=vfs_write(x->target,fd,x->bytes,nbytes,&done);
            if (!e && (got!=nbytes || done!=nbytes)) e=-FS_EIO;
            at+=(uint32_t)nbytes; fs_service();
        }
        if (!e) e=vfs_commit(x->target,fd);
        int ce=vfs_close(x->target,fd); if (e || ce) return e ? e : ce;
        x->files++; fs_service();
    }
    return 0;
}
int dos_save_store_export(struct vfs_table *t,struct blkdev *merged,struct blkdev *base,
                          struct block_cache *cache,unsigned game,char path[32],uint32_t *files)
{
    *files=0; path[0]=0;
    struct save_export *x=fs_contig_alloc(sizeof(*x)); if (!x) return -FS_ENOMEM;
    x->target=t;
    int e=vfs_mkdir(t,"C:\\Documents\\Saves",0);
    if (e==-FS_EEXIST) e=0;
    bool view_open=false,base_open=false;
    if (!e) { e=save_mount(x,&x->view,merged,cache,0); view_open=!e; }
    if (!e) { e=save_mount(x,&x->base,base,cache,1); base_open=!e; }
    if (!e) {
        e=-FS_ENOSPC;
        for (unsigned n=0;n<4096;n++) {
            save_format(x->destination,sizeof(x->destination),"C:\\Documents\\Saves\\G%u-%u",game,n);
            e=vfs_mkdir(t,x->destination,0);
            if (e!=-FS_EEXIST) break;
        }
    }
    if (!e) e=save_export_walk(x);
    /* On failure leave a clearly incomplete export directory, never report
     * it as a completed backup. The overlay and previous exports are intact. */
    if (!e) { memcpy(path,x->destination,strlen(x->destination)+1); *files=x->files; }
    if (base_open) { int ce=fat_unmount(&x->base); if (!e) e=ce; }
    if (view_open) { int ce=fat_unmount(&x->view); if (!e) e=ce; }
    fs_contig_free(x,sizeof(*x)); return e;
}
