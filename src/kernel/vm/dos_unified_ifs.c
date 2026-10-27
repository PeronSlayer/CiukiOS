/* f3-57 live native backend of the unified CiukiDOS binding. See the header
 * and docs/validation/f3-57-unified-file-channel.md (register conventions:
 * K2043 int2f.asm:353-485 and dosfns.c callers; RBIL INT 2F/11xxh).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dos_unified_ifs.h"
/* dos_file_error_from_fs() shares dos_file.c's internal error namespace. */
#define IFS_BAD_POINTER (-207)
#define IFS_BAD_DRIVE (-208)
#define IFS_BAD_MODE (-209)
#define IFS_NO_MORE (-210)
#define IFS_BAD_PARENT (-211)
#define IFS_CURRENT_DIRECTORY (-212)
#define IFS_CHAIN 1                 /* not an owned request: run the original INT 2Fh */
#define IFS_DOS(code) (-(1000+(code)))   /* an explicit DOS error code */
#define SFT_FSHARED 0x8000u
#define SFT_FDATE 0x4000u
#define SFT_FCLEAN 0x0040u
#define O_NOCRIT 0x2000u
#define O_SYNC 0x4000u

static uint32_t lin(struct dos_file_far p) { return ((uint32_t)p.seg<<4)+p.off; }
static struct dos_file_far plus(struct dos_file_far p,unsigned off) { p.off=(uint16_t)(p.off+off); return p; }
static struct dos_file_far at(uint16_t seg,uint16_t off) { return (struct dos_file_far){off,seg}; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return u16(p)|(uint32_t)u16(p+2)<<16; }
static void w16(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p,uint32_t v) { w16(p,v); w16(p+2,v>>16); }
static bool grd(struct dfv2_ifs *s,struct dos_file_far p,void *b,size_t n) {
    if (!n || n>65536u-p.off || !s->ops.check(s->ops.arg,lin(p),n,false)) return false;
    s->ops.read(s->ops.arg,lin(p),b,n); return true;
}
static bool gwr(struct dfv2_ifs *s,struct dos_file_far p,const void *b,size_t n) {
    if (!n || n>65536u-p.off || !s->ops.check(s->ops.arg,lin(p),n,true)) return false;
    s->ops.write(s->ops.arg,lin(p),b,n); return true;
}
static const struct dfv2_layout *layout(struct dfv2_ifs *s) { return dos_unified_layout(s->unified); }
static struct dos_file_far sda(struct dfv2_ifs *s,unsigned off) { return plus(layout(s)->sda,off); }
static uint64_t owner(struct dfv2_ifs *s,uint16_t psp) {
    return ((uint64_t)s->generation<<32)|((uint64_t)(s->vm&0xffff)<<16)|psp;
}
static bool cancelled(void *a) { struct dfv2_ifs *s=a; return !s->active || (s->ops.stopped && s->ops.stopped(s->ops.arg)); }
static uint64_t now(void *a) { struct dfv2_ifs *s=a; return s->ops.wall ? s->ops.wall(s->ops.arg) : 0; }
static void pause_(void *a) { struct dfv2_ifs *s=a; if (s->ops.pause) s->ops.pause(s->ops.arg); }

void dfv2_ifs_init(struct dfv2_ifs *s,struct dos_unified *u,const struct dos_file_ops *ops,
                   const struct dfv2_ifs_host *host,uint32_t vm,uint32_t generation) {
    memset(s,0,sizeof(*s)); s->unified=u; s->ops=*ops; if (host) s->host=*host;
    s->vm=vm; s->generation=generation; s->nonce=generation*0x9e3779b1u+0x57u;
}
int dfv2_ifs_map(struct dfv2_ifs *s,unsigned guest,struct vfs_table *t,unsigned native,bool readonly) {
    if (s->active || guest>=26 || native>=26 || !t || !t->vfs || !vfs_drive_present(t->vfs,native)) return IFS_BAD_DRIVE;
    s->view[guest]=(struct dfv2_ifs_view){t,(uint8_t)native,readonly,t->vfs->generation[native],0}; return 0;
}
int dfv2_ifs_map_host(struct dfv2_ifs *s,unsigned guest,struct vfs_table *t,unsigned native,const struct dos_host_gate *gate) {
    if (!gate || !gate->check || !gate->retain || !gate->child || !gate->recheck || !gate->release) return -FS_EACCES;
    int e=dfv2_ifs_map(s,guest,t,native,true);       /* host views are never writable here */
    if (!e) s->view[guest].gate=gate;
    return e;
}
unsigned dfv2_ifs_held(const struct dfv2_ifs *s) {
    unsigned n=0;
    for (unsigned i=0;i<DFV2_SFT_MAX;i++) n+=s->file[i].used;
    for (unsigned i=0;i<DFV2_IFS_SEARCHES;i++) n+=s->search[i].used;
    return n;
}

/* ---- request context: 2000 ms total, cancellable, Retry keeps the start - */
static void begin(struct dfv2_ifs *s,uint64_t deadline,uint64_t start) {
    memset(&s->wait,0,sizeof(s->wait)); s->started=start;
    s->request=(struct vfs_request){.arg=s,.deadline=deadline,.now=now,.cancelled=cancelled,.pause=pause_,
        .wait=&s->wait,.cleanup_status=&s->cleanup_status,.task=fs_request_identity(0),.operation="dfv2"};
}
static struct vfs_table *use(struct dfv2_ifs *s,struct vfs_table *t,uint16_t psp) {
    t->request=&s->request; t->owner=owner(s,psp); s->request.owner=t->owner; return t;
}
static int view_live(struct dfv2_ifs *s,unsigned g) {
    struct dfv2_ifs_view *v=&s->view[g];
    return v->generation==v->table->vfs->generation[v->drive] && vfs_drive_present(v->table->vfs,v->drive) ? 0 : -FS_EAGAIN;
}

/* ---- names --------------------------------------------------------- */
/* FN1/FN2 hold FreeDOS truename output ("C:\DIR\NAME.EXT", K2043
 * newstuff.c). Returns the owned guest drive, IFS_CHAIN or an error. */
