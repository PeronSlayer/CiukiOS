/* Private DFV2 binding for the unified CiukiDOS guest (f3-56). Separate from
 * the v1 compact mirror: nothing here writes FreeDOS's SDA, SFT chain, CDS,
 * PSP or JFT. The only guest writes are the registered call record and the
 * claim bitmap of the final bind record. Errors are DOS error codes.
 * See docs/validation/f3-56-unified-binding.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dos_unified.h"

enum { E_HANDLE=6, E_MEMORY=8, E_PARAM=87, E_GENERAL=0x1f };
static const uint16_t sda_offsets[DFV2_SDA_FIELDS]={0x00,0x01,0x03,0x04,0x06,0x07,0x0c,0x10,0x14,0x16};
static const uint16_t gates[3]={DFV2_DIRECT,DFV2_LOCAL,DFV2_EVENT};
static const char driver_id[17]="CIUKIDF2 F3-56 1";
static const char kernel_id[17]="CIUKIDOS K2043 2";

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return u16(p)|(uint32_t)u16(p+2)<<16; }
static void w16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p,uint32_t v) { w16(p,(uint16_t)v); w16(p+2,(uint16_t)(v>>16)); }
static struct dos_file_far fp(const uint8_t *p) { return (struct dos_file_far){u16(p),u16(p+2)}; }
static uint32_t lin(struct dos_file_far p) { return ((uint32_t)p.seg<<4)+p.off; }
static struct dos_file_far at(uint16_t seg,uint16_t off) { return (struct dos_file_far){off,seg}; }
static struct dos_file_far plus(struct dos_file_far p,unsigned n) { return at(p.seg,(uint16_t)(p.off+n)); }
static bool same(struct dos_file_far a,struct dos_file_far b) { return lin(a)==lin(b); }

/* Conventional memory only (no A20 alias, VGA or ROM), no offset wrap. */
static bool conv(struct dos_file_far p,uint32_t n) {
    uint32_t a=lin(p); return n && a>=0x500 && a<0xa0000 && n<=0xa0000-a && n<=65536u-p.off;
}
static bool rd(struct dos_unified *d,struct dos_file_far p,void *b,uint32_t n) {
    if (!conv(p,n) || !d->memory.check(d->memory.arg,lin(p),n,false)) return false;
    d->memory.read(d->memory.arg,lin(p),b,n); return true;
}
struct span { uint32_t base,bytes; };
static struct span sp(struct dos_file_far p,uint32_t n) { return (struct span){lin(p),n}; }
static bool disjoint(struct span a,struct span b) { return a.base+a.bytes<=b.base || b.base+b.bytes<=a.base; }
static bool inside(struct span outer,struct span s) { return s.base>=outer.base && s.bytes<=outer.bytes && s.base-outer.base<=outer.bytes-s.bytes; }

void dos_unified_rom(uint8_t *page,uint32_t generation) {
    static const uint8_t roles[4]={0xd2,0xa2,0xe2,0xc2};
    for (unsigned i=0;i<4;i++) {
        uint8_t *g=page+(DFV2_DIRECT-0x1000u)+4*i;
        g[0]=0xcd; g[1]=0xfe; g[2]=i==3 ? 0xf4 : 0xcb; g[3]=roles[i];
    }
    uint8_t *b=page+(DFV2_DESCRIPTOR-0x1000u); memset(b,0,DFV2_DESCRIPTOR_BYTES);
    memcpy(b,"DFV2",4); w16(b+4,DFV2_VERSION); w16(b+6,DFV2_DESCRIPTOR_BYTES); w32(b+8,DFV2_FEATURES);
    for (unsigned i=0;i<4;i++) { w16(b+12+4*i,(uint16_t)(DFV2_DIRECT+4*i)); w16(b+14+4*i,DFV2_ROM_SEG); }
    w32(b+0x1c,generation); w16(b+0x20,DFV2_CALL_BYTES); w16(b+0x22,DFV2_BIND_BYTES);
    w16(b+0x24,DFV2_DIRECT); w16(b+0x26,DFV2_END);
}
bool dos_unified_discover(struct dos_file_frame *f) {
    if (f->ax!=0xc148 || f->bx!=0x4644 || f->cx!=DFV2_VERSION || f->dx!=0x564d) return false;
    f->ax=0; f->cx=DFV2_VERSION; f->es=DFV2_ROM_SEG; f->di=DFV2_DESCRIPTOR; return true;
}
void dos_unified_init(struct dos_unified *d,uint32_t generation,const struct dos_file_ops *memory,const struct dfv2_backend *backend) {
    memset(d,0,sizeof(*d)); d->generation=generation; d->memory=*memory;
    if (backend) d->backend=*backend;
}

