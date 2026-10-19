/* DOS2-6 handle services over the production VFS. No FAT parser or data cache.
 * Research/decisions: docs/validation/f3-49-vmm-dos-file-api.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dos_file.h"
#define BAD_POINTER (-207)
#define BAD_DRIVE (-208)
#define BAD_MODE (-209)
#define NO_MORE (-210)
#define BAD_PARENT (-211)
#define CURRENT_DIRECTORY (-212)
static bool request_cancelled(void *);
static unsigned device_kind(const char *);
static int device_index(struct dos_file *d,const char *path) {
    const char *leaf=path;
    for (const char *s=path;*s;s++) if (*s=='/' || *s=='\\' || *s==':') leaf=s+1;
    char name[9]; unsigned n=0;
    while (*leaf && *leaf!='.' && *leaf!=':' && n<8) name[n++]=(char)path_upper((uint8_t)*leaf++);
    while (n && name[n-1]==' ') n--; name[n]=0;
    for (unsigned i=0;i<DOS_FILE_DEVICES;i++) if (d->device[i].used && !strncmp(d->device[i].name,name,9)) return (int)i;
    return -FS_ENOENT;
}
static struct dos_file_process *process(struct dos_file *d,uint16_t psp) {
    for (unsigned i=0;i<DOS_FILE_PSPS;i++) if (d->process[i].used && d->process[i].psp==psp) return &d->process[i];
    return 0;
}
static uint64_t owner(struct dos_file *d,uint16_t psp) {
    return ((uint64_t)d->generation<<32) | ((uint64_t)d->id<<16) | psp;
}
static uint32_t address(struct dos_file *d,struct dos_file_far p) {
    uint32_t linear=((uint32_t)p.seg<<4)+p.off;
    return d->a20 ? linear : linear&0xfffffu;
}
static bool check(struct dos_file *d,struct dos_file_far p,size_t n,bool write) {
    if (n>65536u-p.off) return false;
    uint32_t linear=address(d,p); size_t first=n;
    if (!d->a20 && first>0x100000u-linear) first=0x100000u-linear;
    return d->ops.check(d->ops.arg,linear,first,write) &&
        (first==n || d->ops.check(d->ops.arg,0,n-first,write));
}
static struct dos_file_far plus(struct dos_file_far p,unsigned off) { p.off=(uint16_t)(p.off+off); return p; }
static void read_guest(struct dos_file *d,struct dos_file_far p,void *b,size_t n) {
    uint32_t linear=address(d,p); size_t first=n;
    if (!d->a20 && first>0x100000u-linear) first=0x100000u-linear;
    d->ops.read(d->ops.arg,linear,b,first);
    if (first<n) d->ops.read(d->ops.arg,0,(uint8_t *)b+first,n-first);
}
static void write_guest(struct dos_file *d,struct dos_file_far p,const void *b,size_t n) {
    if (request_cancelled(d)) return;
    uint32_t linear=address(d,p); size_t first=n;
    if (!d->a20 && first>0x100000u-linear) first=0x100000u-linear;
    d->ops.write(d->ops.arg,linear,b,first);
    if (first<n) d->ops.write(d->ops.arg,0,(const uint8_t *)b+first,n-first);
}
static struct dos_file_far far_read(const uint8_t *p) { return (struct dos_file_far){fs_rd16(p),fs_rd16(p+2)}; }
static void far_write(uint8_t *p,struct dos_file_far f) { fs_wr16(p,f.off); fs_wr16(p+2,f.seg); }
static int guest_string(struct dos_file *d,uint16_t seg,uint16_t off,char out[FS_PATH_BYTES]) {
    unsigned used=0;
    for (unsigned i=0;i<FS_PATH_BYTES;i++) {
        if (i>65535u-off) return BAD_POINTER;
        struct dos_file_far p={(uint16_t)(off+i),seg}; uint8_t raw;
        if (!check(d,p,1,false)) return BAD_POINTER;
        read_guest(d,p,&raw,1);
        if (!raw) {
            out[used]=0; if (!used) return -FS_ENOENT;
            /* DOS ignores trailing dots/spaces in ordinary components, while
             * dot and dot-dot retain their navigation meaning. */
            unsigned dst=0,start=0;
            for (unsigned end=0;end<=used;end++) if (!out[end] || out[end]=='/' || out[end]=='\\') {
                unsigned n=end-start;
                if (!(n==1 && out[start]=='.') && !(n==2 && out[start]=='.' && out[start+1]=='.'))
                    while (n && (out[start+n-1]=='.' || out[start+n-1]==' ')) n--;
                memmove(out+dst,out+start,n); dst+=n;
                char separator=out[end]; if (separator) out[dst++]=separator;
                start=end+1;
            }
            out[dst]=0; return dst ? 0 : -FS_ENOENT;
        }
        uint16_t ch=path_from_cp437(raw); char encoded[4];
        if (path_from_ucs2(&ch,1,encoded,sizeof(encoded))) return -FS_EILSEQ;
        size_t n=strlen(encoded); if (used+n>=FS_PATH_BYTES) return -FS_ENAMETOOLONG;
        memcpy(out+used,encoded,n); used+=(unsigned)n;
    }
    return -FS_ENAMETOOLONG;
}
static int dos_name(const char *utf8,char *out,unsigned capacity,bool uppercase) {
    unsigned n=0;
    while (*utf8) {
        uint16_t ch; if (path_decode(&utf8,&ch)) return -FS_EILSEQ;
        if (uppercase && path_cp437(path_upper(ch))>=0) ch=path_upper(ch);
        int byte=path_cp437(ch); if (byte<0 || n+1>=capacity) return -FS_ENAMETOOLONG;
        out[n++]=ch=='/' ? '\\' : (char)byte;
    }
    out[n]=0; return 0;
}

static int preflight(struct dos_file *d,struct dos_file_process *p) {
    if (!check(d,d->binding.sft,6u+59u*d->binding.sft_count,true) ||
        !check(d,d->binding.sda,d->binding.sda_bytes,true) || !check(d,d->binding.indos,1,true) ||
        !check(d,d->binding.selected_psp,2,true) || !check(d,d->binding.dta,4,true)) return BAD_POINTER;
    if (!p) return -FS_EBADF;
    struct dos_file_far header={0x32,p->psp}; uint8_t b[6];
    if (!check(d,header,6,true)) return BAD_POINTER;
    read_guest(d,header,b,6);
    unsigned count=fs_rd16(b); struct dos_file_far jft=far_read(b+2);
    if (count<20 || count>DOS_FILE_JFT || !check(d,jft,count,true)) return BAD_POINTER;
    uint8_t handles[DOS_FILE_JFT]; read_guest(d,jft,handles,count);
    /* The guest can repoint an identical JFT, but cannot acquire authority by
     * writing an SFT index into a free slot or swapping another PSP's slots. */
    for (unsigned i=0;i<count;i++) if (handles[i]!=p->handles[i]) return -FS_EBADF;
    for (unsigned i=count;i<p->count;i++) if (p->handles[i]!=255) return -FS_EMFILE;
    p->jft=jft; p->count=(uint16_t)count; return 0;
}
static void short_fcb(const char *name,uint8_t out[11]) {
    memset(out,' ',11); unsigned at=0;
    while (*name) {
        uint16_t ch; if (path_decode(&name,&ch)) break;
        if (ch=='.') { at=8; continue; }
        int byte=path_cp437(path_upper(ch));
        if (at<11) out[at++]=(uint8_t)(byte<0 ? '_' : byte);
    }
}