static int path(struct dfv2_ifs *s,unsigned off,char native[FS_PATH_BYTES],unsigned *guest) {
    uint8_t b[128]; if (!grd(s,sda(s,off),b,sizeof(b))) return IFS_BAD_POINTER;
    size_t n=0; while (n<sizeof(b) && b[n]) n++;
    if (n==sizeof(b) || n<3 || b[1]!=':' || b[0]<'A' || b[0]>'Z' || b[2]!='\\') return IFS_CHAIN;
    unsigned g=(unsigned)(b[0]-'A');
    if (!(s->owned&(1u<<g)) || !s->view[g].table) return IFS_CHAIN;
    if (n+1>FS_PATH_BYTES) return -FS_ENAMETOOLONG;
    native[0]=(char)('A'+s->view[g].drive); memcpy(native+1,b+1,n); native[n]=0;
    *guest=g; return view_live(s,g);
}
static void fcb_name(const char *name,uint8_t out[11]) {
    memset(out,' ',11);
    if (name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2]))) { memcpy(out,name,strlen(name)); return; }
    unsigned i=0,j=0;
    for (;name[i] && name[i]!='.' && j<8;i++) out[j++]=(uint8_t)(name[i]>='a' && name[i]<='z' ? name[i]-32 : name[i]);
    while (name[i] && name[i]!='.') i++;
    if (name[i]=='.') { i++; for (j=8;name[i] && j<11;i++) out[j++]=(uint8_t)(name[i]>='a' && name[i]<='z' ? name[i]-32 : name[i]); }
}
static const char *leaf(const char *p) { const char *l=p; for (;*p;p++) if (*p=='\\' || *p==':') l=p+1; return l; }
static const char *short_name(const struct fat_entry *e) { return e->alias[0] ? e->alias : e->name; }
static bool wildcard(const char *p) { for (;*p;p++) if (*p=='?' || *p=='*') return true; return false; }
/* A missing ancestor is error 3, a missing final component error 2. */
static int path_error(struct vfs_table *t,char *native,int e) {
    if (e!=-FS_ENOENT) return e;
    char *last=0; for (char *p=native+2;*p;p++) if (*p=='\\') last=p;
    if (!last || last==native+2) return e;
    char save=*last; *last=0; struct fat_entry ent; int r=vfs_stat(t,native,&ent); *last=save;
    return r==-FS_ENOENT || r==-FS_ENOTDIR ? IFS_BAD_PARENT : e;
}

/* ---- SFT slots ----------------------------------------------------- */
static int slot(struct dfv2_ifs *s,uint16_t seg,uint16_t off) {
    const struct dfv2_layout *l=layout(s); uint32_t want=((uint32_t)seg<<4)+off;
    for (unsigned i=0;i<l->sft_count;i++) if (lin(l->slot[i])==want) return (int)i;
    return -1;
}
struct sft { uint8_t b[59]; };
static bool sft_read(struct dfv2_ifs *s,unsigned i,struct sft *f) { return grd(s,layout(s)->slot[i],f->b,59); }
static bool sft_write(struct dfv2_ifs *s,unsigned i,const struct sft *f) { return gwr(s,layout(s)->slot[i],f->b,59); }
/* The open file named by ES:DI. IFS_CHAIN when it is not an owned remote
 * SFT; a forged/stale owned SFT is error 6 and never reaches native state. */
static int file_of(struct dfv2_ifs *s,const struct dfv2_call *k,struct sft *f,struct dfv2_ifs_file **x,unsigned *index) {
    int i=slot(s,k->es,k->di); if (i<0) return IFS_CHAIN;
    if (!sft_read(s,(unsigned)i,f)) return IFS_BAD_POINTER;
    uint16_t flags=u16(f->b+5); unsigned g=flags&0x3f;
    if (!(flags&SFT_FSHARED) || (flags&0x80) || g>=26 || !(s->owned&(1u<<g))) return IFS_CHAIN;
    struct dfv2_ifs_file *r=&s->file[i];
    if (!r->used || r->guest!=g || u32(f->b+0x1b)!=r->nonce) { s->stats.forged++; return -FS_EBADF; }
    if (r->generation!=r->table->vfs->generation[s->view[g].drive]) return -FS_EAGAIN;
    use(s,r->table,u16(f->b+0x31)); *x=r; *index=(unsigned)i; return 0;
}
/* sec-k05: the gate of the view an owned record belongs to (NULL: C:). */
static const struct dos_host_gate *gate_of(struct dfv2_ifs *s,unsigned g) { return g<26 ? s->view[g].gate : 0; }
static int gate_denied(struct dfv2_ifs *s,int e) { if (e==-FS_EACCES) s->stats.denied++; return e; }
static void file_unbind(struct dfv2_ifs *s,struct dfv2_ifs_file *r) {
    const struct dos_host_gate *gate=gate_of(s,r->guest);
    if (r->object && gate) gate->release(gate,r->table,r->object);
    r->object=0;
}
static void release_file(struct dfv2_ifs *s,struct dfv2_ifs_file *r) {
    if (!r->used) return;
    (void)vfs_close(r->table,r->handle); file_unbind(s,r); r->used=false; s->stats.closes++;
}