static int prepare(struct dos_unified *d,unsigned event,uint16_t psp,uint16_t parent) {
    return d->backend.prepare ? d->backend.prepare(d->backend.arg,event,psp,parent) : 0;
}
static void commit(struct dos_unified *d,unsigned event,uint16_t psp) {
    if (d->backend.commit) d->backend.commit(d->backend.arg,event,psp);
}

static struct span driver_span(const struct dfv2_layout *l) { return sp(at(l->driver_seg,0),l->driver_bytes); }
static uint16_t stack_top(const struct dfv2_layout *l) { return (uint16_t)(l->stack_off+l->stack_bytes); }

#define REQUIRE(cond,reason,error) do { if (!(cond)) { d->why=(reason); return (error); } } while (0)

/* Bytes 10h-2Fh: the driver registration, identical in both phases. */
static const char *driver_fields(const uint8_t *r,struct dfv2_layout *l) {
    l->driver_seg=u16(r+0x10); l->driver_bytes=u16(r+0x12); l->call_off=u16(r+0x14);
    l->stack_off=u16(r+0x16); l->stack_bytes=u16(r+0x18);
    for (unsigned i=0;i<3;i++) l->site[i]=u16(r+0x1a+2*i);
    memcpy(l->driver_id,r+0x20,16);
    uint32_t top=(uint32_t)l->stack_off+l->stack_bytes;
    if (memcmp(l->driver_id,driver_id,16)) return "driver identity";
    if (!conv(at(l->driver_seg,0),l->driver_bytes)) return "driver extent";
    if (l->call_off>l->driver_bytes || DFV2_CALL_BYTES>l->driver_bytes-l->call_off || (l->call_off&1)) return "call record extent";
    if (l->stack_bytes<256 || top>l->driver_bytes || (top&1)) return "driver stack extent";
    if (!disjoint(sp(at(l->driver_seg,l->call_off),DFV2_CALL_BYTES),sp(at(l->driver_seg,l->stack_off),l->stack_bytes)))
        return "call record overlaps stack";
    for (unsigned i=0;i<3;i++) if (l->site[i]>=l->driver_bytes || l->site[i]==l->site[(i+1)%3]) return "return sites";
    return 0;
}
static bool parse_call(const uint8_t *c,struct dfv2_call *k) {
    *k=(struct dfv2_call){.generation=u32(c+4),.serial=u32(c+8),.role=u16(c+0x0c),.status=u16(c+0x0e),
        .flags=u16(c+0x10),.ax=u16(c+0x12),.bx=u16(c+0x14),.cx=u16(c+0x16),.dx=u16(c+0x18),.si=u16(c+0x1a),
        .di=u16(c+0x1c),.bp=u16(c+0x1e),.ds=u16(c+0x20),.es=u16(c+0x22),.frame_ss=u16(c+0x24),
        .frame_sp=u16(c+0x26),.arg=u16(c+0x28)};
    for (unsigned i=0x2a;i<DFV2_CALL_BYTES;i++) if (c[i]) return false;
    return u16(c)==DFV2_VERSION && u16(c+2)==DFV2_CALL_BYTES && k->status==0xffff;
}
/* The only guest write besides the claim output: status and, for DONE,
 * the result registers and FLAGS. The extent was pinned writable. */
static void finish(struct dos_unified *d,const struct dfv2_call *k,unsigned status) {
    uint8_t c[0x24-0x0e];
    w16(c,(uint16_t)status); w16(c+2,k->flags);
    const uint16_t r[9]={k->ax,k->bx,k->cx,k->dx,k->si,k->di,k->bp,k->ds,k->es};
    for (unsigned i=0;i<9;i++) w16(c+4+2*i,r[i]);
    struct dos_file_far call=at(d->layout.driver_seg,(uint16_t)(d->layout.call_off+0x0e));
    d->memory.write(d->memory.arg,lin(call),c,status==DFV2_STATUS_DONE ? sizeof(c) : 2);
}
static void done_error(struct dfv2_call *k,unsigned error) {
    k->flags=(uint16_t)(error ? k->flags|1u : k->flags&~1u); k->ax=(uint16_t)error;
}

