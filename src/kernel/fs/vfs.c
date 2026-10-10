/* SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
struct resolved { unsigned drive; uint32_t parent; char name[FS_NAME_BYTES]; };
void vfs_init(struct vfs *v) { memset(v,0,sizeof(*v)); fs_lock_init(&v->lock); }
void vfs_destroy(struct vfs *v) { fs_lock_destroy(&v->lock); }
int vfs_attach(struct vfs *v, unsigned d, struct fat_volume *vol) {
    if (d>=26 || !vol || !vol->mounted) return -FS_EINVAL;
    fs_lock_take(&v->lock); int e=0;
    if (v->volumes[d]) e=-FS_EBUSY;
    for (unsigned i=0;i<26;i++) if (v->volumes[i]) {
        struct fat_volume *other=v->volumes[i];
        if (other->cache!=vol->cache) e=-FS_EINVAL;
        if (other->dev==vol->dev && other->start<vol->start+vol->sectors && vol->start<other->start+other->sectors) e=-FS_EBUSY;
    }
    if (!e) { v->volumes[d]=vol; v->generation[d]++; }
    fs_lock_drop(&v->lock); return e;
}
int vfs_detach(struct vfs *v, unsigned d) {
    if (d>=26) return -FS_EINVAL;
    fs_lock_take(&v->lock); int e=0; struct fat_volume *vol=v->volumes[d];
    if (!vol) e=-FS_ENOENT;
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol) e=-FS_EBUSY;
    if (!e) e=fat_unmount(vol);
    if (!e) {
        v->volumes[d]=0; v->generation[d]++;
        for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i]) {
            v->tables[i]->cwd[d][0]='/'; v->tables[i]->cwd[d][1]=0; v->tables[i]->cwd_cluster[d]=0;
        }
    }
    fs_lock_drop(&v->lock); return e;
}
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned d) {
    if (d>=26) return -FS_EINVAL;
    fs_lock_take(&v->lock); int slot=-1;
    for (unsigned i=0;i<VFS_TABLES;i++) { if (v->tables[i]==t) { fs_lock_drop(&v->lock); return -FS_EBUSY; } if (!v->tables[i]) slot=(int)i; }
    if (slot<0) { fs_lock_drop(&v->lock); return -FS_EMFILE; }
    memset(t,0,sizeof(*t)); t->vfs=v; t->drive=d;
    for (unsigned i=0;i<26;i++) { t->cwd[i][0]='/'; t->cwd[i][1]=0; }
    v->tables[slot]=t; fs_lock_drop(&v->lock); return 0;
}
static void close_handle(struct vfs_table *t, unsigned h) {
    struct vfs_description *d=t->handles[h]; if (!d) return;
    t->handles[h]=0;
    if (!--d->refs) { if (!--d->node->refs) memset(d->node,0,sizeof(*d->node)); memset(d,0,sizeof(*d)); }
}
void vfs_table_destroy(struct vfs_table *t) {
    struct vfs *v=t->vfs; fs_lock_take(&v->lock);
    for (unsigned i=0;i<VFS_HANDLES;i++) close_handle(t,i);
    for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i]==t) v->tables[i]=0;
    t->vfs=0; fs_lock_drop(&v->lock);
}
static int normalize(struct vfs_table *t, const char *s, unsigned *drive, char out[FS_PATH_BYTES]) {
    if (!s || !*s) return -FS_EINVAL;
    unsigned d=t->drive;
    if (s[1]==':') { d=(unsigned)(path_upper((uint8_t)s[0])-'A'); if (d>=26) return -FS_EINVAL; }
    int e=path_normalize(s,d,t->cwd[d],drive,out); if (e) return e;
    return t->vfs->volumes[*drive] ? 0 : -FS_ENOENT;
}
static uint32_t root(struct fat_volume *v) { return v->type==32 ? v->root : 0; }
static int resolve(struct vfs_table *t, const char *path, struct resolved *r) {
    char full[FS_PATH_BYTES]; int e=normalize(t,path,&r->drive,full); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[r->drive]; r->parent=root(v); const char *s=full+1;
    if (!*s) { r->name[0]=0; return 0; }
    while (*s) {
        unsigned n=0; while (*s && *s!='/') { if (n>=sizeof(r->name)-1) return -FS_ENAMETOOLONG; r->name[n++]=*s++; } r->name[n]=0;
        if (!*s) return 0;
        struct fat_entry ent; if ((e=fat_lookup(v,r->parent,r->name,&ent))) return e;
        if (!(ent.attr&FAT_ATTR_DIR)) return -FS_ENOTDIR;
        r->parent=ent.first; s++;
    }
    return -FS_EINVAL;
}
static int lookup(struct vfs_table *t, const char *path, struct resolved *r, struct fat_entry *ent) {
    int e=resolve(t,path,r); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[r->drive];
    if (!r->name[0]) { memset(ent,0,sizeof(*ent)); ent->first=root(v); ent->attr=FAT_ATTR_DIR; ent->index=UINT32_MAX; ent->name[0]='/'; return 0; }
    return fat_lookup(v,r->parent,r->name,ent);
}
static struct vfs_node *node_find(struct vfs *v, struct fat_volume *vol, const struct fat_entry *ent) {
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol && v->nodes[i].entry.parent==ent->parent && v->nodes[i].entry.index==ent->index) return &v->nodes[i];
    return 0;
}
static int handle_free(struct vfs_table *t) { for (unsigned i=0;i<VFS_HANDLES;i++) if (!t->handles[i]) return (int)i; return -FS_EMFILE; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share deny, uint8_t attr) {
    if (!(flags&3) || flags&~31u || deny>VFS_DENY_ALL || (flags&VFS_TRUNCATE && !(flags&VFS_WRITE)) || (flags&VFS_EXCLUSIVE && !(flags&VFS_CREATE))) return -FS_EINVAL;
    struct vfs *v=t->vfs; fs_lock_take(&v->lock);
    struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    bool create=e==-FS_ENOENT && (flags&VFS_CREATE);
    /* A missing ancestor is distinguished by resolving again before create. */
    if (create) e=resolve(t,path,&r);
    if (e) goto out;
    if (!create && (flags&(VFS_CREATE|VFS_EXCLUSIVE))==(VFS_CREATE|VFS_EXCLUSIVE)) { e=-FS_EEXIST; goto out; }
    struct fat_volume *vol=v->volumes[r.drive];
    if (!create && (ent.attr&FAT_ATTR_DIR)) { e=-FS_EISDIR; goto out; }
    if ((flags&VFS_WRITE) && (vol->readonly || (!create && (ent.attr&FAT_ATTR_RO)))) { e=vol->readonly ? -FS_EROFS : -FS_EACCES; goto out; }
    int h=handle_free(t); if (h<0) { e=h; goto out; }
    struct vfs_description *d=0; for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (!v->descriptions[i].refs) { d=&v->descriptions[i]; break; }
    if (!d) { e=-FS_EMFILE; goto out; }
    struct vfs_node *node=create ? 0 : node_find(v,vol,&ent);
    if (node) {
        for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) {
            struct vfs_description *other=&v->descriptions[i];
            if (other->refs && other->node==node && ((other->deny&(flags&3)) || ((unsigned)deny&other->access))) { e=-FS_EACCES; goto out; }
        }
    } else {
        for (unsigned i=0;i<VFS_NODES;i++) if (!v->nodes[i].refs) { node=&v->nodes[i]; break; }
        if (!node) { e=-FS_EMFILE; goto out; }
    }
    if (create) { if (attr&FAT_ATTR_DIR) { e=-FS_EISDIR; goto out; } if ((e=fat_create(vol,r.parent,r.name,attr,&ent))) goto out; }
    if (!node->refs) { node->volume=vol; node->entry=ent; }
    if ((flags&VFS_TRUNCATE) && (e=fat_truncate(vol,&node->entry,0))) goto out;
    node->refs++; d->refs=1; d->node=node; d->position=0; d->access=flags&3; d->deny=(unsigned)deny; t->handles[h]=d; e=h;