/* ---- operations ---------------------------------------------------- */
static int do_open(struct dfv2_ifs *s,struct dfv2_call *k) {
    unsigned sub=k->ax&255,g; char native[FS_PATH_BYTES];
    int e=path(s,DFV2_SDA_FN1,native,&g); if (e) return e;
    int i=slot(s,k->es,k->di); if (i<0) return IFS_BAD_POINTER;
    struct sft f; if (!sft_read(s,(unsigned)i,&f)) return IFS_BAD_POINTER;
    unsigned mode=u16(f.b+2),flags=0,attr=0,access;
    if (sub==0x16) mode=(mode&~0xffu)|(k->arg&0xff);
    else if (sub==0x17) { flags=VFS_CREATE|VFS_TRUNCATE|((k->arg&0x100) ? VFS_EXCLUSIVE : 0); attr=k->arg&0xff; }
    else {
        uint8_t x[6]; if (!grd(s,sda(s,DFV2_SDA_EXT_ACTION),x,6)) return IFS_BAD_POINTER;
        /* K2043 dosfns.c:545-552 rebuilds 6Ch DL from O_OPEN/O_TRUNC/O_CREAT;
         * same mapping as the f3-49 6C00h path (dos_file.c). */
        unsigned action=u16(x); attr=u16(x+2)&0xff; mode=u16(x+4);
        if (action&0xff00 || (action&15)>2 || (action>>4)>1) return -FS_EINVAL;
        if (action&0x10) flags|=VFS_CREATE;
        if (!(action&15)) flags|=(flags&VFS_CREATE) ? VFS_EXCLUSIVE : VFS_FAIL_EXISTING;
        if ((action&15)==2) flags|=VFS_TRUNCATE;
    }
    if ((mode&3)>2 || ((mode>>4)&7)>4) return IFS_BAD_MODE;
    access=(mode&3)==0 ? VFS_READ : (mode&3)==1 ? VFS_WRITE : VFS_READ|VFS_WRITE;
    if ((access&VFS_WRITE || flags&(VFS_CREATE|VFS_TRUNCATE)) && s->view[g].readonly) return -FS_EACCES;
    if (sub!=0x16 && (attr&~0x27u)) return -FS_EACCES;  /* volume label / directory creation */
    struct dfv2_ifs_file *r=&s->file[i];
    if (r->used) { release_file(s,r); s->stats.reclaimed++; }   /* the guest reused a slot we still held */
    static const enum vfs_share shares[]={VFS_DENY_ALL,VFS_DENY_ALL,VFS_DENY_WRITE,VFS_DENY_READ,VFS_DENY_NONE};
    struct vfs_table *t=use(s,s->view[g].table,u16(f.b+0x31)); unsigned status=0;
    /* sec-k05: a host view authorizes the actual object (including EXEC,
     * overlay and FCB opens, which all arrive here) before any open. */
    const struct dos_host_gate *gate=gate_of(s,g);
    if (gate && (e=gate->check(gate,t,native,POLICY_READ))) return e==-FS_ENOENT ? path_error(t,native,e) : gate_denied(s,e);
    int h=vfs_open_claim(t,native,access|flags,shares[(mode>>4)&7],(uint8_t)attr,(mode>>4)&7,&status);
    if (h<0) return path_error(t,native,h);
    void *object=0;
    if (gate && (e=gate->retain(gate,t,native,POLICY_READ,h,&object))) { (void)vfs_close(t,h); return gate_denied(s,e==-FS_ENOENT ? -FS_EACCES : e); }
    struct fat_entry ent; uint64_t position;
    if ((e=vfs_handle_stat(t,h,&ent,&position))) {
        (void)vfs_close(t,h); if (object) gate->release(gate,t,object); return e;
    }
    *r=(struct dfv2_ifs_file){.used=true,.guest=(uint8_t)g,.handle=h,.nonce=++s->nonce,
        .generation=t->vfs->generation[s->view[g].drive],.table=t,.object=object};
    f.b[4]=ent.attr; w16(f.b+5,SFT_FSHARED|SFT_FCLEAN|g);
    w32(f.b+7,0); w16(f.b+0x0b,0); w16(f.b+0x0d,ent.times.write_time); w16(f.b+0x0f,ent.times.write_date);
    w32(f.b+0x11,ent.size); w32(f.b+0x15,0); w16(f.b+0x19,0); w32(f.b+0x1b,r->nonce); f.b[0x1f]=0;
    fcb_name(short_name(&ent),f.b+0x20);
    if (sub==0x16 || sub==0x2e) w16(f.b+2,(u16(f.b+2)&0xff00)|(mode&0xff));
    if (!sft_write(s,(unsigned)i,&f)) { release_file(s,r); return IFS_BAD_POINTER; }
    if (sub==0x2e) k->cx=(uint16_t)status;
    s->stats.opens++; return 0;
}
/* sec-k05: a date set on a handle (5701h) is a metadata change; a read-only
 * host view (D:/E:) never applies it to the host file (DOS error 5). */