/* ---- PSPs and processes ------------------------------------------- */
struct jft { uint16_t count; struct dos_file_far table; uint8_t h[255]; };
static bool read_psp(struct dos_unified *d,const struct dfv2_layout *l,uint16_t psp,struct jft *j) {
    uint8_t b[0x38];
    if (!psp || !rd(d,at(psp,0),b,sizeof(b)) || u16(b)!=0x20cd) return false;
    j->count=u16(b+0x32); j->table=fp(b+0x34);
    if (!j->count || j->count>DFV2_SFT_MAX || !rd(d,j->table,j->h,j->count)) return false;
    for (unsigned i=0;i<j->count;i++) if (j->h[i]!=0xff && j->h[i]>=l->sft_count) return false;
    return true;
}
/* The live selected PSP (SDA+10h): a TSR may also switch it directly. */
static bool current(struct dos_unified *d,uint16_t *psp) {
    uint8_t b[2]; if (!rd(d,d->layout.field[DFV2_PSP],b,2)) return false;
    *psp=u16(b); return true;
}
/* CloneHandle() mirror (dosfns.c:632-645, task.c:249-265): an entry is
 * inherited iff it names an open SFT (count != 0) without O_NOINHERIT. */
static int inherited(struct dos_unified *d,uint16_t child,uint16_t parent) {
    struct jft c,p; uint8_t slot[4];
    REQUIRE(read_psp(d,&d->layout,child,&c) && read_psp(d,&d->layout,parent,&p),"inheritance PSP",E_PARAM);
    REQUIRE(c.count==20 && same(c.table,at(child,0x18)),"child JFT not the PSP's 20 entries",E_HANDLE);
    for (unsigned i=0;i<20;i++) {
        uint8_t want=0xff;
        if (i<p.count && p.h[i]!=0xff) {
            REQUIRE(rd(d,d->layout.slot[p.h[i]],slot,4),"inherited SFT slot",E_PARAM);
            if (u16(slot) && !(u16(slot+2)&0x80)) want=p.h[i];
        }
        REQUIRE(c.h[i]==want,"first-20 inheritance mismatch",E_HANDLE);
    }
    return 0;
}
static struct dfv2_process *find(struct dos_unified *d,uint16_t psp) {
    for (unsigned i=0;i<DFV2_PROCESSES;i++) if (d->process[i].used && d->process[i].psp==psp) return &d->process[i];
    return 0;
}
static struct dfv2_process *free_slot(struct dos_unified *d) {
    for (unsigned i=0;i<DFV2_PROCESSES;i++) if (!d->process[i].used) return &d->process[i];
    return 0;
}
/* The arena header in front of a program block (RBIL 2152 MCB): 'M'/'Z',
 * owner and size in paragraphs. EXEC_BEGIN precedes child_psp(), so the
 * block still belongs to the parent; patchPSP() gives it to the child. */
static bool arena(struct dos_unified *d,uint16_t psp,uint16_t owner) {
    uint8_t m[5];
    return psp>1 && rd(d,at((uint16_t)(psp-1),0),m,5) && (m[0]=='M' || m[0]=='Z') &&
        u16(m+1)==owner && u16(m+3)>=0x10 && conv(at(psp,0),256);
}
/* Copy/child PSP records can be replaced; an EXEC or root record cannot. */
static bool replaceable(const struct dos_unified *d,const struct dfv2_process *p) {
    return (p->kind==0x26 || p->kind==0x55) && p->psp!=d->owning && p->psp!=d->selected;
}

/* Events 1-7 (event 8 belongs to the monitor). Every check precedes the
 * backend reservation; nothing after the reservation can fail. */