out: fs_lock_drop(&v->lock); return e;
}
static struct vfs_description *get(struct vfs_table *t, int h) { return h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0; }
int vfs_close(struct vfs_table *t, int h) {
    fs_lock_take(&t->vfs->lock); int e=get(t,h) ? 0 : -FS_EBADF; if (!e) close_handle(t,(unsigned)h); fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_dup(struct vfs_table *src, int h, struct vfs_table *dst, int target) {
    if (src->vfs!=dst->vfs || target< -1 || target>=(int)VFS_HANDLES) return -FS_EINVAL;
    fs_lock_take(&src->vfs->lock); struct vfs_description *d=get(src,h); int e=-FS_EBADF;
    if (d) {
        if (target==-1) target=handle_free(dst);
        if (target<0) e=target;
        else { d->refs++; close_handle(dst,(unsigned)target); dst->handles[target]=d; e=target; }
    }
    fs_lock_drop(&src->vfs->lock); return e;
}
int vfs_read(struct vfs_table *t, int h, void *buf, size_t n, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    fs_lock_take(&t->vfs->lock); struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF; else if (!(d->access&VFS_READ)) e=-FS_EACCES;
    else { e=fat_read(d->node->volume,&d->node->entry,d->position,buf,n,done); d->position+=*done; }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_write(struct vfs_table *t, int h, const void *buf, size_t n, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    fs_lock_take(&t->vfs->lock); struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF; else if (!(d->access&VFS_WRITE)) e=-FS_EACCES;
    else { e=fat_write(d->node->volume,&d->node->entry,d->position,buf,n,done); d->position+=*done; }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_seek(struct vfs_table *t, int h, int64_t off, enum vfs_whence whence, uint64_t *position) {
    if (!position || whence>VFS_SEEK_END) return -FS_EINVAL;
    fs_lock_take(&t->vfs->lock); struct vfs_description *d=get(t,h); int e=0;
    if (!d) e=-FS_EBADF;
    else {
        uint64_t base=whence==VFS_SEEK_SET ? 0 : whence==VFS_SEEK_CUR ? d->position : d->node->entry.size;
        uint64_t magnitude=off<0 ? (uint64_t)(-(off+1))+1 : (uint64_t)off;
        if (off<0 && magnitude>base) e=-FS_EINVAL;
        else if (off>=0 && magnitude>UINT64_MAX-base) e=-FS_EINVAL;
        else { d->position=off<0 ? base-magnitude : base+magnitude; *position=d->position; }
    }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_truncate(struct vfs_table *t, int h, uint64_t size) {
    fs_lock_take(&t->vfs->lock); struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : !(d->access&VFS_WRITE) ? -FS_EACCES : fat_truncate(d->node->volume,&d->node->entry,size);
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_commit(struct vfs_table *t, int h) {
    fs_lock_take(&t->vfs->lock); struct vfs_description *d=get(t,h);
    int e=d ? fat_commit(d->node->volume) : -FS_EBADF; fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *out) {
    fs_lock_take(&t->vfs->lock); struct resolved r; int e=lookup(t,path,&r,out);
    if (!e) { struct vfs_node *n=node_find(t->vfs,t->vfs->volumes[r.drive],out); if (n) *out=n->entry; }
    fs_lock_drop(&t->vfs->lock); return e;
}
static int metadata(struct vfs_table *t, const char *path, int attr, const struct fat_times *times) {
    fs_lock_take(&t->vfs->lock); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && ent.index==UINT32_MAX) e=-FS_EACCES;
    if (!e) {
        struct fat_volume *vol=t->vfs->volumes[r.drive]; struct vfs_node *n=node_find(t->vfs,vol,&ent);
        if (n) ent=n->entry;
        e=fat_set_metadata(vol,&ent,attr<0 ? ent.attr : (uint8_t)attr,times);
        if (!e && n) n->entry=ent;
    }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_attrib(struct vfs_table *t, const char *p, uint8_t attr) { return metadata(t,p,attr,0); }
int vfs_times(struct vfs_table *t, const char *p, const struct fat_times *times) { return times ? metadata(t,p,-1,times) : -FS_EINVAL; }
static int find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    if (f->drive>=26 || !t->vfs->volumes[f->drive] || f->generation!=t->vfs->generation[f->drive]) return -FS_EIO;
    int e;
    while (!(e=fat_next(t->vfs->volumes[f->drive],f->directory,&f->cursor,ent))) {
        if (ent->attr & (FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM|FAT_ATTR_DIR) & ~f->attr_mask) continue;
        if (path_wildcard(f->pattern,ent->name) || path_wildcard(f->pattern,ent->alias)) return 0;
    }
    return e;
}
int vfs_find_first(struct vfs_table *t, const char *p, uint8_t mask, struct vfs_find *f, struct fat_entry *ent) {
    fs_lock_take(&t->vfs->lock); struct resolved r; int e=resolve(t,p,&r);
    if (!e && !r.name[0]) e=-FS_EINVAL;
    if (!e) { memset(f,0,sizeof(*f)); f->drive=r.drive; f->directory=r.parent; f->generation=t->vfs->generation[r.drive]; f->attr_mask=mask; memcpy(f->pattern,r.name,strlen(r.name)+1); e=find_next(t,f,ent); }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    fs_lock_take(&t->vfs->lock); int e=find_next(t,f,ent); fs_lock_drop(&t->vfs->lock); return e;
}
/* Does ancestor contain directory? Resolve on-disk parents, so aliases cannot
 * evade cycle/cwd checks. Root . . references are normalized to root(). */
static __attribute__((noinline)) int contains(struct fat_volume *v, uint32_t ancestor, uint32_t dir) {
    for (uint32_t i=0;i<v->clusters;i++) {
        if (dir==ancestor) return 1; if (!dir || dir==root(v)) return 0;
        struct fat_entry parent; int e=fat_lookup(v,dir,"..",&parent); if (e) return e;
        dir=parent.first;
    }
    return -FS_ELOOP;
}
static int busy(struct vfs *v, struct fat_volume *vol, const struct fat_entry *ent) {
    if (node_find(v,vol,ent)) return -FS_EBUSY;
    if (!(ent->attr&FAT_ATTR_DIR)) return 0;
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol) {
        int e=contains(vol,ent->first,v->nodes[i].entry.parent); if (e) return e<0 ? e : -FS_EBUSY;
    }
    for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i]) {
        struct vfs_table *t=v->tables[i];
        for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) {
            int e=contains(vol,ent->first,t->cwd_cluster[d] ? t->cwd_cluster[d] : root(vol));
            if (e) return e<0 ? e : -FS_EBUSY;
        }
    }
    return 0;
}
static int remove_path(struct vfs_table *t, const char *path, bool dir) {
    fs_lock_take(&t->vfs->lock); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && !r.name[0]) e=-FS_EACCES;
    if (!e) { struct fat_volume *v=t->vfs->volumes[r.drive]; if (!(e=busy(t->vfs,v,&ent))) e=fat_remove(v,&ent,dir); }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_delete(struct vfs_table *t, const char *p) { return remove_path(t,p,false); }