static int apply_times(struct dfv2_ifs *s,struct dfv2_ifs_file *r,const struct sft *f) {
    if (!(u16(f->b+5)&SFT_FDATE)) return 0;
    if (s->view[r->guest].readonly) return -FS_EACCES;
    return vfs_handle_times(r->table,r->handle,u16(f->b+0x0f),u16(f->b+0x0d));
}
static int do_close(struct dfv2_ifs *s,struct dfv2_call *k,bool commit_only) {
    struct sft f; struct dfv2_ifs_file *r=0; unsigned i; int e0=file_of(s,k,&f,&r,&i); if (e0) return e0;
    int e=apply_times(s,r,&f);
    if (commit_only) return e ? e : vfs_commit(r->table,r->handle);
    unsigned count=u16(f.b);
    if (count>1) { w16(f.b,count-1); return sft_write(s,i,&f) ? e : IFS_BAD_POINTER; }
    /* Last reference (RBIL 2F/1106h: the redirector decrements the count;
     * K2043 dosfns.c:700 does not). Close before publishing count 0. */
    int c=vfs_close(r->table,r->handle); file_unbind(s,r); r->used=false; s->stats.closes++;
    w16(f.b,0); if (!sft_write(s,i,&f)) return IFS_BAD_POINTER;
    return e ? e : c;
}
static int do_rw(struct dfv2_ifs *s,struct dfv2_call *k,bool writing) {
    struct sft f; struct dfv2_ifs_file *r=0; unsigned i; int e0=file_of(s,k,&f,&r,&i); if (e0) return e0;
    if (writing && s->view[r->guest].readonly) return -FS_EACCES;
    /* sec-k05: every transfer rechecks the retained subject and the object's
     * current label (logoff, revocation, policy change deny here). */
    const struct dos_host_gate *gate=gate_of(s,r->guest);
    if (gate) { int denied=r->object ? gate->recheck(gate,r->table,r->object,writing ? POLICY_WRITE : POLICY_READ) : -FS_EACCES;
        if (denied) return gate_denied(s,denied); }
    uint8_t d[4]; if (!grd(s,sda(s,DFV2_SDA_DTA),d,4)) return IFS_BAD_POINTER;
    struct dos_file_far data=at(u16(d+2),u16(d)); unsigned want=k->cx; uint32_t pos=u32(f.b+0x15);
    if (want && (want>65536u-data.off || !s->ops.check(s->ops.arg,lin(data),want,!writing))) return IFS_BAD_POINTER;
    uint64_t position; int e=vfs_seek(r->table,r->handle,pos,VFS_SEEK_SET,&position); if (e) return e;
    size_t total=0;
    if (writing && !want) e=vfs_truncate(r->table,r->handle,pos);
    while (!e && total<want) {
        if ((e=vfs_request_check(&s->request))) break;
        size_t n=want-total,done=0; if (n>sizeof(s->io)) n=sizeof(s->io);
        if (writing) { s->ops.read(s->ops.arg,lin(plus(data,(unsigned)total)),s->io,n); e=vfs_write(r->table,r->handle,s->io,n,&done); }
        else { e=vfs_read(r->table,r->handle,s->io,n,&done);
            if (done && !vfs_request_check(&s->request)) s->ops.write(s->ops.arg,lin(plus(data,(unsigned)total)),s->io,done); }
        total+=done; if (done<n) break;
    }
    if (e && !(writing && total)) return e;      /* a partial write reports its count */
    struct fat_entry ent; uint64_t now_pos;
    int st=vfs_handle_stat(r->table,r->handle,&ent,&now_pos);
    w32(f.b+0x15,pos+(uint32_t)total);
    if (!st) w32(f.b+0x11,ent.size);
    if (writing) w16(f.b+5,u16(f.b+5)&~SFT_FCLEAN);
    if (!sft_write(s,i,&f)) return IFS_BAD_POINTER;
    if (writing) { s->stats.writes++; s->stats.written_bytes+=total; } else { s->stats.reads++; s->stats.read_bytes+=total; }
    if (writing && (u16(f.b+2)&O_SYNC) && (e=vfs_commit(r->table,r->handle))) return e;
    k->cx=(uint16_t)total; return 0;
}
static int do_lock(struct dfv2_ifs *s,struct dfv2_call *k) {
    struct sft f; struct dfv2_ifs_file *r=0; unsigned i; int e0=file_of(s,k,&f,&r,&i); if (e0) return e0;
    uint8_t p[8]; if (k->cx!=1 || (k->bx&255)>1 || !grd(s,at(k->ds,k->dx),p,8)) return IFS_BAD_POINTER;
    return vfs_lock_range(r->table,r->handle,u32(p),u32(p+4),(k->bx&255)==1);
}
static int do_seek_end(struct dfv2_ifs *s,struct dfv2_call *k) {
    struct sft f; struct dfv2_ifs_file *r=0; unsigned i; int e0=file_of(s,k,&f,&r,&i); if (e0) return e0;
    struct fat_entry ent; uint64_t pos; int e=vfs_handle_stat(r->table,r->handle,&ent,&pos); if (e) return e;
    uint32_t next=ent.size+(uint32_t)(((uint32_t)k->cx<<16)|k->dx);
    w32(f.b+0x11,ent.size); if (!sft_write(s,i,&f)) return IFS_BAD_POINTER;
    k->dx=(uint16_t)(next>>16); k->ax=(uint16_t)next; return 0;
}
static int do_space(struct dfv2_ifs *s,struct dfv2_call *k) {
    const struct dfv2_layout *l=layout(s); uint32_t base=lin(l->cds),p=((uint32_t)k->es<<4)+k->di;
    if (p<base || (p-base)%l->cds_stride || (p-base)/l->cds_stride>=l->cds_count) return IFS_CHAIN;
    unsigned g=(p-base)/l->cds_stride;
    if (!(s->owned&(1u<<g))) return IFS_CHAIN;
    int e=view_live(s,g); if (e) return e;
    struct vfs_table *t=use(s,s->view[g].table,0); struct fat_volume *vol=t->vfs->volumes[s->view[g].drive];
    uint64_t bytes; uint32_t cluster;
    if ((e=vfs_free_space(t,s->view[g].drive,&bytes,&cluster))) return e;
    uint32_t spc=vol ? vol->spc : cluster/512,total=vol ? vol->clusters : 0,avail=(uint32_t)(bytes/(cluster ? cluster : 512));
    uint8_t media=vol ? vol->media : 0xf8;
    if (!spc) spc=1;
    while ((total>0xffff || avail>0xffff) && spc<=64) { spc*=2; total/=2; avail/=2; }  /* 16-bit fields */
    if (total>0xffff) total=0xffff;
    if (avail>0xffff) avail=0xffff;
    /* AX is sectors per cluster only. FreeDOS K2043 DosGetExtFree() (7303h on a
     * network drive) copies the raw AX into xfs_clussize without clearing a
     * media byte in AH, so 0xF8 in AH made every cluster 0xF8xx times larger
     * ("801,637 Mega bytes free" in DIR). The 36h path masks AH; we need not
     * carry the media id here (redirector convention: AX = spc). */
    (void)media;
    k->ax=(uint16_t)(spc&255); k->bx=(uint16_t)total; k->cx=512; k->dx=(uint16_t)avail; return 0;
}
static int do_attr(struct dfv2_ifs *s,struct dfv2_call *k,bool set) {
    char native[FS_PATH_BYTES]; unsigned g; int e=path(s,DFV2_SDA_FN1,native,&g); if (e) return e;
    struct vfs_table *t=use(s,s->view[g].table,0);
    if (set) {
        if (s->view[g].readonly) return -FS_EACCES;
        if (k->arg&~0x27u) return -FS_EACCES;
        e=vfs_attrib(t,native,(uint8_t)k->arg); return path_error(t,native,e);
    }
    const struct dos_host_gate *gate=gate_of(s,g);
    if (gate && (e=gate->check(gate,t,native,POLICY_READ))) return e==-FS_ENOENT ? path_error(t,native,e) : gate_denied(s,e);
    struct fat_entry ent; e=vfs_stat(t,native,&ent); if (e) return path_error(t,native,e);
    k->ax=ent.attr; k->bx=(uint16_t)(ent.size>>16); k->di=(uint16_t)ent.size;
    k->cx=ent.times.write_time; k->dx=ent.times.write_date; return 0;
}
static bool cds_current(struct dfv2_ifs *s,unsigned g,const char *guest_path) {
    const struct dfv2_layout *l=layout(s); uint8_t c[67];
    for (unsigned i=0;i<l->cds_count && i<26;i++) {
        if (!grd(s,plus(l->cds,i*l->cds_stride),c,sizeof(c))) continue;
        if (c[0]=='A'+g && !strncmp((const char *)c,guest_path,sizeof(c))) return true;
    }
    return false;
}
static int do_dir(struct dfv2_ifs *s,struct dfv2_call *k) {
    unsigned sub=k->ax&255,g; char native[FS_PATH_BYTES]; int e=path(s,DFV2_SDA_FN1,native,&g); if (e) return e;
    struct vfs_table *t=use(s,s->view[g].table,0);
    if (sub==0x05) {
        if (!native[3]) return 0;
        const struct dos_host_gate *gate=gate_of(s,g);
        if (gate && (e=gate->check(gate,t,native,POLICY_READ)))
            return e==-FS_ENOENT || e==-FS_ENOTDIR ? IFS_BAD_PARENT : gate_denied(s,e);
        struct fat_entry ent; e=vfs_stat(t,native,&ent);
        if (e) return e==-FS_ENOENT || e==-FS_ENOTDIR ? IFS_BAD_PARENT : e;
        return (ent.attr&0x10) ? 0 : IFS_BAD_PARENT;
    }
    if (s->view[g].readonly) return -FS_EACCES;
    if (sub==0x03) { e=vfs_mkdir(t,native,0); return e==-FS_EEXIST ? -FS_EACCES : path_error(t,native,e); }
    char guest[FS_PATH_BYTES]; memcpy(guest,native,FS_PATH_BYTES); guest[0]=(char)('A'+g);
    if (cds_current(s,g,guest)) return IFS_CURRENT_DIRECTORY;
    e=vfs_rmdir(t,native);
    return e==-FS_ENOENT || e==-FS_ENOTDIR ? IFS_BAD_PARENT : e;
}
static int do_rename(struct dfv2_ifs *s,struct dfv2_call *k) {
    (void)k; char from[FS_PATH_BYTES],to[FS_PATH_BYTES]; unsigned g,h;
    int e=path(s,DFV2_SDA_FN1,from,&g); if (e) return e;
    e=path(s,DFV2_SDA_FN2,to,&h); if (e==IFS_CHAIN || (!e && h!=g)) return IFS_DOS(17);
    if (e) return e;
    if (s->view[g].readonly) return -FS_EACCES;
    struct vfs_table *t=use(s,s->view[g].table,0);
    e=vfs_rename(t,from,to); return path_error(t,from,e);
}
static int do_delete(struct dfv2_ifs *s,struct dfv2_call *k) {
    (void)k; char native[FS_PATH_BYTES]; unsigned g; int e=path(s,DFV2_SDA_FN1,native,&g); if (e) return e;
    if (s->view[g].readonly) return -FS_EACCES;
    struct vfs_table *t=use(s,s->view[g].table,0);
    if (!wildcard(leaf(native))) { e=vfs_delete(t,native); return path_error(t,native,e); }
    /* Wildcards (RBIL 2F/1113h): delete every normal match, first error stops. */
    unsigned deleted=0;
    for (unsigned guard=0;guard<4096;guard++) {
        struct vfs_find find; struct fat_entry ent;
        e=vfs_find_first(t,native,VFS_FIND_DOS,&find,&ent);
        if (e) break;
        char victim[FS_PATH_BYTES]; size_t dir=(size_t)(leaf(native)-native),n=strlen(short_name(&ent));
        if (dir+n+1>FS_PATH_BYTES) return -FS_ENAMETOOLONG;
        memcpy(victim,native,dir); memcpy(victim+dir,short_name(&ent),n+1);
        if ((e=vfs_delete(t,victim))) return e;
        deleted++;
    }
    return deleted ? 0 : e==-FS_ENOENT ? path_error(t,native,e) : e;
}