static int lifecycle(struct dos_unified *d,unsigned event,uint16_t psp,uint16_t parent,uint16_t kind) {
    struct dfv2_process *p=find(d,psp),candidate={0},*target=0; struct jft j; int e; uint16_t live;
    REQUIRE(event>=1 && event<=7,"event number",E_PARAM);
    REQUIRE(!kind || (event==1 && (kind==0x26 || kind==0x55)),"creation kind",E_PARAM);
    REQUIRE(event==1 ? kind!=0 : 1,"PSP_CREATE without kind",E_PARAM);
    REQUIRE(current(d,&live),"SDA PSP field",E_PARAM);
    switch (event) {
    case 1: case 3:
        REQUIRE(event==3 ? arena(d,psp,parent) : read_psp(d,&d->layout,psp,&j),"new PSP",E_PARAM);
        REQUIRE(!p || replaceable(d,p),"PSP already owned",E_PARAM);
        REQUIRE((event!=3 && kind!=0x55) || parent==live,"parent is not the selected PSP",E_PARAM);
        if (kind==0x55 && (e=inherited(d,psp,parent))) return e;
        target=p ? p : free_slot(d);
        REQUIRE(target,"process records exhausted",E_MEMORY);
        candidate=(struct dfv2_process){.used=true,.provisional=event==3,.kind=event==3 ? 1 : (uint8_t)kind,
            .psp=psp,.parent=parent};
        break;
    case 2:
        REQUIRE(read_psp(d,&d->layout,psp,&j),"selected PSP",E_PARAM);
        break;
    case 4:
        REQUIRE(p && p->provisional && p->parent==parent && parent==live,"commit without matching begin",E_PARAM);
        REQUIRE(arena(d,psp,psp),"child block not owned by the child",E_PARAM);
        if ((e=inherited(d,psp,parent))) return e;
        candidate=*p; candidate.provisional=false; target=p;
        break;
    case 5:
        REQUIRE(p && p->provisional && p->parent==parent,"rollback without matching begin",E_PARAM);
        target=p;
        break;
    default: /* 6, 7 */
        REQUIRE(p && !p->provisional && p->kind && (event==6 || p->kind==1),"exit of an untracked process",E_PARAM);
        candidate=*p; candidate.resident=event==7; target=p;
        break;
    }
    if ((e=prepare(d,event,psp,parent))) { d->why="backend refused"; return e; }
    /* Publication: cannot fail. */
    d->selected=live;
    if (event==2) d->selected=psp;
    else if (event==5) memset(target,0,sizeof(*target));
    else if (event==6) {
        uint16_t up=target->parent;
        if (d->owning==psp) d->owning=up;
        d->selected=up; memset(target,0,sizeof(*target));
    } else {
        *target=candidate;
        if (event==1 && kind==0x55) d->selected=psp;
        if (event==4) d->selected=d->owning=psp;
        if (event==7) { if (d->owning==psp) d->owning=target->parent; d->selected=target->parent; }
    }
    commit(d,event,psp); d->stats.events++;
    return 0;
}

/* ---- the final layout --------------------------------------------- */
static bool device_name(const uint8_t *h,const char *name) { return !memcmp(h+10,name,8); }
/* The one country-1/CP437 definition is the kernel's hardcoded package;
 * it is checked for shape and invariants and snapshotted here. */