int vfs_rmdir(struct vfs_table *t, const char *p) { return remove_path(t,p,true); }
int vfs_mkdir(struct vfs_table *t, const char *p, uint8_t attr) {
    fs_lock_take(&t->vfs->lock); struct resolved r; int e=resolve(t,p,&r); struct fat_entry ent;
    if (!e && !r.name[0]) e=-FS_EEXIST;
    if (!e) e=fat_create(t->vfs->volumes[r.drive],r.parent,r.name,attr|FAT_ATTR_DIR,&ent);
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_rename(struct vfs_table *t, const char *from, const char *to) {
    fs_lock_take(&t->vfs->lock); struct resolved a,b; struct fat_entry ent; int e=lookup(t,from,&a,&ent);
    if (!e) e=resolve(t,to,&b);
    if (!e && (!a.name[0] || !b.name[0])) e=-FS_EACCES;
    if (!e && a.drive!=b.drive) e=-FS_EXDEV;
    if (!e) {
        struct fat_volume *v=t->vfs->volumes[a.drive]; e=busy(t->vfs,v,&ent);
        if (!e && (ent.attr&FAT_ATTR_DIR)) { e=contains(v,ent.first,b.parent); if (e>0) e=-FS_EINVAL; }
        if (!e) e=fat_rename(v,&ent,b.parent,b.name);
    }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_chdir(struct vfs_table *t, const char *p) {
    fs_lock_take(&t->vfs->lock); struct resolved r; struct fat_entry ent; int e=lookup(t,p,&r,&ent);
    if (!e && !(ent.attr&FAT_ATTR_DIR)) e=-FS_ENOTDIR;
    if (!e) {
        char full[FS_PATH_BYTES], canonical[FS_PATH_BYTES]; unsigned d; e=normalize(t,p,&d,full);
        if (!e) {
            unsigned used=1; canonical[0]='/'; canonical[1]=0; uint32_t dir=root(t->vfs->volumes[d]); const char *s=full+1;
            while (*s && !e) {
                unsigned n=0; while (*s && *s!='/') r.name[n++]=*s++; r.name[n]=0;
                e=fat_lookup(t->vfs->volumes[d],dir,r.name,&ent); if (e) break;
                n=(unsigned)strlen(ent.name); if (used>1) canonical[used++]='/';
                if (used+n>=sizeof(canonical)) { e=-FS_ENAMETOOLONG; break; }
                memcpy(canonical+used,ent.name,n+1); used+=n; dir=ent.first; if (*s) s++;
            }
            if (!e) { memcpy(t->cwd[d],canonical,used+1); t->cwd_cluster[d]=dir; }
        }
    }
    fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_set_drive(struct vfs_table *t, unsigned d) {
    if (d>=26) return -FS_EINVAL; fs_lock_take(&t->vfs->lock);
    int e=t->vfs->volumes[d] ? 0 : -FS_ENOENT; if (!e) t->drive=d; fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_getcwd(struct vfs_table *t, unsigned d, char out[FS_PATH_BYTES]) {
    if (d>=26 || !out) return -FS_EINVAL; fs_lock_take(&t->vfs->lock);
    int e=t->vfs->volumes[d] ? 0 : -FS_ENOENT; if (!e) memcpy(out,t->cwd[d],strlen(t->cwd[d])+1); fs_lock_drop(&t->vfs->lock); return e;
}
int vfs_free_space(struct vfs_table *t, unsigned d, uint64_t *bytes, uint32_t *cb) {
    if (d>=26 || !bytes || !cb) return -FS_EINVAL; fs_lock_take(&t->vfs->lock);
    struct fat_volume *v=t->vfs->volumes[d]; int e=v ? 0 : -FS_ENOENT;
    if (v) { *cb=(uint32_t)v->spc*512; *bytes=(uint64_t)v->free_clusters * *cb; }
    fs_lock_drop(&t->vfs->lock); return e;
}