/* ---- search: 21-byte SDB (dm_drive|80h, template, attribute, 8 bytes ours)
 * followed by the 32-byte entry FreeDOS's pop_dmp copies (dosfns.c:977). */
static uint16_t search_check(const struct dfv2_ifs_search *c,uint32_t cursor) { return (uint16_t)((c->nonce^(cursor*0x9e3779b1u))>>8); }
static int put_entry(struct dfv2_ifs *s,struct dos_file_far dta,unsigned index,const struct dfv2_ifs_search *c,
                     uint32_t cursor,const struct fat_entry *ent) {
    uint8_t b[53]; memset(b,0,sizeof(b));
    b[0]=(uint8_t)(0x80|c->guest); fcb_name(c->find.pattern,b+1); b[12]=c->find.attr_mask&0x3f;
    b[13]=(uint8_t)index; b[14]=(uint8_t)(c->nonce>>24); w32(b+15,cursor); w16(b+19,search_check(c,cursor));
    fcb_name(short_name(ent),b+21); b[32]=ent->attr; w16(b+43,ent->times.write_time); w16(b+45,ent->times.write_date);
    w16(b+47,(uint16_t)ent->first); w32(b+49,ent->size);
    return gwr(s,dta,b,sizeof(b)) ? 0 : IFS_BAD_POINTER;
}
/* sec-k05: a gated view lists only entries the retained subject may READ
 * (credential-filtered D:/E:, contract 8.4): stable 8.3 aliases from the
 * real resolver, never a hidden/private name. ENOENT ends the search. */