static const char *nls_tables(struct dos_unified *d,struct dfv2_layout *l) {
    uint8_t *c=l->country,*u=l->upcase,*t=l->terminator,*k=l->collate;
    if (!rd(d,l->nls[DFV2_COUNTRY],c,3)) return "country table";
    unsigned size=u16(c+1);
    if (c[0]!=1 || size!=38 || !rd(d,l->nls[DFV2_COUNTRY],c,size+3) ||
        u16(c+3)!=1 || u16(c+5)!=437) return "country 1 / code page 437";
    if (!rd(d,l->nls[DFV2_UPCASE],u,130) || !rd(d,l->nls[DFV2_FUPCASE],l->fupcase,130) ||
        !rd(d,l->nls[DFV2_TERMINATOR],t,24) || !rd(d,l->nls[DFV2_COLLATE],k,258) ||
        !rd(d,l->nls[DFV2_DBCS],l->dbcs,4)) return "NLS table extent";
    if (u16(u)!=128 || memcmp(u,l->fupcase,130)) return "upcase tables";
    if (u16(k)!=256) return "collating table";
    if (u16(l->dbcs) || u16(l->dbcs+2)) return "DBCS table not empty";
    for (unsigned i=0;i<128;i++) {
        uint8_t up=u[2+i];
        if (up>=0x80 ? u[2+up-0x80]!=up : !(up>='A' && up<='Z')) return "upcase not idempotent";
    }
    for (unsigned ch='a';ch<='z';ch++) if (k[2+ch]!=k[2+ch-32]) return "collating case";
    static const char terminators[]=".\"/\\[]:|<>+=;,";
    if (u16(t)!=22 || t[3] || t[4]!=0xff || t[6] || t[7]!=0x20 || t[9]!=14 || memcmp(t+10,terminators,14))
        return "filename terminator table";
    return 0;
}
static const char *sft_chain(struct dos_unified *d,struct dfv2_layout *l,const struct span *fixed,unsigned nfixed) {
    struct dos_file_far b=l->sft_head; unsigned total=0;
    while (b.off!=0xffff) {
        uint8_t h[6];
        if (l->blocks==DFV2_SFT_BLOCKS || !rd(d,b,h,6)) return "SFT block header";
        unsigned count=u16(h+4);
        if (!count || count>DFV2_SFT_MAX-total || !conv(b,6+59u*count)) return "SFT block extent";
        struct span s=sp(b,6+59u*count);
        for (unsigned i=0;i<nfixed;i++) if (!disjoint(s,fixed[i])) return "SFT block overlaps a control area";
        for (unsigned i=0;i<l->blocks;i++) if (!disjoint(s,sp(l->block[i],6+59u*l->block_count[i]))) return "SFT blocks overlap";
        l->block[l->blocks]=b; l->block_count[l->blocks++]=(uint16_t)count;
        for (unsigned i=0;i<count;i++) l->slot[total+i]=plus(b,6+59*i);
        total+=count; b=fp(h);
    }
    return total==l->sft_count && total>=5 ? 0 : "SFT count";
}
static int final_bind(struct dos_unified *d,struct dfv2_call *k) {
    uint8_t r[DFV2_BIND_BYTES]; struct dfv2_layout l=d->layout; const char *why;
    struct dos_file_far rec=at(k->ds,k->si);
    REQUIRE(rd(d,rec,r,sizeof(r)) && !memcmp(r,"DFB2",4) && u16(r+4)==DFV2_VERSION && u16(r+6)==DFV2_BIND_BYTES &&
            u16(r+8)==1 && !u16(r+0x0a),"final bind record header",E_PARAM);
    REQUIRE(u32(r+0x0c)==d->generation,"stale bind generation",E_PARAM);
    REQUIRE(!driver_fields(r,&l) && !memcmp(&l,&d->layout,offsetof(struct dfv2_layout,kernel_id)),
            "driver fields differ from registration",E_PARAM);
    for (unsigned i=0xa8;i<0xd0;i++) REQUIRE(!r[i],"reserved/output bytes not zero",E_PARAM);
    memcpy(l.kernel_id,r+0x30,16);
    l.data_seg=u16(r+0x40); l.data_bytes=u16(r+0x42); l.text_seg=u16(r+0x44); l.text_bytes=u16(r+0x46);
    l.lol=fp(r+0x48); l.sda=fp(r+0x4c); l.sda_bytes=u16(r+0x50); l.sft_entry=u16(r+0x52);
    for (unsigned i=0;i<DFV2_SDA_FIELDS;i++) l.field[i]=fp(r+0x54+4*i);
    l.sft_head=fp(r+0x7c); l.sft_count=u16(r+0x80); l.cds_count=u16(r+0x82); l.cds=fp(r+0x84);
    l.cds_stride=u16(r+0x88); l.root=u16(r+0x8a); l.nul=fp(r+0x8c);
    for (unsigned i=0;i<DFV2_NLS_TABLES;i++) l.nls[i]=fp(r+0x90+4*i);
    struct span driver=driver_span(&l),data=sp(at(l.data_seg,0),l.data_bytes),text=sp(at(l.text_seg,0),l.text_bytes);
    struct span sda=sp(l.sda,l.sda_bytes),lol=sp(at(l.lol.seg,(uint16_t)(l.lol.off-0x26)),0x26+0x6a);
    struct span cds=sp(l.cds,(uint32_t)l.cds_count*l.cds_stride),root=sp(at(l.root,0),256);
    REQUIRE(!memcmp(l.kernel_id,kernel_id,16),"kernel build identity",E_PARAM);
    REQUIRE(conv(at(l.data_seg,0),l.data_bytes) && conv(at(l.text_seg,0),l.text_bytes),
            "kernel data/text not in conventional memory (DOS=LOW required)",E_PARAM);
    REQUIRE(disjoint(driver,data) && disjoint(driver,text) && disjoint(data,text),"kernel/driver segments overlap",E_PARAM);
    REQUIRE(l.lol.off>=0x26 && inside(data,lol) && inside(data,sda) && l.sda_bytes>=0x18,"LoL/SDA outside kernel data",E_PARAM);
    REQUIRE(l.sft_entry==59,"SFT entry size",E_PARAM);
    REQUIRE(l.cds_count && l.cds_count<=26 && l.cds_stride==88 && conv(l.cds,cds.bytes),"CDS array",E_PARAM);
    REQUIRE(conv(at(l.root,0),256),"root PSP extent",E_PARAM);
    REQUIRE(disjoint(cds,driver) && disjoint(cds,text) && disjoint(cds,sda) && disjoint(cds,lol) && disjoint(cds,root),
            "CDS overlaps a control area",E_PARAM);
    REQUIRE(disjoint(root,driver) && disjoint(root,data) && disjoint(root,text),"root PSP overlaps",E_PARAM);
    REQUIRE(same(l.nul,plus(l.lol,0x22)),"NUL device is not LoL+22h",E_PARAM);
    for (unsigned i=0;i<DFV2_SDA_FIELDS;i++) REQUIRE(same(l.field[i],plus(l.sda,sda_offsets[i])),"SDA field address",E_PARAM);
    for (unsigned i=0;i<DFV2_NLS_TABLES;i++) REQUIRE(inside(data,sp(l.nls[i],2)),"NLS table outside kernel data",E_PARAM);
    uint8_t lv[0x22];
    REQUIRE(rd(d,l.lol,lv,sizeof(lv)) && same(fp(lv+4),l.sft_head) && same(fp(lv+0x16),l.cds) && lv[0x21]==l.cds_count,
            "List-of-Lists disagrees with the record",E_PARAM);
    const struct span fixed[]={driver,text,sda,lol,cds,root};
    if ((why=sft_chain(d,&l,fixed,6)) || (why=nls_tables(d,&l))) { d->why=why; return E_PARAM; }
    /* Device chain from NUL: CON, AUX, PRN, CLOCK$ and this driver once. */
    struct dos_file_far dev=l.nul; unsigned found=0,mine=0;
    for (unsigned n=0;n<64 && dev.off!=0xffff;n++) {
        uint8_t h[18]; REQUIRE(rd(d,dev,h,sizeof(h)),"device header",E_PARAM);
        if (u16(h+4)&0x8000) {
            if (device_name(h,"CON     ") && (u16(h+4)&3)==3) found|=1;
            if (device_name(h,"AUX     ")) found|=2;
            if (device_name(h,"PRN     ")) found|=4;
            if (device_name(h,"CLOCK$  ") && (u16(h+4)&8)) found|=8;
            if (device_name(h,"CIUKIDF$")) mine+=same(dev,at(l.driver_seg,0)) ? 1 : 100;
        }
        dev=fp(h);
    }
    REQUIRE(dev.off==0xffff && found==15 && mine==1,"device chain",E_PARAM);
    /* Root: selected now, a CD 20h PSP whose 20-entry JFT is in the PSP. */
    uint8_t psp[2]; struct jft j;
    REQUIRE(rd(d,l.field[DFV2_PSP],psp,2) && u16(psp)==l.root,"root is not the selected PSP",E_PARAM);
    REQUIRE(read_psp(d,&l,l.root,&j) && j.count==20 && same(j.table,at(l.root,0x18)),"root PSP/JFT",E_PARAM);
    REQUIRE(d->memory.check(d->memory.arg,lin(plus(rec,0xb0)),32,true),"claim output not writable",E_PARAM);
    if (d->backend.claim) memcpy(l.claim,d->backend.claim,32);
    int e=prepare(d,DFV2_EVENT_BIND,l.root,l.root);
    if (e) { d->why="backend refused final bind"; return e; }
    d->memory.write(d->memory.arg,lin(plus(rec,0xb0)),l.claim,32);
    d->layout=l; memset(d->process,0,sizeof(d->process));
    d->process[0]=(struct dfv2_process){.used=true,.kind=0,.psp=l.root,.parent=l.root};
    d->selected=d->owning=l.root; d->bound=true;
    commit(d,DFV2_EVENT_BIND,l.root); d->stats.binds++;
    return 0;
}