static uint16_t file_info(const struct dos_file_description *s) {
    unsigned drive=s->table->handles[s->handle]->node->drive;
    return (uint16_t)(s->drive | (s->written ? 0 : 0x40) |
        ((s->table->vfs->drive_flags[drive]&VFS_DRIVE_REMOVABLE) ? 0 : 0x800));
}
static int mirror(struct dos_file *d,struct dos_file_process *p,const struct vfs_request *request) {
    int error=0;
    uint8_t b[59]={0},head[6];
    memset(head,255,4); fs_wr16(head+4,d->binding.sft_count); write_guest(d,d->binding.sft,head,6);
    for (unsigned i=0;i<d->binding.sft_count;i++) {
        memset(b,0,sizeof(b)); struct dos_file_description *s=&d->sft[i];
        fs_wr16(b,(uint16_t)s->refs); fs_wr16(b+2,(uint16_t)s->mode); fs_wr16(b+0x31,s->owner);
        if (s->refs && s->kind==DOS_FILE_REGULAR) {
            /* A native writer can update this entry while namespace is
             * dropped for I/O. Join its volume reservation before copying. */
            struct vfs *space=s->table->vfs;
            int e=vfs_admit(space,vfs_drive_mask(d->view[s->drive].drive),request);
            if (e) { if (!error) error=e; goto entry_done; }
            struct vfs_description *v=s->table->handles[s->handle];
            if (v) { struct fat_entry *e=&v->node->entry;
                b[4]=e->attr; fs_wr16(b+5,file_info(s)); fs_wr16(b+0xb,(uint16_t)e->first);
                far_write(b+7,d->dpb[s->drive]);
                fs_wr16(b+0xd,e->times.write_time); fs_wr16(b+0xf,e->times.write_date);
                fs_wr32(b+0x11,e->size); fs_wr32(b+0x15,(uint32_t)v->position);
                short_fcb(e->alias[0] ? e->alias : e->name,b+0x20);
            }
            vfs_unlock(space);
        } else if (s->refs) {
            const struct dos_file_device *dev=&d->device[s->device];
            fs_wr16(b+5,dev->info); far_write(b+7,dev->header); short_fcb(dev->name,b+0x20);
        }
entry_done:
        write_guest(d,plus(d->binding.sft,6+i*59),b,59);
    }
    if (!p) return error;
    write_guest(d,p->jft,p->handles,p->count);
    uint8_t state[0x20]; read_guest(d,d->binding.sda,state,sizeof(state));
    state[0]=d->critical; state[1]=0; state[2]=d->critical ? d->active_drive : 255; state[3]=p->error.locus;
    far_write(state+8,d->binding.critical_device);
    fs_wr16(state+4,p->error.code); state[6]=p->error.action; state[7]=p->error.class_;
    far_write(state+0xc,p->dta); fs_wr16(state+0x10,p->psp); state[0x16]=d->drive;
    fs_wr16(state+0x1c,d->owning); write_guest(d,d->binding.sda,state,sizeof(state));
    write_guest(d,d->binding.indos,state+1,1);
    write_guest(d,d->binding.selected_psp,state+0x10,2);
    write_guest(d,d->binding.dta,state+0x0c,4);
    return error;
}
void dos_file_error_from_fs(struct dos_file_error *e,int error,bool write) {
    *e=(struct dos_file_error){5,3,3,2};
    switch (-error) {
    case FS_EOPNOTSUPP: *e=(struct dos_file_error){1,7,4,1}; break;
    case -BAD_MODE: *e=(struct dos_file_error){12,7,4,1}; break;
    case FS_EINVAL: case FS_ENAMETOOLONG: case FS_EILSEQ: case FS_ELOOP: case -BAD_POINTER:
        *e=(struct dos_file_error){87,7,4,1}; break;
    case FS_ENOENT: *e=(struct dos_file_error){2,8,3,2}; break;
    case FS_ENOTDIR: case -BAD_PARENT: *e=(struct dos_file_error){3,8,3,2}; break;
    case FS_EMFILE: *e=(struct dos_file_error){4,1,4,1}; break;
    case FS_EBADF: *e=(struct dos_file_error){6,7,4,1}; break;
    case FS_ENOMEM: *e=(struct dos_file_error){8,1,4,5}; break;
    case -BAD_DRIVE: *e=(struct dos_file_error){15,8,3,2}; break;
    case -CURRENT_DIRECTORY: *e=(struct dos_file_error){16,3,3,2}; break;
    case FS_EXDEV: *e=(struct dos_file_error){17,9,3,2}; break;
    case -NO_MORE: *e=(struct dos_file_error){18,8,3,2}; break;
    case FS_EROFS: *e=(struct dos_file_error){19,11,7,2}; break;
    case FS_EIO: case FS_EUCLEAN: case FS_EQUARANTINED: case VFS_ETIMEOUT: case VFS_ECANCELLED:
        *e=(struct dos_file_error){write ? 29 : 30,5,1,2}; break;
    case FS_EAGAIN: *e=(struct dos_file_error){21,11,7,2}; break;
    case FS_EBUSY: case VFS_ESHARE: case VFS_ECRITICAL_SHARE: *e=(struct dos_file_error){32,10,2,2}; break;
    case VFS_ELOCK: *e=(struct dos_file_error){33,10,2,2}; break;
    case VFS_ELOCKFULL: *e=(struct dos_file_error){36,1,4,2}; break;
    case FS_ENOSPC: case FS_EFBIG: *e=(struct dos_file_error){39,1,4,2}; break;
    case FS_EEXIST: *e=(struct dos_file_error){80,12,3,2}; break;
    default: break;
    }
}
void dos_file_init(struct dos_file *d,uint32_t id,uint32_t generation,const struct dos_file_ops *ops) {
    memset(d,0,sizeof(*d)); d->id=id; d->generation=generation; d->ops=*ops; d->drive=2; d->a20=true;
    d->retry_pause=1; d->retry_count=3;
}
int dos_file_bind(struct dos_file *d,const struct dos_file_binding *b) {
    int e=0;
    if (d->bound || request_cancelled(d) || b->sft_count<5 || b->sft_count>DOS_FILE_SFT || b->sda_bytes<0x20 ||
        !check(d,b->sda,b->sda_bytes,true) || !check(d,b->sft,6+59*b->sft_count,true) ||
        !check(d,b->lists,8,true) || !check(d,b->indos,1,true) || !check(d,b->media,26,true) ||
        !check(d,b->selected_psp,2,true) || !check(d,b->dta,4,true)) e=BAD_POINTER;
    if (!e) { uint8_t p[4]; far_write(p,b->sft); d->binding=*b; write_guest(d,plus(b->lists,4),p,4); d->bound=true; }
    d->ops.release(d->ops.arg); return e;
}
int dos_file_map(struct dos_file *d,unsigned guest,struct vfs_table *table,unsigned native,bool readonly) {
    if (guest>=26 || native>=26 || !table || !table->vfs || !vfs_drive_present(table->vfs,native)) return BAD_DRIVE;
    d->view[guest]=(struct dos_file_view){table,native,table->vfs->generation[native],readonly}; return 0;
}
int dos_file_register_device(struct dos_file *d,const char *name,struct dos_file_far header,uint16_t info) {
    int e=0; size_t n=strlen(name); unsigned slot=DOS_FILE_DEVICES;
    if (!d->bound || d->owning || !n || n>8 || device_kind(name)==DOS_FILE_REGULAR || !(info&0x80)) return -FS_EINVAL;
    for (unsigned i=0;i<DOS_FILE_DEVICES;i++) if (!d->device[i].used) { slot=i; break; }
    if (slot==DOS_FILE_DEVICES) return -FS_EMFILE;
    if (device_index(d,name)>=0) return -FS_EEXIST;
    uint8_t driver[18];
    if (!check(d,header,sizeof(driver),false)) e=BAD_POINTER;
    else {
        read_guest(d,header,driver,sizeof(driver));
        if (!(fs_rd16(driver+4)&0x8000)) e=-FS_EINVAL;
        else {
            struct dos_file_device *dev=&d->device[slot];
            memset(dev,0,sizeof(*dev));
            for (unsigned i=0;i<n;i++) dev->name[i]=(char)path_upper((uint8_t)name[i]);
            dev->header=header; dev->info=info; dev->used=true;
        }
    }
    d->ops.release(d->ops.arg); return e;
}
int dos_file_register_dpb(struct dos_file *d,unsigned drive,struct dos_file_far pointer) {
    int e=drive>=26 || !d->view[drive].table ? BAD_DRIVE :
        !check(d,pointer,0x21,false) ? BAD_POINTER : 0;
    if (!e) d->dpb[drive]=pointer;
    d->ops.release(d->ops.arg); return e;
}
int dos_file_native_views(struct dos_file *d,struct vfs_table *t,bool removable_writable) {
    int e=dos_file_map(d,3,t,2,true); if (e) return e;
    unsigned stick=26;
    for (unsigned i=3;i<26;i++) if (vfs_drive_present(t->vfs,i) && (t->vfs->drive_flags[i]&VFS_DRIVE_REMOVABLE)) { stick=i; break; }
    if (stick<26 && (e=dos_file_map(d,4,t,stick,!removable_writable))) return e;
    for (unsigned i=3;i<26;i++) {
        if (i==stick || !vfs_drive_present(t->vfs,i)) continue;
        unsigned letter=i;
        if (d->view[letter].table || letter<5) {
            letter=5; while (letter<26 && d->view[letter].table) letter++;
            if (letter==26) return -FS_EMFILE;
        }
        e=dos_file_map(d,letter,t,i,!removable_writable); if (e) return e;
#ifndef FS_HOST
        klog("[dosvm] drive_view vm=%u gen=%u guest=%c source=%c readonly=%u",d->id,d->generation,'A'+letter,'A'+i,!removable_writable);
#endif
    }
    return 0;
}
static int release_slot(struct dos_file *d,struct dos_file_process *p,unsigned handle) {
    uint8_t index=p->handles[handle]; if (index==255) return -FS_EBADF;
    struct dos_file_description *s=&d->sft[index]; int e=0;
    if (s->kind==DOS_FILE_REGULAR) {
        s->table->owner=owner(d,p->psp);
        /* DOS closes an owner's locks even when a duplicate remains. */
        if (s->refs==1) {
            e=vfs_close(s->table,s->handle);
            if (e && s->table->handles[s->handle]) return e; /* admission failed: still owned */
        } else {
            struct vfs *v=s->table->vfs;
            e=vfs_admit(v,0,s->table->request); if (e) return e;
            struct vfs_description *description=s->table->handles[s->handle];
            for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) if (v->range_locks[i].description==description && v->range_locks[i].owner==s->table->owner)
                memset(&v->range_locks[i],0,sizeof(v->range_locks[i]));
            vfs_unlock(v);
        }
    }
    p->handles[handle]=255; if (!--s->refs) memset(s,0,sizeof(*s)); return e;
}
static int release_process(struct dos_file *d,struct dos_file_process *p) {
    int error=0;
    for (unsigned i=0;i<26;i++) if (d->view[i].table) d->view[i].table->request=0;
    for (unsigned i=0;i<DOS_FILE_JFT;i++) if (p->handles[i]!=255) {
        int e=release_slot(d,p,i); if (e && !error) error=e;
    }
    for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) if (d->cursor[i].used && d->cursor[i].psp==p->psp) d->cursor[i].used=false;
    for (unsigned i=0;i<DOS_FILE_OBSERVERS;i++) if (d->observer[i].used && d->observer[i].psp==p->psp) { d->observer[i].used=false; d->dropped++; }
    for (unsigned i=0;i<26;i++) if (d->view[i].table) vfs_release_owner(d->view[i].table->vfs,owner(d,p->psp));
    memset(p,0,sizeof(*p));
    if (error && !d->cleanup_status) d->cleanup_status=error;
    return error;
}
int dos_file_lifecycle(struct dos_file *d,enum dos_file_event event,uint16_t psp,uint16_t parent,struct dos_file_far expanded) {
    struct dos_file_process *p=process(d,psp),*ancestor=process(d,parent); int e=0;
    if (!d->bound || d->dead) return -FS_EINVAL;
    if (event==DOS_FILE_VM_DESTROY) { dos_file_cancel(d); return 0; }
    if (event==DOS_FILE_PSP_CREATE || event==DOS_FILE_EXEC_BEGIN) {
        if (p || (parent && !ancestor) || (!parent && d->owning)) return -FS_EINVAL;
        if (!parent && (device_index(d,"CON")<0 || device_index(d,"AUX")<0 || device_index(d,"PRN")<0)) return -FS_EOPNOTSUPP;
        for (unsigned i=0;i<DOS_FILE_PSPS;i++) if (!d->process[i].used) { p=&d->process[i]; break; }
        struct dos_file_far header={0x18,psp};
        if (!p) e=-FS_EMFILE;
        else if (!check(d,header,0x22,true) || !check(d,expanded,DOS_FILE_JFT,true)) e=BAD_POINTER;
        if (!e) {
            memset(p,0,sizeof(*p)); p->used=true; p->psp=psp; p->parent=parent;
            p->count=20; p->jft=header; p->dta=(struct dos_file_far){0x80,psp}; p->expanded=expanded;
            p->provisional=event==DOS_FILE_EXEC_BEGIN; memset(p->handles,255,sizeof(p->handles));
            if (ancestor) {
                for (unsigned i=0;i<20;i++) { uint8_t index=ancestor->handles[i];
                    if (index!=255 && !(d->sft[index].mode&0x80)) { p->handles[i]=index; d->sft[index].refs++; }
                }
            } else {
                for (unsigned i=0;i<5;i++) { p->handles[i]=(uint8_t)i;
                    d->sft[i]=(struct dos_file_description){.refs=1,.kind=i<3 ? DOS_FILE_CON : DOS_FILE_DEVICE,.owner=psp,.mode=2,
                        .device=(uint8_t)device_index(d,i<3 ? "CON" : i==3 ? "AUX" : "PRN")}; }
            }
            uint8_t info[6]; fs_wr16(info,20); far_write(info+2,p->jft); write_guest(d,(struct dos_file_far){0x32,psp},info,6);
            write_guest(d,p->jft,p->handles,20);
            if (!parent) d->selected=d->owning=psp;
        }
    } else if (!p) {
        if (event==DOS_FILE_PROCESS_EXIT && d->aborted_psp==psp) d->aborted_psp=0;
        else e=-FS_EBADF;
    }
    else if (event==DOS_FILE_EXEC_ROLLBACK || event==DOS_FILE_PROCESS_EXIT) {
        /* A dying PSP's forged JFT cannot prevent release of owned references. */
        uint16_t restore=p->parent; e=release_process(d,p); d->selected=restore;
        if (d->owning==psp) d->owning=restore;
    } else if ((e=preflight(d,p))) { }
    else if (event==DOS_FILE_PSP_SELECT) d->selected=psp;
    else if (event==DOS_FILE_EXEC_COMMIT) { p->provisional=false; d->selected=d->owning=psp; }
    else if (event==DOS_FILE_TSR) { p->resident=true; d->selected=d->owning=p->parent; }
    else e=-FS_EINVAL;
    p=process(d,d->selected);
    if (e && p) dos_file_error_from_fs(&p->error,e,false);
    if (p && !preflight(d,p)) { int snapshot=mirror(d,p,0); if (!e) e=snapshot; }
    d->ops.release(d->ops.arg); return e;
}
void dos_file_cancel(struct dos_file *d) {
    d->dead=true; d->critical=false; d->token=0;
    for (unsigned i=0;i<DOS_FILE_PSPS;i++) if (d->process[i].used) release_process(d,&d->process[i]);
    memset(d->observer,0,sizeof(d->observer));
    d->ops.release(d->ops.arg);
}
static bool request_cancelled(void *arg) {
    struct dos_file *d=arg;
    return d->dead || (d->ops.stopped && d->ops.stopped(d->ops.arg));
}
static uint64_t request_now(void *arg) { struct dos_file *d=arg; return d->ops.wall(d->ops.arg); }
static void request_pause(void *arg) { struct dos_file *d=arg; d->ops.pause(d->ops.arg); }
static void trace(struct dos_file *d,const struct dos_file_frame *f,uint64_t seq,unsigned origin,unsigned vector,
                  uint16_t psp,bool returning,uint64_t wall,uint64_t monitor,unsigned bytes) {
    if (!d->trace_enabled) return;
    if (d->trace_count==DOS_FILE_TRACE) { d->dropped++; return; }
    unsigned i=(d->trace_head+d->trace_count++)%DOS_FILE_TRACE;
    d->trace[i]=(struct dos_file_trace){*f,seq,wall,monitor,d->generation,bytes,d->dropped,psp,(uint8_t)origin,(uint8_t)vector,returning};
}
bool dos_file_trace_pop(struct dos_file *d,struct dos_file_trace *out) {
    if (!d->trace_count) return false; *out=d->trace[d->trace_head];
    d->trace_head=(d->trace_head+1)%DOS_FILE_TRACE; d->trace_count--; return true;
}
bool dos_file_wait_pop(struct dos_file *d,struct dos_file_wait *out) {
    if (!d->wait_count) return false; *out=d->waits[d->wait_head];
    d->wait_head=(d->wait_head+1)%DOS_FILE_WAITS; d->wait_count--; return true;
}
static void retain_wait(struct dos_file *d) {
    if (!d->wait.valid) return;
    if (d->wait_count==DOS_FILE_WAITS) d->dropped++;
    else {
        unsigned index=(d->wait_head+d->wait_count++)%DOS_FILE_WAITS;
        d->waits[index]=(struct dos_file_wait){d->wait,d->request_sequence,d->generation,d->selected,d->active_drive};
    }
    d->wait.valid=false;
}
void dos_file_observe_return(struct dos_file *d,const struct dos_file_frame *f) {
    if (!d || d->dead) return;
    struct dos_file_observer *best=0;
    for (unsigned i=0;i<DOS_FILE_OBSERVERS;i++) {
        struct dos_file_observer *o=&d->observer[i];
        if (o->used && o->input.cs==f->cs && o->input.ip==f->ip && o->input.ss==f->ss && o->input.sp==f->sp &&
            (!best || o->sequence>best->sequence)) best=o;
    }
    if (best) {
        trace(d,f,best->sequence,best->origin,best->vector,best->psp,true,d->ops.wall(d->ops.arg)-best->wall,
              d->ops.monitor(d->ops.arg)-best->monitor,best->bytes); best->used=false;
    }
}
bool dos_file_serves(unsigned vector,uint16_t ax) {
    if (vector==0x2f) return ax==0x1000;
    if (vector!=0x21) return false;
    unsigned ah=ax>>8;
    if (ah==0x43 || ah==0x57 || ah==0x5c) return (ax&255)<=1;
    if (ah==0x6c) return ax==0x6c00;
    return ah==0x0d || ah==0x0e || ah==0x19 || ah==0x1a || ah==0x1b || ah==0x1c || ah==0x2f || ah==0x36 ||
        (ah>=0x39 && ah<=0x47) || ah==0x4e || ah==0x4f || ah==0x56 || ah==0x57 || ah==0x59 ||
        ah==0x5a || ah==0x5b || ah==0x5c || ah==0x60 || ah==0x67 || ah==0x68 || ah==0x6c;
}
static int route(struct dos_file *d,const char *path,struct vfs_table **table,char native[FS_PATH_BYTES],unsigned *guest,bool write) {
    unsigned g=d->drive; const char *s=path;
    if (s[0] && s[1]==':') { g=(unsigned)(path_upper((uint8_t)s[0])-'A'); s+=2; }
    if (g>=26 || !d->view[g].table) return BAD_DRIVE;
    struct dos_file_view *v=&d->view[g];
    if (v->generation!=v->table->vfs->generation[v->drive] || !vfs_drive_present(v->table->vfs,v->drive)) return -FS_EAGAIN;
    if (write && v->readonly) return -FS_EACCES;
    if (strlen(s)+3>FS_PATH_BYTES) return -FS_ENAMETOOLONG;
    native[0]=(char)('A'+v->drive); native[1]=':'; memcpy(native+2,s,strlen(s)+1);
    *table=v->table; (*table)->owner=owner(d,d->selected); (*table)->request=&d->request; *guest=g; d->active_drive=(uint8_t)g; return 0;
}
static struct dos_file_description *get_handle(struct dos_file *d,struct dos_file_process *p,unsigned h) {
    if (h>=p->count || p->handles[h]==255) return 0;
    unsigned i=p->handles[h]; if (i>=d->binding.sft_count || !d->sft[i].refs) return 0;
    struct dos_file_description *s=&d->sft[i];
    if (s->table) { s->table->owner=owner(d,p->psp); s->table->request=&d->request; }
    return s;
}
static int handle_live(struct dos_file_description *s) {
    if (s->kind!=DOS_FILE_REGULAR) return 0;
    return s->table->vfs->generation[s->table->handles[s->handle]->node->drive]==s->generation ? 0 : -FS_EAGAIN;
}
static int free_handle(struct dos_file_process *p) {
    for (unsigned i=0;i<p->count;i++) if (p->handles[i]==255) return (int)i;
    return -FS_EMFILE;
}
static unsigned device_kind(const char *path) {
    const char *leaf=path; for (const char *s=path;*s;s++) if (*s=='/' || *s=='\\' || *s==':') leaf=s+1;
    char name[9]; unsigned n=0;
    while (*leaf && *leaf!='.' && *leaf!=':' && n<8) name[n++]=(char)path_upper((uint8_t)*leaf++);
    while (n && name[n-1]==' ') n--; name[n]=0;
    if (!strncmp(name,"NUL",9)) return DOS_FILE_NUL;
    if (!strncmp(name,"CON",9)) return DOS_FILE_CON;
    if (!strncmp(name,"AUX",9) || !strncmp(name,"PRN",9) ||
        (n==4 && (name[3]>='1' && name[3]<='9') && ((!memcmp(name,"COM",3)) || !memcmp(name,"LPT",3)))) return DOS_FILE_DEVICE;
    return DOS_FILE_REGULAR;
}
static int open_file(struct dos_file *d,struct dos_file_process *p,const char *path,unsigned mode,unsigned flags,uint8_t attr,unsigned *status) {
    if ((mode&0xf)>2 || (mode&0x70)>0x40 || mode&~0x60ffu) return BAD_MODE;
    int h=flags&VFS_FAIL_EXISTING ? 0 : free_handle(p); if (h<0) return h;
    unsigned index; for (index=0;index<d->binding.sft_count;index++) if (!d->sft[index].refs) break;
    if (index==d->binding.sft_count) { if (flags&VFS_FAIL_EXISTING) index=0; else return -FS_EMFILE; }
    struct dos_file_description *s=&d->sft[index]; unsigned kind=device_kind(path),g=d->drive;
    struct vfs_table *table=0; int fd=-1,e=0,dev=0;
    if (kind==DOS_FILE_REGULAR) {
        char native[FS_PATH_BYTES];
        unsigned access=(mode&3)==0 ? VFS_READ : (mode&3)==1 ? VFS_WRITE : VFS_READ|VFS_WRITE;
        e=route(d,path,&table,native,&g,!(flags&VFS_FAIL_EXISTING) && ((access&VFS_WRITE)!=0 || (flags&(VFS_CREATE|VFS_TRUNCATE))!=0)); if (e) return e;
        static const enum vfs_share shares[]={VFS_DENY_ALL,VFS_DENY_ALL,VFS_DENY_WRITE,VFS_DENY_READ,VFS_DENY_NONE};
        fd=vfs_open_claim(table,native,access|flags,shares[(mode>>4)&7],attr,(mode>>4)&7,status);
        if (fd<0) return fd;
    } else {
        if (flags&VFS_FAIL_EXISTING) return -FS_EEXIST;
        dev=device_index(d,path); if (dev<0) return dev;
        if (status) *status=1;
    }
    *s=(struct dos_file_description){.refs=1,.kind=kind,.mode=mode,.table=table,.handle=fd,.owner=p->psp,
        .drive=(uint8_t)g,.device=(uint8_t)dev,.generation=table ? table->vfs->generation[d->view[g].drive] : 0};
    p->handles[h]=(uint8_t)index; return h;
}
/* A missing ancestor is not the same DOS error as a missing final component. */
static int path_error(struct vfs_table *t,char *native,int error) {
    if (error!=-FS_ENOENT) return error;
    char *leaf=0; for (char *s=native+2;*s;s++) if (*s=='/' || *s=='\\') leaf=s;
    if (!leaf || leaf==native+2) return error;
    char save=*leaf; *leaf=0; struct fat_entry ent; int e=vfs_stat(t,native,&ent); *leaf=save;
    return e==-FS_ENOENT || e==-FS_ENOTDIR ? BAD_PARENT : error;
}
static int search(struct dos_file *d,struct dos_file_process *p,struct dos_file_frame *f,bool first) {
    if (!check(d,p->dta,43,true)) return BAD_POINTER;
    struct dos_file_cursor *c=0; uint8_t bytes[43]; struct fat_entry entry; int e=0;
    read_guest(d,p->dta,bytes,43);
    if (first) {
        char path[FS_PATH_BYTES],native[FS_PATH_BYTES]; struct vfs_table *t; unsigned g;
        e=guest_string(d,f->ds,f->dx,path); if (e) return e;
        e=route(d,path,&t,native,&g,false); if (e) return e;
        for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) if (!d->cursor[i].used) { c=&d->cursor[i]; break; }
        if (!c) return -FS_EMFILE;
        struct vfs_find find;
        e=vfs_find_first(t,native,(uint8_t)((f->cx&0x3f)|VFS_FIND_DOS),&find,&entry);
        if (e) { if (e!=-FS_ENOENT) return e; e=path_error(t,native,e); return e==-FS_ENOENT ? NO_MORE : e; }
        *c=(struct dos_file_cursor){.used=true,.id=++d->search_sequence,.nonce=d->search_sequence^d->generation^0xd05f49u,
            .psp=p->psp,.drive=(uint8_t)g,.find=find,.table=t};
    } else {
        if (fs_rd16(bytes)!=0x49df || fs_rd32(bytes+10)!=d->generation || fs_rd16(bytes+14)!=p->psp) return NO_MORE;
        for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) if (d->cursor[i].used && d->cursor[i].id==fs_rd32(bytes+2) &&
            (d->cursor[i].nonce ^ (fs_rd32(bytes+16)*0x9e3779b1u))==fs_rd32(bytes+6) && d->cursor[i].psp==p->psp) { c=&d->cursor[i]; break; }
        if (!c) return NO_MORE;
        /* Each copied DTA has its own cursor position. The bounded live record
         * proves authority; the DTA contains a checked replay position. */
        struct vfs_find find=c->find; find.cursor=fs_rd32(bytes+16);
        if (find.cursor>c->find.cursor) return NO_MORE;
        c->table->request=&d->request; c->table->owner=owner(d,p->psp); d->active_drive=c->drive;
        e=vfs_find_next(c->table,&find,&entry);
        if (e) { c->used=false; return e==-FS_ENOENT ? NO_MORE : e; }
        if (find.cursor>c->find.cursor) c->find.cursor=find.cursor;
        fs_wr32(bytes+16,find.cursor);
    }
    uint32_t position=first ? c->find.cursor : fs_rd32(bytes+16);
    memset(bytes,0,sizeof(bytes)); fs_wr16(bytes,0x49df); fs_wr32(bytes+2,c->id);
    fs_wr32(bytes+6,c->nonce ^ (position*0x9e3779b1u));
    fs_wr32(bytes+10,d->generation); fs_wr16(bytes+14,p->psp); fs_wr32(bytes+16,position);
    bytes[21]=entry.attr; fs_wr16(bytes+22,entry.times.write_time); fs_wr16(bytes+24,entry.times.write_date); fs_wr32(bytes+26,entry.size);
    const char *name=entry.alias[0] ? entry.alias : entry.name;
    int conversion=dos_name(name,(char *)bytes+30,13,true);
    if (conversion) { c->used=false; return conversion; } write_guest(d,p->dta,bytes,43); return 0;
}
static int service(struct dos_file *d,struct dos_file_process *p,struct dos_file_frame *f) {
    unsigned ah=f->ax>>8,al=f->ax&255; int e=0; char path[FS_PATH_BYTES],native[FS_PATH_BYTES];
    struct vfs_table *t=0; unsigned g=0; struct fat_entry ent;
    if (d->vector==0x2f) { f->ax=(uint16_t)((f->ax&0xff00)|255); return 0; }
    if (ah==0x59) {
        if (f->bx) return -FS_EOPNOTSUPP;
        f->ax=p->error.code; f->bx=(uint16_t)((p->error.class_<<8)|p->error.action); f->cx=(uint16_t)((f->cx&255)|(p->error.locus<<8)); return 0;
    }
    if (ah==0x1a) { struct dos_file_far a={f->dx,f->ds}; if (!check(d,a,43,true)) return BAD_POINTER; p->dta=a; return 0; }
    if (ah==0x2f) { f->es=p->dta.seg; f->bx=p->dta.off; return 0; }
    if (ah==0x19) { f->ax=(uint16_t)((f->ax&0xff00)|d->drive); return 0; }
    if (ah==0x0e) { unsigned drive=f->dx&255; if (drive<26 && d->view[drive].table) d->drive=(uint8_t)drive;
        unsigned last=2; for (unsigned i=0;i<26;i++) if (d->view[i].table) last=i+1; f->ax=(uint16_t)((f->ax&0xff00)|last); return 0; }
    if (ah==0x67) {
        unsigned count=f->bx<=20 ? 20 : f->bx; if (count>DOS_FILE_JFT) return -FS_EMFILE;
        for (unsigned i=count;i<DOS_FILE_JFT;i++) if (p->handles[i]!=255) return -FS_EMFILE;
        struct dos_file_far jft=count>20 ? p->expanded : (struct dos_file_far){0x18,p->psp};
        if (!check(d,jft,count,true)) return BAD_POINTER;
        p->count=(uint16_t)count; p->jft=jft; uint8_t b[6]; fs_wr16(b,(uint16_t)count); far_write(b+2,jft);
        write_guest(d,(struct dos_file_far){0x32,p->psp},b,6); return 0;
    }
    if (ah==0x4e || ah==0x4f) return search(d,p,f,ah==0x4e);
    if (ah==0x36 || ah==0x1b || ah==0x1c) {
        g=ah==0x1b || !(f->dx&255) ? d->drive : (f->dx&255)-1;
        if (g>=26 || !d->view[g].table) { f->ax=ah==0x36 ? 65535 : (uint16_t)((f->ax&0xff00)|255); return 0; }
        t=d->view[g].table; struct fat_volume *vol=t->vfs->volumes[d->view[g].drive];
        if (!vol) return -FS_EOPNOTSUPP;
        t->request=&d->request; t->owner=owner(d,p->psp); d->active_drive=(uint8_t)g;
        uint64_t bytes; uint32_t cb; e=vfs_free_space(t,d->view[g].drive,&bytes,&cb); if (e) return e;
        if (ah==0x36) { f->ax=vol->spc; f->cx=512; f->bx=(uint16_t)(vol->free_clusters>65535 ? 65535 : vol->free_clusters);
            f->dx=(uint16_t)(vol->clusters>65535 ? 65535 : vol->clusters); }
        else {
            uint8_t media=vol->media; struct dos_file_far pointer=plus(d->binding.media,g);
            if (!check(d,pointer,1,true)) return BAD_POINTER;
            write_guest(d,pointer,&media,1); f->ax=(uint16_t)((f->ax&0xff00)|vol->spc);
            f->cx=512; f->dx=(uint16_t)(vol->clusters>65535 ? 65535 : vol->clusters);
            f->ds=pointer.seg; f->bx=pointer.off;
        }
        return 0;
    }
    if (ah==0x0d) {
        for (unsigned i=0;i<26;i++) if (d->view[i].table && !d->view[i].readonly) {
            bool duplicate=false;
            for (unsigned j=0;j<i;j++) if (d->view[j].table==d->view[i].table && d->view[j].drive==d->view[i].drive && !d->view[j].readonly) duplicate=true;
            if (duplicate) continue;
            d->view[i].table->request=&d->request; d->active_drive=(uint8_t)i;
            e=vfs_flush_drive(d->view[i].table,d->view[i].drive); if (e) return e;
        }
        return 0;
    }
    if (ah==0x44) {
        if (al==0xb) { d->retry_pause=f->cx; if (f->dx) d->retry_count=f->dx; return 0; }
        if (al==8 || al==9 || al==0xe) {
            g=(f->bx&255) ? (f->bx&255)-1 : d->drive;
            if (g>=26 || !d->view[g].table) return BAD_DRIVE;
            t=d->view[g].table; bool removable=(t->vfs->drive_flags[d->view[g].drive]&VFS_DRIVE_REMOVABLE)!=0;
            if (al==8) f->ax=removable ? 0 : 1;
            else if (al==9) f->dx=0;
            else f->ax=(uint16_t)(f->ax&0xff00); return 0;
        }
    }
    if (ah==0x3e || ah==0x3f || ah==0x40 || ah==0x42 || ah==0x44 || ah==0x45 || ah==0x46 || ah==0x57 || ah==0x5c || ah==0x68) {
        struct dos_file_description *s=get_handle(d,p,f->bx); if (!s) return -FS_EBADF;
        d->active_drive=s->drive;
        if ((e=handle_live(s))) return e;
        if (ah==0x3e) return release_slot(d,p,f->bx);
        if (ah==0x45 || ah==0x46) {
            int target=ah==0x45 ? free_handle(p) : f->cx;
            if (target<0) return target; if ((unsigned)target>=p->count) return -FS_EBADF;
            if (target!=(int)f->bx) {
                if (p->handles[target]!=255) { e=release_slot(d,p,(unsigned)target); if (e) return e; }
                s->refs++; p->handles[target]=p->handles[f->bx];
            }
            if (ah==0x45) f->ax=(uint16_t)target; return 0;
        }
        if (ah==0x44) {
            if (al==0) { f->dx=s->kind==DOS_FILE_REGULAR ? file_info(s) : d->device[s->device].info; return 0; }
            if (al==6 || al==7) { if (s->kind!=DOS_FILE_REGULAR && s->kind!=DOS_FILE_NUL) return -FS_EOPNOTSUPP;
                unsigned ready=255;
                if (al==6) {
                    if (s->kind!=DOS_FILE_NUL) { uint64_t at; e=vfs_handle_stat(s->table,s->handle,&ent,&at); if (e) return e; ready=at<ent.size ? 255 : 0; }
                }
                f->ax=(uint16_t)((f->ax&0xff00)|ready); return 0; }
            if (al==0xa) { f->dx=0; return 0; }
            return -FS_EOPNOTSUPP;
        }
        if (ah==0x42 && al>2) return -FS_EOPNOTSUPP;
        if (s->kind!=DOS_FILE_REGULAR) {
            if (s->kind==DOS_FILE_NUL && (ah==0x3f || ah==0x40)) { f->ax=ah==0x3f ? 0 : f->cx; return 0; }
            if (ah==0x42) { f->ax=f->dx=0; return 0; }
            return -FS_EOPNOTSUPP;
        }
        t=s->table;
        if (ah==0x68) return vfs_commit(t,s->handle);
        if (ah==0x5c) { if (al>1) return -FS_EOPNOTSUPP; return vfs_lock_range(t,s->handle,((uint32_t)f->cx<<16)|f->dx,((uint32_t)f->si<<16)|f->di,al==1); }
        if (ah==0x57) {
            if (al>1) return -FS_EOPNOTSUPP;
            if (al==1) return vfs_handle_times(t,s->handle,f->dx,f->cx);
            e=vfs_handle_stat(t,s->handle,&ent,0); if (!e) { f->cx=ent.times.write_time; f->dx=ent.times.write_date; } return e;
        }
        uint64_t position; e=vfs_handle_stat(t,s->handle,&ent,&position); if (e) return e;
        if (ah==0x42) {
            if (al>2) return -FS_EOPNOTSUPP;
            int32_t off=(int32_t)(((uint32_t)f->cx<<16)|f->dx);
            int64_t base=al==0 ? 0 : al==1 ? (int64_t)position : ent.size;
            int64_t next=base+off;
            e=vfs_seek(t,s->handle,(uint32_t)next,VFS_SEEK_SET,&position);
            if (!e) { s->negative_position=next<0; f->dx=(uint16_t)(position>>16); f->ax=(uint16_t)position; } return e;
        }
        bool writing=ah==0x40;
        if (s->negative_position) return -FS_EACCES;
        if (writing && d->view[s->drive].readonly) return -FS_EACCES;
        struct dos_file_far data={f->dx,f->ds};
        if (writing && !f->cx) {
            e=vfs_truncate(t,s->handle,position);
            if (!e) {
                f->ax=0; s->written=true;
                if (s->mode&0x4000) { e=vfs_commit(t,s->handle); d->pending_commit=e!=0; }
            }
            return e;
        }
        if (f->cx && !check(d,data,f->cx,!writing)) return BAD_POINTER;
        size_t total=0;
        while (total<f->cx) {
            if ((e=vfs_request_check(&d->request))) break;
            size_t n=f->cx-total; if (n>sizeof(d->transfer)) n=sizeof(d->transfer); size_t done=0;
            if (writing) { read_guest(d,plus(data,(unsigned)total),d->transfer,n); e=vfs_write(t,s->handle,d->transfer,n,&done); }
            else { e=vfs_read(t,s->handle,d->transfer,n,&done);
                if (!request_cancelled(d) && done) write_guest(d,plus(data,(unsigned)total),d->transfer,done); }
            total+=done; if (e || done<n || d->dead) break;
        }
        d->transferred=(uint32_t)total;
        if (writing && total) s->written=true;
        if (writing && total && e) { dos_file_error_from_fs(&p->error,e,true); f->ax=(uint16_t)total; return 0; }
        if (!e) f->ax=(uint16_t)total;
        if (!e && writing && (s->mode&0x4000)) { e=vfs_commit(t,s->handle); d->pending_commit=e!=0; }
        return e;
    }
    if (ah==0x47) {
        g=(f->dx&255) ? (f->dx&255)-1 : d->drive;
        if (g>=26 || !d->view[g].table) return BAD_DRIVE;
        t=d->view[g].table; t->request=&d->request; t->owner=owner(d,p->psp); d->active_drive=(uint8_t)g;
        e=vfs_getcwd(t,d->view[g].drive,path); if (e) return e;
        char result[64]; e=dos_name(path+1,result,sizeof(result),true); if (e) return e;
        size_t n=strlen(result)+1; if (!check(d,(struct dos_file_far){f->si,f->ds},n,true)) return BAD_POINTER;
        write_guest(d,(struct dos_file_far){f->si,f->ds},result,n); f->ax=0x0100; return 0;
    }
    if (ah==0x57 || ah==0x59 || ah==0x5c || ah==0x6c) {
        if ((ah==0x6c && al) || (ah==0x59 && f->bx)) return -FS_EOPNOTSUPP;
    }
    e=guest_string(d,f->ds,ah==0x6c ? f->si : f->dx,path); if (e) return e;
    if (ah==0x60) {
        char full[FS_PATH_BYTES]; unsigned drive; struct dos_file_far output={f->di,f->es};
        e=route(d,path,&t,native,&g,false); if (e) return e;
        e=path_normalize(native,d->view[g].drive,t->cwd[d->view[g].drive],&drive,full); if (e) return e;
        native[0]=(char)('A'+g); native[1]=':';
        e=dos_name(full,native+2,126,true); if (e) return e;
        size_t n=strlen(native)+1; if (!check(d,output,n,true)) return BAD_POINTER;
        write_guest(d,output,native,n); return 0;
    }
    if (ah==0x3c || ah==0x3d || ah==0x5a || ah==0x5b || ah==0x6c) {
        unsigned flags=0,mode=ah==0x3d ? al : ah==0x6c ? f->bx : 2,status=0;
        if (ah!=0x3d && (f->cx&~0x27u)) return -FS_EACCES;
        if (ah==0x3c) flags=VFS_CREATE|VFS_TRUNCATE;
        if (ah==0x5b || ah==0x5a) flags=VFS_CREATE|VFS_EXCLUSIVE;
        if (ah==0x6c) {
            if (al || f->dx&0xff00 || (f->dx&15)>2 || (f->dx>>4)>1 || f->bx&0x9f00) return -FS_EINVAL;
            if (f->dx&0x10) flags|=VFS_CREATE;
            if (!(f->dx&15)) { if (flags&VFS_CREATE) flags|=VFS_EXCLUSIVE; else flags|=VFS_FAIL_EXISTING; }
            if ((f->dx&15)==2) flags|=VFS_TRUNCATE;
        }
        if (ah==0x5a) {
            char result[FS_PATH_BYTES]; e=dos_name(path,result,sizeof(result),false); if (e) return e;
            size_t n=strlen(path),encoded=strlen(result);
            if (n+13>=FS_PATH_BYTES || !check(d,(struct dos_file_far){f->dx,f->ds},encoded+13,true)) return BAD_POINTER;
            if (n && path[n-1]!='/' && path[n-1]!='\\') path[n++]='\\';
            static const char hex[]="0123456789ABCDEF";
            for (unsigned attempt=0;attempt<65536;attempt++) {
                uint32_t value=++d->temp_sequence;
                for (unsigned i=0;i<8;i++) path[n+i]=hex[(value>>((7-i)*4))&15]; path[n+8]=0;
                e=open_file(d,p,path,mode,flags,(uint8_t)f->cx,&status);
                if (e!=-FS_EEXIST) break;
            }
            if (e>=0) { (void)dos_name(path,result,sizeof(result),false);
                write_guest(d,(struct dos_file_far){f->dx,f->ds},result,strlen(result)+1); }
        } else e=open_file(d,p,path,mode,flags,(uint8_t)f->cx,&status);
        if (e<0) return e; f->ax=(uint16_t)e; if (ah==0x6c) f->cx=(uint16_t)status; return 0;
    }
    e=route(d,path,&t,native,&g,ah==0x39 || ah==0x3a || ah==0x41 || ah==0x56 || (ah==0x43 && al==1)); if (e) return e;
    if (ah==0x39) e=vfs_mkdir(t,native,0);
    else if (ah==0x3a) {
        e=vfs_stat(t,native,&ent);
        if (!e && t->cwd_cluster[d->view[g].drive] && ent.first==t->cwd_cluster[d->view[g].drive]) return CURRENT_DIRECTORY;
        if (!e) e=vfs_rmdir(t,native);
    }
    else if (ah==0x3b) e=vfs_chdir(t,native);
    else if (ah==0x41) e=vfs_delete(t,native);
    else if (ah==0x43) {
        if (al==0) { e=vfs_stat(t,native,&ent); if (!e) f->cx=ent.attr; }
        else if (al==1) {
            if (f->cx&~0x27u) return -FS_EACCES;
            e=vfs_stat(t,native,&ent); if (!e) e=vfs_attrib(t,native,(uint8_t)(f->cx|(ent.attr&FAT_ATTR_DIR)));
        } else e=-FS_EOPNOTSUPP;
    } else if (ah==0x56) {
        char dest[FS_PATH_BYTES]; struct vfs_table *other; unsigned gd;
        e=guest_string(d,f->es,f->di,path); if (e) return e;
        e=route(d,path,&other,dest,&gd,true); if (e) return e;
        if (other->vfs!=t->vfs || d->view[gd].drive!=d->view[g].drive) return -FS_EXDEV;
        e=vfs_rename(t,native,dest);
    } else e=-FS_EOPNOTSUPP;
    return path_error(t,native,e);
}
static void clear_requests(struct dos_file *d) {
    for (unsigned i=0;i<26;i++) if (d->view[i].table) d->view[i].table->request=0;
}
static void observe(struct dos_file *d,const struct dos_file_frame *f,uint64_t seq,uint64_t wall,uint64_t monitor,unsigned origin,unsigned vector) {
    if (!d->trace_enabled) return;
    unsigned i; for (i=0;i<DOS_FILE_OBSERVERS;i++) if (!d->observer[i].used) break;
    if (i==DOS_FILE_OBSERVERS) d->dropped++;
    else d->observer[i]=(struct dos_file_observer){true,*f,seq,wall,monitor,d->selected,(uint8_t)origin,(uint8_t)vector,0};
}
static void trace_return(struct dos_file *d,const struct dos_file_frame *f) {
    if (d->origin==DOS_FILE_DPMI_0300 || d->origin==DOS_FILE_EXTENDER || d->origin==DOS_FILE_HOST) {
        for (unsigned i=0;i<DOS_FILE_OBSERVERS;i++) if (d->observer[i].used && d->observer[i].sequence==d->request_sequence)
            d->observer[i].bytes=d->transferred;
        return;
    }
    trace(d,f,d->request_sequence,d->origin,d->vector,d->selected,true,d->ops.wall(d->ops.arg)-d->request_wall,
        d->ops.monitor(d->ops.arg)-d->request_monitor,d->transferred);
}
static bool critical_frame(struct dos_file *d,const struct dos_file_frame *original,uint8_t flags,uint8_t cause) {
    struct dos_file_far ivt={0x24*4,0},stack={(uint16_t)(original->sp-30),original->ss};
    uint8_t vector[4],bytes[30];
    if (original->sp<30 || !check(d,ivt,4,false) || !check(d,stack,30,true) ||
        !check(d,d->binding.critical_return,1,false) || !check(d,d->binding.critical_device,18,false)) return false;
    read_guest(d,ivt,vector,4); struct dos_file_far handler=far_read(vector);
    if ((!handler.seg && !handler.off) || !check(d,handler,1,false)) return false;
    uint16_t words[15]={d->binding.critical_return.off,d->binding.critical_return.seg,original->flags,
        original->ax,original->bx,original->cx,original->dx,original->si,original->di,original->bp,
        original->ds,original->es,original->ip,original->cs,original->flags};
    for (unsigned i=0;i<15;i++) fs_wr16(bytes+2*i,words[i]);
    write_guest(d,stack,bytes,sizeof(bytes)); d->callback=*original;
    d->callback.cs=handler.seg; d->callback.ip=handler.off; d->callback.sp=stack.off;
    d->callback.ax=(uint16_t)((flags<<8)|d->active_drive); d->callback.di=cause;
    d->callback.bp=d->binding.critical_device.seg; d->callback.si=d->binding.critical_device.off;
    return true;
}
static enum dos_file_result execute(struct dos_file *d,struct dos_file_frame *f,bool retry) {
    struct dos_file_process *p=process(d,d->selected); struct dos_file_frame output=*f;
    int e=preflight(d,p); bool memory_ok=!e;
    if (!e && retry && d->pending_commit) {
        struct dos_file_description *s=get_handle(d,p,f->bx);
        e=!s ? -FS_EBADF : handle_live(s);
        if (!e) e=vfs_commit(s->table,s->handle);
        if (!e) { d->pending_commit=false; output.ax=(uint16_t)d->transferred; }
    } else if (!e) e=service(d,p,&output);
    clear_requests(d);
    if (memory_ok && !retry && (e==-VFS_ECRITICAL_SHARE || (e==-VFS_ELOCK && ((f->ax>>8)==0x3f || (f->ax>>8)==0x40)))) {
        for (unsigned attempt=0;attempt<d->retry_count && e;attempt++) {
            uint64_t until=d->ops.wall(d->ops.arg)+(uint64_t)(d->retry_pause ? d->retry_pause : 1)*1000;
            if (until>d->deadline) until=d->deadline;
            while (d->ops.wall(d->ops.arg)<until && !vfs_request_check(&d->request)) d->ops.pause(d->ops.arg);
            if (vfs_request_check(&d->request)) break;
            output=*f; e=service(d,p,&output); clear_requests(d);
            if (e!=-VFS_ECRITICAL_SHARE && e!=-VFS_ELOCK) break;
        }
    }
    if (request_cancelled(d)) { d->ops.release(d->ops.arg); return DOS_FILE_PENDING; }
    if (e) {
        if (!p) { f->ax=6; f->flags|=1; retain_wait(d); trace_return(d,f); d->ops.release(d->ops.arg); return DOS_FILE_DONE; }
        bool writing=(f->ax>>8)==0x40; dos_file_error_from_fs(&p->error,e,writing);
        output=*f; output.ax=p->error.code; output.flags|=1;
        if (e==-VFS_ESHARE || e==-VFS_ECRITICAL_SHARE) output.ax=5;
        bool critical=e==-VFS_ECRITICAL_SHARE || e==-FS_EIO || e==-FS_EAGAIN || e==-FS_EROFS ||
            (e==-VFS_ELOCK && ((f->ax>>8)==0x3f || (f->ax>>8)==0x40));
        bool no_critical=(f->ax==0x6c00 && (f->bx&0x2000));
        unsigned ah=f->ax>>8;
        if ((ah==0x3f || ah==0x40 || ah==0x68 || ah==0x57) && f->bx<p->count && p->handles[f->bx]!=255)
            no_critical|=(d->sft[p->handles[f->bx]].mode&0x2000)!=0;
        if (critical && memory_ok && !retry && !no_critical && d->ops.critical && !vfs_request_check(&d->request)) {
            uint8_t flags=(uint8_t)(0x18|(writing ? 1 : 0));
            uint8_t cause=(uint8_t)(p->error.code>=19 && p->error.code<=36 ? p->error.code-19 : 12);
            if (!critical_frame(d,f,flags,cause)) goto complete;
            d->pending=*f; d->critical=true; d->token=(d->request_sequence<<32)|d->generation;
            int snapshot=mirror(d,p,&d->request);
            if (snapshot) { d->critical=false; d->token=0; e=snapshot; goto complete; }
            d->ops.release(d->ops.arg);
            if (d->ops.critical(d->ops.arg,d->token,&d->callback,flags,cause)) return DOS_FILE_PENDING;
            if (d->dead) return DOS_FILE_PENDING;
            d->critical=false; d->token=0;
            memory_ok=!preflight(d,p);
        }
    } else output.flags&=(uint16_t)~1u;
complete:
    if (memory_ok) {
        int snapshot=mirror(d,p,&d->request);
        if (!e && snapshot) { dos_file_error_from_fs(&p->error,snapshot,(f->ax>>8)==0x40);
            output=*f; output.ax=p->error.code; output.flags|=1; }
    }
    *f=output;
    retain_wait(d);
    trace_return(d,f);
    d->ops.release(d->ops.arg); return DOS_FILE_DONE;
}
enum dos_file_result dos_file_dispatch(struct dos_file *d,struct dos_file_frame *f,enum dos_file_origin origin,unsigned vector) {
    if (!d || !d->bound || d->dead) return DOS_FILE_CHAIN;
    if (d->critical && !(vector==0x21 && (f->ax>>8)==0x59 && !f->bx)) {
        if (vector==0x21 && (!(f->ax>>8) || (f->ax>>8)>0x0c)) {
            uint64_t sequence=++d->sequence;
            trace(d,f,sequence,origin,vector,d->selected,false,0,0,0);
            f->ax=5; f->flags|=1;
            trace(d,f,sequence,origin,vector,d->selected,true,0,0,0); return DOS_FILE_DONE;
        }
        return DOS_FILE_CHAIN;
    }
    /* Device transfers enter the guest driver in the original interrupt frame. */
    struct dos_file_process *p=process(d,d->selected);
    bool chain=!dos_file_serves(vector,f->ax);
    if (vector==0x21 && (f->ax>>8)==0x59 && f->bx) chain=true;
    if (!chain && p && vector==0x21 && ((f->ax>>8)==0x3f || (f->ax>>8)==0x40)) {
        struct dos_file_description *s=get_handle(d,p,f->bx); if (s && s->kind!=DOS_FILE_REGULAR && s->kind!=DOS_FILE_NUL) chain=true;
    }
    if (!chain && p && vector==0x21 && (f->ax>>8)==0x44) {
        unsigned sub=f->ax&255;
        struct dos_file_description *s=get_handle(d,p,f->bx);
        if (s && s->kind!=DOS_FILE_REGULAR &&
            (sub==1 || sub==2 || sub==3 || sub==0x0c || ((sub==6 || sub==7) && s->kind!=DOS_FILE_NUL))) chain=true;
    }
    uint64_t seq=++d->sequence,wall=d->ops.wall(d->ops.arg),monitor=d->ops.monitor(d->ops.arg);
    trace(d,f,seq,origin,vector,d->selected,false,0,0,0);
    if (chain) {
        observe(d,f,seq,wall,monitor,origin,vector);
        clear_requests(d); return DOS_FILE_CHAIN;
    }
    if (d->critical) {
        /* Nested 59h reads context without replacing the suspended request. */
        struct dos_file_frame output=*f; (void)service(d,p,&output); *f=output;
        trace(d,f,seq,origin,vector,d->selected,true,0,0,0); d->ops.release(d->ops.arg); return DOS_FILE_DONE;
    }
    d->request_sequence=seq; d->origin=origin; d->vector=(uint8_t)vector; d->transferred=0; d->pending_commit=false;
    if (origin==DOS_FILE_DPMI_0300 || origin==DOS_FILE_EXTENDER || origin==DOS_FILE_HOST) observe(d,f,seq,wall,monitor,origin,vector);
    d->request_wall=wall; d->request_monitor=monitor; d->deadline=wall+DOS_FILE_BUDGET_US;
    memset(&d->wait,0,sizeof(d->wait));
    d->request=(struct vfs_request){d,d->deadline,request_now,request_cancelled,request_pause,owner(d,d->selected),&d->wait};
    return execute(d,f,false);
}
enum dos_file_result dos_file_complete(struct dos_file *d,uint64_t token,unsigned reply,struct dos_file_frame *f) {
    if (request_cancelled(d) || !d->critical || token!=d->token || (uint32_t)token!=d->generation) return DOS_FILE_PENDING;
    d->critical=false; d->token=0; *f=d->pending;
    struct dos_file_process *p=process(d,d->selected);
    if (reply==2) { uint16_t psp=d->owning ? d->owning : d->selected; p=process(d,psp);
        d->aborted_psp=psp;
        if (d->ops.abort_process) d->ops.abort_process(d->ops.arg,psp);
        if (p) { uint16_t parent=p->parent; release_process(d,p); d->selected=d->owning=parent; }
        for (unsigned i=0;i<DOS_FILE_OBSERVERS;i++) if (d->observer[i].used && d->observer[i].sequence==d->request_sequence) { d->observer[i].used=false; d->dropped++; }
        d->ops.release(d->ops.arg); return DOS_FILE_PENDING; }
    if (reply==1 && !vfs_request_check(&d->request)) return execute(d,f,true);
    /* Ignore is not offered for these file requests. Preserve the cause in
     * 59h while returning DOS's fail-result code to the interrupted call. */
    f->ax=83; f->flags|=1;
    if (p && !preflight(d,p)) (void)mirror(d,p,&d->request);
    retain_wait(d);
    trace_return(d,f);
    d->ops.release(d->ops.arg); return DOS_FILE_DONE;
}
bool dos_file_poll(struct dos_file *d,struct dos_file_frame *f) {
    if (!d || !d->critical || d->dead || d->ops.wall(d->ops.arg)<d->deadline) return false;
    return dos_file_complete(d,d->token,3,f)==DOS_FILE_DONE;
}