static int find_filtered(struct dfv2_ifs *s,const struct dos_host_gate *gate,struct vfs_table *t,
                         void *directory,struct vfs_find *find,struct fat_entry *ent,int e) {
    for (unsigned guard=0;gate && !e;guard++) {
        if (guard>=65536u) return -FS_ELOOP;
        int visible=directory ? gate->child(gate,t,directory,short_name(ent),POLICY_READ) : -FS_EACCES;
        if (!visible) return 0;
        if (visible!=-FS_EACCES && visible!=-FS_ENOENT && visible!=-FS_ENOTDIR && visible!=-FS_EILSEQ &&
            visible!=-FS_EINVAL && visible!=-FS_ENAMETOOLONG) return visible;
        s->stats.hidden++;
        e=vfs_find_next(t,find,ent);
    }
    return e;
}
static void search_drop(struct dfv2_ifs *s,struct dfv2_ifs_search *c) {
    const struct dos_host_gate *gate=gate_of(s,c->guest);
    if (c->used && c->directory && gate) gate->release(gate,c->table,c->directory);
    c->directory=0; c->used=false;
}
static int do_find(struct dfv2_ifs *s,bool first) {
    uint8_t d[4]; if (!grd(s,sda(s,DFV2_SDA_DTA),d,4)) return IFS_BAD_POINTER;
    struct dos_file_far dta=at(u16(d+2),u16(d)); struct fat_entry ent; int e;
    for (unsigned i=0;i<DFV2_IFS_SEARCHES;i++) {
        struct dfv2_ifs_search *c=&s->search[i];
        if (c->used && c->find.generation!=c->table->vfs->generation[c->find.drive]) search_drop(s,c);
    }
    uint8_t psp_b[2]; if (!grd(s,sda(s,DFV2_SDA_PSP),psp_b,2)) return IFS_BAD_POINTER;
    uint16_t psp=u16(psp_b);
    if (first) {
        char native[FS_PATH_BYTES]; unsigned g; if ((e=path(s,DFV2_SDA_FN1,native,&g))) return e;
        uint8_t attr; if (!grd(s,sda(s,DFV2_SDA_ATTR),&attr,1)) return IFS_BAD_POINTER;
        struct vfs_table *t=use(s,s->view[g].table,psp); struct vfs_find find;
        const struct dos_host_gate *gate=gate_of(s,g);
        void *directory=0;
        if (gate) {
            /* Enumeration needs READ on the real directory (each ancestor
             * traversed); a private directory is not listed at all.
             * "X:\\DIR\\pattern" -> "X:\\DIR"; the root is "X:\\". */
            char *dir=(char *)s->io; size_t cut=(size_t)(leaf(native)-native);
            if (cut<3) return IFS_BAD_POINTER;
            if (cut>3) cut--;
            memcpy(dir,native,cut); dir[cut]=0;
            if ((e=gate->retain(gate,t,dir,POLICY_READ,-1,&directory)))
                return e==-FS_ENOENT || e==-FS_ENOTDIR ? IFS_BAD_PARENT : gate_denied(s,e);
        }
        e=vfs_find_first(t,native,(uint8_t)((attr&0x3f)|VFS_FIND_DOS),&find,&ent);
        if (gate) e=find_filtered(s,gate,t,directory,&find,&ent,e);
        if (e) {
            if (directory) gate->release(gate,t,directory);
            if (e!=-FS_ENOENT) return e; e=path_error(t,native,e); return e==-FS_ENOENT ? IFS_DOS(18) : e;
        }
        struct dfv2_ifs_search *c=0; unsigned index=0;
        for (unsigned i=0;i<DFV2_IFS_SEARCHES;i++) {
            struct dfv2_ifs_search *o=&s->search[i];
            if (o->used && o->psp==psp && o->table==t && o->guest==g && o->find.directory==find.directory &&
                o->find.attr_mask==find.attr_mask && !strncmp(o->find.pattern,find.pattern,sizeof(find.pattern))) { c=o; index=i; break; }
            if (!o->used && !c) { c=o; index=i; }
        }
        if (!c) { c=&s->search[0]; index=0;     /* bounded: retire the least recently used */
            for (unsigned i=1;i<DFV2_IFS_SEARCHES;i++) if (s->search[i].stamp<c->stamp) { c=&s->search[i]; index=i; } search_drop(s,c); }
        if (!c->used) {
            *c=(struct dfv2_ifs_search){.used=true,.guest=(uint8_t)g,.psp=psp,.nonce=++s->nonce*0x2545f491u,.table=t,.find=find,
                .directory=directory};
            directory=0;
        }
        else if (find.cursor>c->find.cursor) c->find.cursor=find.cursor;
        if (directory) gate->release(gate,t,directory);   /* an equal live search already holds one */
        c->stamp=++s->stamp; s->stats.finds++;
        return put_entry(s,dta,index,c,find.cursor,&ent);
    }
    uint8_t b[21]; if (!grd(s,dta,b,sizeof(b))) return IFS_BAD_POINTER;
    unsigned index=b[13]; uint32_t cursor=u32(b+15);
    if (!(b[0]&0x80) || index>=DFV2_IFS_SEARCHES) return IFS_CHAIN;
    struct dfv2_ifs_search *c=&s->search[index];
    if (!c->used || (uint8_t)(c->nonce>>24)!=b[14] || u16(b+19)!=search_check(c,cursor) || cursor>c->find.cursor ||
        (b[0]&0x7f)!=c->guest) return IFS_DOS(18);
    if (!(s->owned&(1u<<c->guest))) return IFS_CHAIN;
    struct vfs_find find=c->find; find.cursor=cursor;
    use(s,c->table,psp); e=vfs_find_next(c->table,&find,&ent);
    const struct dos_host_gate *gate=gate_of(s,c->guest);
    if (gate) e=find_filtered(s,gate,c->table,c->directory,&find,&ent,e);
    if (e) { if (e!=-FS_ENOENT) search_drop(s,c); return e==-FS_ENOENT ? IFS_DOS(18) : e; }
    if (find.cursor>c->find.cursor) c->find.cursor=find.cursor;
    c->stamp=++s->stamp; s->stats.finds++;
    return put_entry(s,dta,index,c,find.cursor,&ent);
}
static void process_end(struct dfv2_ifs *s,uint16_t psp) {
    for (unsigned i=0;i<DFV2_IFS_SEARCHES;i++) if (s->search[i].used && s->search[i].psp==psp) search_drop(s,&s->search[i]);
}
static void flush_all(struct dfv2_ifs *s) {
    for (unsigned g=0;g<26;g++) if (s->owned&(1u<<g) && !s->view[g].readonly && !view_live(s,g)) {
        bool duplicate=false;
        for (unsigned h=0;h<g;h++) if (s->owned&(1u<<h) && s->view[h].table==s->view[g].table && s->view[h].drive==s->view[g].drive) duplicate=true;
        if (!duplicate) (void)vfs_flush_drive(use(s,s->view[g].table,0),s->view[g].drive);
    }
}

/* ---- errors and the critical result ---------------------------------- */
/* f3-61c: name of a failed request for its evidence record (8.3 leaf, no
 * drive or directory; at most 12 characters). */