/* ---- gate entry ---------------------------------------------------- */
static enum dfv2_outcome refused(struct dos_unified *d,const char *why) {
    d->why=why; d->stats.refused++; d->memory.release(d->memory.arg); return DFV2_REFUSED;
}
#define REFUSE(cond,reason) do { if (!(cond)) return refused(d,(reason)); } while (0)
/* Phase 0: the only call made before a call record is registered. Live
 * DS:SI names the record; nothing is inspected outside the driver. */
static enum dfv2_outcome register_driver(struct dos_unified *d,const struct dos_file_frame *live) {
    uint8_t r[DFV2_BIND_BYTES],c[DFV2_CALL_BYTES],h[0x22],ret[4]; struct dfv2_layout l={0}; struct dfv2_call k;
    struct dos_file_far rec=at(live->ds,live->si); const char *why;
    REFUSE(rd(d,rec,r,sizeof(r)) && !memcmp(r,"DFB2",4) && u16(r+4)==DFV2_VERSION && u16(r+6)==DFV2_BIND_BYTES &&
           !u16(r+8) && !u16(r+0x0a),"registration record header");
    REFUSE(u32(r+0x0c)==d->generation,"stale registration generation");
    if ((why=driver_fields(r,&l))) return refused(d,why);
    for (unsigned i=0x30;i<DFV2_BIND_BYTES;i++) REFUSE(!r[i],"phase-1 fields in a registration");
    REFUSE(live->ss==l.driver_seg && live->sp==stack_top(&l)-4,"gate not entered on the driver stack");
    REFUSE(rd(d,at(live->ss,live->sp),ret,4) && u16(ret)==l.site[2] && u16(ret+2)==l.driver_seg,"event return site");
    REFUSE(rd(d,at(l.driver_seg,0),h,sizeof(h)) && u16(h)==0xffff && (u16(h+4)&0x8000) && device_name(h,"CIUKIDF$") &&
           !memcmp(h+0x12,"CIUKIDFC",8),"CIUKIDF$ header/control block");
    REFUSE(rd(d,at(l.driver_seg,l.call_off),c,sizeof(c)) && parse_call(c,&k),"registration call record");
    REFUSE(k.generation==d->generation && k.serial==1 && k.role==DFV2_ROLE_EVENT && !k.ax &&
           k.ds==live->ds && k.si==live->si,"registration call fields");
    REFUSE(d->memory.check(d->memory.arg,lin(at(l.driver_seg,l.call_off)),DFV2_CALL_BYTES,true),"call record not writable");
    int e=prepare(d,DFV2_EVENT_BIND,0,0);
    d->layout=l; done_error(&k,(unsigned)e);
    finish(d,&k,DFV2_STATUS_DONE);
    if (!e) { d->registered=true; d->serial=1; commit(d,DFV2_EVENT_BIND,0); }
    else { memset(&d->layout,0,sizeof(d->layout)); d->why="backend refused registration"; }
    d->stats.done++; d->memory.release(d->memory.arg); return DFV2_RESUME;
}
enum dfv2_outcome dos_unified_gate(struct dos_unified *d,const struct dos_file_frame *live) {
    unsigned role=0; uint8_t c[DFV2_CALL_BYTES],ret[4]; struct dfv2_call k;
    for (unsigned i=0;i<3;i++) if (live->cs==DFV2_ROM_SEG && live->ip==gates[i]+2) role=i+1;
    REFUSE(role,"not a DFV2 gate");
    REFUSE(!d->dead,"VM stopped");
    REFUSE(!d->pending,"request pending");
    if (!d->registered) {
        REFUSE(role==DFV2_ROLE_EVENT,"unregistered direct/local gate");
        return register_driver(d,live);
    }
    const struct dfv2_layout *l=&d->layout;
    REFUSE(live->ss==l->driver_seg && live->sp==stack_top(l)-4,"gate not entered on the driver stack");
    REFUSE(rd(d,at(live->ss,live->sp),ret,4) && u16(ret)==l->site[role-1] && u16(ret+2)==l->driver_seg,"role return site");
    REFUSE(rd(d,at(l->driver_seg,l->call_off),c,sizeof(c)) && parse_call(c,&k),"call record");
    REFUSE(k.generation==d->generation,"stale call generation");
    REFUSE(d->serial!=UINT32_MAX && k.serial==d->serial+1,"call serial");
    REFUSE(k.role==role,"call role differs from gate");
    if (role==DFV2_ROLE_EVENT) {
        REFUSE(!k.frame_ss && !k.frame_sp && !k.arg,"event frame fields");
        REFUSE(k.ax==DFV2_EVENT_BIND ? !d->bound : d->bound,"bind state");
    } else {
        uint8_t f[24];
        REFUSE(d->bound,"unbound direct/local gate");
        if (role==DFV2_ROLE_DIRECT) {
            /* FreeDOS iregs on the caller's stack: AX..ES, IP, CS, FLAGS. */
            const uint16_t r[9]={k.ax,k.bx,k.cx,k.dx,k.si,k.di,k.bp,k.ds,k.es};
            REFUSE(!k.arg && rd(d,at(k.frame_ss,k.frame_sp),f,24) && u16(f+22)==k.flags,"direct iregs frame");
            for (unsigned i=0;i<9;i++) REFUSE(u16(f+2*i)==r[i],"direct registers differ from iregs");
        } else {
            /* Redirector callout: IP, CS, FLAGS, 11xxh stack word on a
             * kernel stack, returning into the resident kernel text. */
            REFUSE(k.frame_ss==l->data_seg && rd(d,at(k.frame_ss,k.frame_sp),f,8) &&
                   inside(sp(at(l->data_seg,0),l->data_bytes),sp(at(k.frame_ss,k.frame_sp),8)),"local frame stack");
            REFUSE(u16(f+4)==k.flags && u16(f+6)==k.arg,"local frame FLAGS/argument");
            REFUSE(inside(sp(at(l->text_seg,0),l->text_bytes),sp(fp(f),1)),"local return outside kernel text");
        }
    }
    REFUSE(d->memory.check(d->memory.arg,lin(at(l->driver_seg,l->call_off)),DFV2_CALL_BYTES,true),"call record not writable");
    d->serial++;
    unsigned status=DFV2_STATUS_DONE;
    if (role==DFV2_ROLE_EVENT) {
        int e=k.ax==DFV2_EVENT_BIND ? final_bind(d,&k) : lifecycle(d,k.ax,k.bx,k.cx,k.dx);
        done_error(&k,(unsigned)e);
    } else {
        enum dos_file_result r=DOS_FILE_CHAIN; struct dfv2_call in=k;
        if (d->backend.request) r=d->backend.request(d->backend.arg,role,&k,&d->critical);
        if (r==DOS_FILE_PENDING) {
            d->pending=true; d->token=++d->tokens; d->suspended=in; d->stats.suspended++;
            d->memory.release(d->memory.arg); return DFV2_SUSPEND;
        }
        if (r==DOS_FILE_CHAIN) { k=in; status=DFV2_STATUS_CHAIN; }
    }
    finish(d,&k,status);
    if (status==DFV2_STATUS_CHAIN) d->stats.chained++; else d->stats.done++;
    d->memory.release(d->memory.arg); return DFV2_RESUME;
}
bool dos_unified_callback_stack(const struct dos_unified *d,uint16_t *ss,uint16_t *spp) {
    if (!d->pending || d->dead) return false;
    *ss=d->layout.driver_seg; *spp=(uint16_t)(stack_top(&d->layout)-4); return true;
}
enum dfv2_outcome dos_unified_critical(struct dos_unified *d,const struct dos_file_frame *live,unsigned reply) {
    REFUSE(!d->dead,"VM stopped");
    REFUSE(d->pending,"no pending request");
    REFUSE(live->cs==DFV2_ROM_SEG && live->ip==DFV2_CRITICAL+2,"not the critical continuation");
    REFUSE(live->ss==d->layout.driver_seg && live->sp==stack_top(&d->layout)-4,"callback did not unwind its frame");
    REFUSE(d->memory.check(d->memory.arg,lin(at(d->layout.driver_seg,d->layout.call_off)),DFV2_CALL_BYTES,true),
           "call record not writable");
    struct dfv2_call k=d->suspended; enum dos_file_result r=DOS_FILE_DONE;
    d->pending=false;    /* resumes exactly once, whatever the backend says */
    if (d->backend.complete) r=d->backend.complete(d->backend.arg,reply,&k);
    else done_error(&k,E_GENERAL);
    unsigned status=r==DOS_FILE_CHAIN ? DFV2_STATUS_CHAIN : DFV2_STATUS_DONE;
    if (status==DFV2_STATUS_CHAIN) k=d->suspended;
    finish(d,&k,status); d->stats.completed++;
    d->memory.release(d->memory.arg); return DFV2_RESUME;
}
void dos_unified_destroy(struct dos_unified *d) {
    if (d->dead) return;
    d->dead=true; d->pending=false; d->bound=false; d->registered=false;
    if (d->backend.destroy) d->backend.destroy(d->backend.arg);
    memset(d->process,0,sizeof(d->process)); d->selected=d->owning=0;
}