static void failure_name(struct dfv2_ifs *s,const struct dfv2_call *k,unsigned sub,char out[13]) {
    out[0]=0;
    if (sub==0x06 || sub==0x07 || sub==0x08 || sub==0x09 || sub==0x0a || sub==0x21) {
        int i=slot(s,k->es,k->di); struct sft f;
        if (i<0 || !sft_read(s,(unsigned)i,&f)) return;
        unsigned n=0;
        for (unsigned c=0;c<8 && f.b[0x20+c]!=' ';c++) out[n++]=(char)f.b[0x20+c];
        if (f.b[0x28]!=' ') { out[n++]='.'; for (unsigned c=8;c<11 && f.b[0x20+c]!=' ';c++) out[n++]=(char)f.b[0x20+c]; }
        out[n]=0; return;
    }
    if (sub==0x1c || sub==0x0c) return;   /* find-next, free space: no name */
    uint8_t b[128]; if (!grd(s,sda(s,DFV2_SDA_FN1),b,sizeof(b))) return;
    size_t len=0,start=0; while (len<sizeof(b) && b[len]) { if (b[len]=='\\' || b[len]==':') start=len+1; len++; }
    if (len==sizeof(b)) return;
    unsigned n=0; for (size_t c=start;c<len && n<12;c++) out[n++]=(char)b[c];
    out[n]=0;
}
/* *cause: the extended error (59h class of the failure) for the INT 24h DI. */
static void dos_error(struct dfv2_call *k,int e,bool writing,unsigned *cause) {
    unsigned c;
    if (e<=-1000) c=*cause=(unsigned)(-e-1000);
    else { struct dos_file_error x; dos_file_error_from_fs(&x,e,writing); c=*cause=x.code;
        if (e==-VFS_ESHARE || e==-VFS_ECRITICAL_SHARE) c=5; }  /* as f3-49 (dos_file.c): caller sees 05h */
    k->ax=(uint16_t)c; k->flags|=1;
}
static bool critical_error(int e,unsigned sub) {
    return e==-VFS_ECRITICAL_SHARE || e==-FS_EIO || e==-FS_EUCLEAN || e==-FS_EQUARANTINED || e==-FS_EAGAIN ||
           e==-FS_EROFS || (e==-VFS_ELOCK && (sub==0x08 || sub==0x09));
}
static bool same_request(const struct dfv2_call *a,const struct dfv2_call *b) {
    return a->ax==b->ax && a->bx==b->bx && a->cx==b->cx && a->dx==b->dx && a->si==b->si && a->di==b->di &&
           a->ds==b->ds && a->es==b->es && a->frame_ss==b->frame_ss && a->frame_sp==b->frame_sp && a->arg==b->arg;
}
static int dispatch(struct dfv2_ifs *s,struct dfv2_call *k,unsigned sub) {
    switch (sub) {
    case 0x01: case 0x03: case 0x05: return do_dir(s,k);
    case 0x06: return do_close(s,k,false);
    case 0x07: return do_close(s,k,true);
    case 0x08: return do_rw(s,k,false);
    case 0x09: return do_rw(s,k,true);
    case 0x0a: return do_lock(s,k);
    case 0x0c: return do_space(s,k);
    case 0x0e: return do_attr(s,k,true);
    case 0x0f: return do_attr(s,k,false);
    case 0x11: return do_rename(s,k);
    case 0x13: return do_delete(s,k);
    case 0x16: case 0x17: case 0x2e: return do_open(s,k);
    case 0x1b: return do_find(s,true);
    case 0x1c: return do_find(s,false);
    case 0x21: return do_seek_end(s,k);
    case 0x20: flush_all(s); return IFS_CHAIN;            /* other redirectors still flush */
    case 0x22: process_end(s,k->ds); return IFS_CHAIN;    /* DS = terminating PSP */
    default: return IFS_CHAIN;   /* 1100h, 111Dh, 111E/1Fh, 1123h, 1125h: unowned/upstream */
    }
}
static enum dos_file_result request(void *arg,unsigned role,struct dfv2_call *k,struct dfv2_critical *c) {
    struct dfv2_ifs *s=arg;
    if (role!=DFV2_ROLE_LOCAL || !s->active || (k->ax>>8)!=0x11) return DOS_FILE_CHAIN;
    s->stats.requests++;
    unsigned sub=k->ax&255; uint64_t t=now(s),start=t,deadline=t+DOS_FILE_BUDGET_US;
    struct dfv2_call original=*k;
    if (k->retry) {
        if (!s->retry.armed || !same_request(&s->retry.call,k)) { s->retry.armed=false; k->ax=0x1f; k->flags|=1; return DOS_FILE_DONE; }
        s->stats.retries++; deadline=s->retry.deadline; start=deadline-DOS_FILE_BUDGET_US;
        if (t>=deadline) { s->retry.armed=false; s->stats.expired++; k->ax=DFV2_FAIL_ON_INT24; k->flags|=1; return DOS_FILE_DONE; }
    }
    s->retry.armed=false;
    begin(s,deadline,start);
    struct fs_request_scope scope; fs_request_enter(&scope,&s->request);
    int e=dispatch(s,k,sub);
    fs_request_leave(&scope);
    if (e==IFS_CHAIN) { *k=original; s->stats.chained++; return DOS_FILE_CHAIN; }
    s->stats.owned++;
    if (!e) { k->flags&=~1u; return DOS_FILE_DONE; }
    bool writing=sub==0x09 || sub==0x0e || sub==0x13 || sub==0x11 || sub==0x03 || sub==0x01 || sub==0x17;
    unsigned code; *k=original; dos_error(k,e,writing,&code);
    if (s->stats.failed<DFV2_IFS_FAILURES) {
        struct dfv2_ifs_failure *x=&s->failure[s->stats.failed];
        x->fn=(uint16_t)original.ax; x->error=k->ax; failure_name(s,&original,sub,x->name);
    }
    s->stats.failed++;
    /* DOS critical-error producers (f3-49 DF5 set): genuine INT 24h through
     * the kernel unless the open mode asked for none (O_NOCRIT, 6Ch bit 13). */
    bool nocrit=false;
    if (sub==0x08 || sub==0x09 || sub==0x0a || sub==0x06 || sub==0x07 || sub==0x21) {
        int i=slot(s,k->es,k->di); struct sft f;
        if (i>=0 && sft_read(s,(unsigned)i,&f)) nocrit=(u16(f.b+2)&O_NOCRIT)!=0;
    } else if (sub==0x2e) {
        uint8_t m[2]; if (grd(s,sda(s,DFV2_SDA_EXT_MODE),m,2)) nocrit=(u16(m)&O_NOCRIT)!=0;
    }
    if (critical_error(e,sub) && !nocrit && !vfs_request_check(&s->request)) {
        unsigned g=2; char native[FS_PATH_BYTES]; struct sft f; int i=slot(s,k->es,k->di);
        if (i>=0 && sft_read(s,(unsigned)i,&f) && (u16(f.b+5)&SFT_FSHARED)) g=u16(f.b+5)&0x3f;
        else { unsigned pg=2; int pe=path(s,DFV2_SDA_FN1,native,&pg); if (pe!=IFS_CHAIN && pe!=IFS_BAD_POINTER) g=pg; }
        uint8_t flags=(uint8_t)(0x18|(writing ? 1 : 0));
        c->ax=(uint16_t)(flags<<8|(g&0x1f)); c->di=(uint16_t)(code>=19 && code<=36 ? code-19 : 12);
        c->bp=layout(s)->driver_seg; c->si=0; c->raise=true;
        s->retry.armed=true; s->retry.call=original; s->retry.call.retry=1; s->retry.deadline=deadline;
        s->stats.critical++;
    }
    return DOS_FILE_DONE;
}
static enum dos_file_result complete(void *arg,unsigned reply,struct dfv2_call *k) {
    (void)arg; (void)reply; k->ax=0x1f; k->flags|=1; return DOS_FILE_DONE;   /* no monitor PENDING producer */
}

/* ---- activation barrier: event 0 phase 1 (after init_kernel, before
 * DoInstall). prepare checks everything and mounts; commit only publishes. */
static int prepare(void *arg,unsigned event,uint16_t psp,uint16_t parent) {
    struct dfv2_ifs *s=arg; (void)psp; (void)parent;
    if (event!=DFV2_EVENT_BIND || !s->unified->staged) return 0;   /* phase 0 and lifecycle: nothing native */
    const struct dfv2_layout *l=s->unified->staged;
    if (s->active || s->mounted) { s->why="activation repeated"; s->stats.refusals++; return 5; }
    if (l->sda_bytes<DFV2_SDA_EXT_MODE+2) { s->why="SDA too short for the redirector fields"; s->stats.refusals++; return 87; }
    memset(s->view,0,sizeof(s->view));
    int e=s->host.mount ? s->host.mount(s->host.arg,s) : -FS_EIO;
    if (e) { s->why="native mount failed"; s->stats.refusals++; memset(s->view,0,sizeof(s->view)); return e<0 ? 31 : e; }
    s->mounted=true;
    uint32_t owned=0; const char *why=0;
    for (unsigned g=0;g<26 && !why;g++) {
        if (!s->view[g].table) continue;
        if (g>=l->cds_count) { why="owned drive beyond LASTDRIVE"; break; }
        uint8_t c[88]; struct dos_file_far p=plus(l->cds,g*l->cds_stride);
        if (!s->ops.check(s->ops.arg,lin(p),88,true)) { why="CDS not writable"; break; }
        s->ops.read(s->ops.arg,lin(p),c,88);
        uint16_t flags=u16(c+0x43);
        if (flags&0xb000) why="owned drive is already network/JOIN/SUBST";
        else if (g==2 && !(flags&0x4000)) why="C: is not a local drive";
        owned|=1u<<g;
    }
    if (!why && !(owned&(1u<<2))) why="no C: view";
    /* Drain: no bootstrap regular file may still be open on an owned drive
     * (CONFIG.SYS is closed; devices are not files). */
    for (unsigned i=0;i<l->sft_count && !why;i++) {
        uint8_t f[59];
        if (!s->ops.check(s->ops.arg,lin(l->slot[i]),59,false)) { why="SFT not readable"; break; }
        s->ops.read(s->ops.arg,lin(l->slot[i]),f,59);
        uint16_t flags=u16(f+5);
        if (u16(f) && !(flags&0x80) && (owned&(1u<<(flags&0x3f)))) why="bootstrap regular file still open on an owned drive";
    }
    if (why) {
        s->why=why; s->stats.refusals++;
        if (s->host.unmount) s->host.unmount(s->host.arg,s);
        s->mounted=false; memset(s->view,0,sizeof(s->view)); return 5;
    }
    s->owned=owned; return 0;
}
static void commit(void *arg,unsigned event,uint16_t psp) {
    struct dfv2_ifs *s=arg; (void)psp;
    if (event!=DFV2_EVENT_BIND || !s->mounted || s->active) return;
    const struct dfv2_layout *l=&s->unified->layout;
    /* Publication: every owned CDS becomes an IFS drive (C000h) in one step,
     * before the kernel publishes the hooks; the extents were pinned. */
    for (unsigned g=0;g<26;g++) if (s->owned&(1u<<g)) {
        uint8_t c[88]; struct dos_file_far p=plus(l->cds,g*l->cds_stride);
        s->ops.read(s->ops.arg,lin(p),c,88);
        if (!(u16(c+0x43)&0x4000)) {           /* a new drive letter: "X:\" root */
            memset(c,0,88); c[0]=(uint8_t)('A'+g); c[1]=':'; c[2]='\\'; w16(c+0x4f,2);
        }
        w16(c+0x43,DFV2_CDS_IFS);
        s->ops.write(s->ops.arg,lin(p),c,88);
    }
    s->active=true; s->stats.activations++;
}
static void destroy(void *arg) {
    struct dfv2_ifs *s=arg;
    s->active=false; s->retry.armed=false;
    for (unsigned i=0;i<DFV2_SFT_MAX;i++) release_file(s,&s->file[i]);
    for (unsigned i=0;i<DFV2_IFS_SEARCHES;i++) search_drop(s,&s->search[i]);
    memset(s->search,0,sizeof(s->search));
    if (s->mounted && s->host.unmount) s->host.unmount(s->host.arg,s);
    s->mounted=false; s->owned=0; memset(s->view,0,sizeof(s->view));
}
void dfv2_ifs_backend(struct dfv2_ifs *s,struct dfv2_backend *b) {
    *b=(struct dfv2_backend){prepare,commit,request,complete,destroy,0,s};
}
