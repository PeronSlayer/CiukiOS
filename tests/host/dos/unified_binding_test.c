/* H56 native DFV2 matrix: the production adapter (dos_unified.c) against
 * guarded synthetic guest memory shaped like K2043's real structures (the
 * CPU harness supplies the genuine ones). Refusals must leave the adapter
 * and guest memory byte-identical; allocation/backend failures are injected
 * at every publication. ASan/UBSan/LSan; no QEMU, no image.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm/dos_unified.h"
#include "vm/dos_file_hook.h"

#define MEM 0x110000u
static uint8_t *guest;
static unsigned checks,failures,section_failures;
static const char *verdict(void) { const char *v=failures==section_failures ? "PASS" : "FAIL"; section_failures=failures; return v; }
static uint32_t deny_lo,deny_hi;           /* [lo,hi) refused for writes */
static uint32_t writes_lo[64],writes_hi[64]; static unsigned writes;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr,"%s:%d: CHECK(%s)\n",__FILE__,__LINE__,#c); } } while (0)

static bool m_check(void *a,uint32_t lin,size_t n,bool w) {
    (void)a; if (!n || lin>=MEM || n>MEM-lin) return false;
    return !(w && lin<deny_hi && lin+n>deny_lo);
}
static void m_read(void *a,uint32_t lin,void *b,size_t n) { (void)a; memcpy(b,guest+lin,n); }
static void m_write(void *a,uint32_t lin,const void *b,size_t n) {
    (void)a; memcpy(guest+lin,b,n);
    if (writes<64) { writes_lo[writes]=lin; writes_hi[writes++]=lin+(uint32_t)n; }
}
static void m_release(void *a) { (void)a; }
static const struct dos_file_ops OPS={.check=m_check,.read=m_read,.write=m_write,.release=m_release};

/* ---- fixture backend: one live reference per published transition ---- */
static struct { unsigned fail_event,fail_at,seen[9],prepared,committed,destroyed; int live; enum dos_file_result answer; unsigned completes; } B;
static int b_prepare(void *a,unsigned e,uint16_t psp,uint16_t parent) {
    (void)a; (void)psp; (void)parent; B.seen[e]++;
    if (e==B.fail_event && B.seen[e]==B.fail_at) return 8;
    B.prepared++; return 0;
}
static void b_commit(void *a,unsigned e,uint16_t psp) {
    (void)a; (void)psp; B.committed++;
    if (e==0 || e==1 || e==3) B.live++;
    if (e==5 || e==6) B.live--;
}
static enum dos_file_result b_request(void *a,unsigned role,struct dfv2_call *k,struct dfv2_critical *c) {
    (void)a; (void)role;
    if (B.answer==DOS_FILE_DONE) { k->ax=0x1234; k->flags&=~1u; }
    if (B.answer==DOS_FILE_PENDING) *c=(struct dfv2_critical){0x1a02,0x0c,0,0,false};
    return B.answer;
}
static enum dos_file_result b_complete(void *a,unsigned reply,struct dfv2_call *k) {
    (void)a; B.completes++; k->ax=(uint16_t)(0x2400|reply); return DOS_FILE_DONE;
}
static void b_destroy(void *a) { (void)a; B.destroyed++; B.live=0; }
static uint8_t claims[32]={[0x61>>3]=1u<<(0x61&7)};
static const struct dfv2_backend BACKEND={b_prepare,b_commit,b_request,b_complete,b_destroy,claims,0};

/* ---- guest image ---- */
static void w16(uint32_t at,uint16_t v) { guest[at]=(uint8_t)v; guest[at+1]=(uint8_t)(v>>8); }
static void w32(uint32_t at,uint32_t v) { w16(at,(uint16_t)v); w16(at+2,(uint16_t)(v>>16)); }
static void wfar(uint32_t at,uint16_t seg,uint16_t off) { w16(at,off); w16(at+2,seg); }
static uint16_t r16(uint32_t at) { return (uint16_t)(guest[at]|guest[at+1]<<8); }
#define LIN(seg,off) (((uint32_t)(seg)<<4)+(off))
enum { DRV=0x2000, CALL=0x100, STK=0x200, STKB=0x200, TOP=STK+STKB, REC=0x600,
       DATA=0x0100, SDA=0x320, TEXT=0x0500, CDSSEG=0x0400, SFT2=0x0300, ROOT=0x0060, GEN=0x5600a001u };
static uint16_t data_seg=DATA,cds_seg=CDSSEG,sft2_seg=SFT2,sft2_off=0;
static uint32_t serial;

static void driver_image(void) {
    uint32_t d=LIN(DRV,0);
    w32(d,0xffffffffu); w16(d+4,0x8000); memcpy(guest+d+10,"CIUKIDF$",8); memcpy(guest+d+0x12,"CIUKIDFC",8);
}
static void bind_record(uint32_t at,unsigned phase) {
    memset(guest+at,0,DFV2_BIND_BYTES);
    memcpy(guest+at,"DFB2",4); w16(at+4,2); w16(at+6,DFV2_BIND_BYTES); w16(at+8,(uint16_t)phase);
    w32(at+0x0c,GEN); w16(at+0x10,DRV); w16(at+0x12,0x700); w16(at+0x14,CALL); w16(at+0x16,STK);
    w16(at+0x18,STKB); w16(at+0x1a,0x80); w16(at+0x1c,0x84); w16(at+0x1e,0x88);
    memcpy(guest+at+0x20,"CIUKIDF2 F3-57 1",16);
    if (!phase) return;
    memcpy(guest+at+0x30,"CIUKIDOS K2043 3",16);
    w16(at+0x40,data_seg); w16(at+0x42,0x1b00); w16(at+0x44,TEXT); w16(at+0x46,0xa000);
    wfar(at+0x48,data_seg,0x26); wfar(at+0x4c,data_seg,SDA); w16(at+0x50,0x78c); w16(at+0x52,59);
    static const uint16_t field[10]={0,1,3,4,6,7,0xc,0x10,0x14,0x16};
    for (unsigned i=0;i<10;i++) wfar(at+0x54+4*i,data_seg,(uint16_t)(SDA+field[i]));
    wfar(at+0x7c,data_seg,0xcc); w16(at+0x80,40); w16(at+0x82,26); wfar(at+0x84,cds_seg,0); w16(at+0x88,88);
    w16(at+0x8a,ROOT); wfar(at+0x8c,data_seg,0x48);
    static const uint16_t nls[6]={0xb00,0xb40,0xbd0,0xc60,0xc80,0xd90};
    for (unsigned i=0;i<6;i++) wfar(at+0x90+4*i,data_seg,nls[i]);
}
/* K2043-shaped kernel: LoL at DGROUP:26h, SDA, first SFT block at DGROUP:CCh
 * (5 entries), second block of 35 entries, CDS x26, NUL->CON->AUX->PRN->
 * CLOCK$->CIUKIDF$ device chain, country-1/CP437 tables, root PSP 0060h. */
static void kernel_image(void) {
    uint32_t d=LIN(data_seg,0),lol=d+0x26;
    memset(guest+d,0,0x1b00);
    wfar(lol+4,data_seg,0xcc); wfar(lol+0x16,cds_seg,0); guest[lol+0x21]=26;
    uint32_t nul=lol+0x22; wfar(nul,TEXT,0x10); w16(nul+4,0x8004); memcpy(guest+nul+10,"NUL     ",8);
    static const char *names[4]={"CON     ","AUX     ","PRN     ","CLOCK$  "};
    static const uint16_t attr[4]={0x8013,0x8000,0xa0c0,0x8008};
    for (unsigned i=0;i<4;i++) {
        uint32_t h=LIN(TEXT,0x10+0x20*i);
        if (i<3) wfar(h,TEXT,(uint16_t)(0x30+0x20*i)); else wfar(h,DRV,0);
        w16(h+4,attr[i]); memcpy(guest+h+10,names[i],8);
    }
    w16(d+SDA+0x10,ROOT);
    uint32_t s1=d+0xcc; wfar(s1,sft2_seg,sft2_off); w16(s1+4,5);
    uint32_t s2=LIN(sft2_seg,sft2_off); memset(guest+s2,0,6+59*35); w32(s2,0xffffffffu); w16(s2+4,35);
    w16(s1+6,1); w16(s1+6+59,3); w16(s1+6+118,1);
    uint32_t c=d+0xb00; guest[c]=1; w16(c+1,38); w16(c+3,1); w16(c+5,437); guest[c+9]='$';
    for (unsigned t=0;t<2;t++) { uint32_t u=d+(t ? 0xbd0 : 0xb40); w16(u,128);
        for (unsigned i=0;i<128;i++) guest[u+2+i]=(uint8_t)(0x80+i); }
    uint32_t term=d+0xc60; w16(term,22); guest[term+4]=0xff; guest[term+7]=0x20; guest[term+9]=14;
    memcpy(guest+term+10,".\"/\\[]:|<>+=;,",14);
    uint32_t col=d+0xc80; w16(col,256); for (unsigned i=0;i<256;i++) guest[col+2+i]=(uint8_t)(i>='a' && i<='z' ? i-32 : i);
    uint32_t p=LIN(ROOT,0); memset(guest+p,0,256); w16(p,0x20cd); w16(p+0x16,ROOT);
    w16(p+0x32,20); wfar(p+0x34,ROOT,0x18); memset(guest+p+0x18,0xff,20);
    guest[p+0x18]=1; guest[p+0x19]=1; guest[p+0x1a]=1; guest[p+0x1b]=0; guest[p+0x1c]=2;
}
static struct dos_unified U;
static void fresh(void) {
    memset(guest,0,MEM); memset(&B,0,sizeof(B)); B.fail_event=99; writes=0; deny_lo=deny_hi=0;
    data_seg=DATA; cds_seg=CDSSEG; sft2_seg=SFT2; sft2_off=0;
    driver_image(); kernel_image(); serial=0;
    dos_unified_init(&U,GEN,&OPS,&BACKEND);
}
/* What CIUKIDF.SYS does around one gate: the call record, the far return
 * at its private stack top, INT FE at the role's ROM gate. */
static void prepare_call(unsigned role,uint16_t ax,uint16_t bx,uint16_t cx,uint16_t dx) {
    uint32_t c=LIN(DRV,CALL); memset(guest+c,0,DFV2_CALL_BYTES);
    w16(c,2); w16(c+2,48); w32(c+4,GEN); w32(c+8,++serial); w16(c+0x0c,(uint16_t)role); w16(c+0x0e,0xffff);
    w16(c+0x12,ax); w16(c+0x14,bx); w16(c+0x16,cx); w16(c+0x18,dx);
    static const uint16_t site[4]={0,0x80,0x84,0x88};
    wfar(LIN(DRV,TOP-4),DRV,site[role]);
}
static struct dos_file_frame live(unsigned role) {
    static const uint16_t gate[4]={0,DFV2_DIRECT,DFV2_LOCAL,DFV2_EVENT};
    return (struct dos_file_frame){.cs=DFV2_ROM_SEG,.ip=(uint16_t)(gate[role]+2),.ss=DRV,.sp=TOP-4};
}
static uint16_t status(void) { return r16(LIN(DRV,CALL+0x0e)); }
static uint16_t out_ax(void) { return r16(LIN(DRV,CALL+0x12)); }
static bool out_cf(void) { return r16(LIN(DRV,CALL+0x10))&1; }
static enum dfv2_outcome event(uint16_t ev,uint16_t psp,uint16_t parent,uint16_t kind) {
    prepare_call(DFV2_ROLE_EVENT,ev,psp,parent,kind); struct dos_file_frame f=live(DFV2_ROLE_EVENT);
    return dos_unified_gate(&U,&f);
}
static enum dfv2_outcome do_register(void) {
    bind_record(LIN(DRV,REC),0); prepare_call(DFV2_ROLE_EVENT,0,0,0,0);
    w16(LIN(DRV,CALL+0x20),DRV); w16(LIN(DRV,CALL+0x1a),REC);
    struct dos_file_frame f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC;
    return dos_unified_gate(&U,&f);
}
static enum dfv2_outcome do_final(void) {
    uint32_t rec=LIN(0x7000,0); bind_record(rec,1);
    prepare_call(DFV2_ROLE_EVENT,0,0,0,0); w16(LIN(DRV,CALL+0x20),0x7000); w16(LIN(DRV,CALL+0x1a),0);
    struct dos_file_frame f=live(DFV2_ROLE_EVENT); return dos_unified_gate(&U,&f);
}
/* A refused call changes nothing: adapter state (but the refusal count and
 * reason) and every guest byte are as before. */
static uint8_t *snap_guest; static struct dos_unified snap_u;
static void snapshot(void) { memcpy(snap_guest,guest,MEM); snap_u=U; }
static bool unchanged(void) {
    struct dos_unified now=U; now.stats.refused=snap_u.stats.refused; now.why=snap_u.why;
    return !memcmp(snap_guest,guest,MEM) && !memcmp(&now,&snap_u,sizeof(now));
}

static void rom_and_discovery(void) {
    uint8_t page[4096]={0},v1[4096]={0};
    dos_file_hook_rom(v1); memcpy(page,v1,sizeof(page)); dos_unified_rom(page,GEN);
    CHECK(!memcmp(page,v1,DFV2_DIRECT-0x1000));                         /* v1 bytes unchanged */
    CHECK(!memcmp(page+(DFV2_END-0x1000),v1+(DFV2_END-0x1000),0x2000-DFV2_END));
    CHECK(DOS_HOOK_END<=DFV2_DIRECT);
    for (unsigned i=DFV2_DIRECT-0x1000;i<DFV2_END-0x1000;i++) CHECK(!v1[i]);
    static const uint8_t gates[16]={0xcd,0xfe,0xcb,0xd2,0xcd,0xfe,0xcb,0xa2,0xcd,0xfe,0xcb,0xe2,0xcd,0xfe,0xf4,0xc2};
    CHECK(!memcmp(page+(DFV2_DIRECT-0x1000),gates,16));
    uint8_t *d=page+(DFV2_DESCRIPTOR-0x1000);
    CHECK(!memcmp(d,"DFV2",4) && d[4]==2 && d[6]==48 && d[8]==0x3f && d[0x0c]==0x00 && d[0x0d]==0x1c);
    CHECK((uint32_t)(d[0x1c]|d[0x1d]<<8|d[0x1e]<<16|(uint32_t)d[0x1f]<<24)==GEN && d[0x20]==48 && d[0x22]==0xd0);
    struct dos_file_frame f={.ax=0xc148,.bx=0x4644,.cx=2,.dx=0x564d};
    CHECK(dos_unified_discover(&f) && !f.ax && f.cx==2 && f.es==0xf000 && f.di==DFV2_DESCRIPTOR);
    const uint16_t bad[][4]={{0xc148,0x4644,1,0x564d},{0xc148,0x4644,3,0x564d},{0xc149,0x4644,2,0x564d},
                             {0xc148,0x4645,2,0x564d},{0xc148,0x4644,2,0x564e}};
    for (unsigned i=0;i<5;i++) {
        struct dos_file_frame g={.ax=bad[i][0],.bx=bad[i][1],.cx=bad[i][2],.dx=bad[i][3],.es=7,.di=9},h=g;
        CHECK(!dos_unified_discover(&g) && !memcmp(&g,&h,sizeof(g)));
    }
    /* v1 discovery answers CX=1 only: the two versions cannot alias. */
    struct dos_file_frame g={.ax=0xc148,.bx=0x4644,.cx=2,.dx=0x564d};
    CHECK(!dos_file_hook_discover(&g));
    printf("%s: %s %s\n","H56-01 ROM/discovery",verdict(),"v1 page bytes unchanged, gates/roles exact, versions disjoint");
}

static void negotiation(void) {
    /* absent/old/truncated/stale/overlap/wrong-role/spoof registrations */
    struct { const char *what; uint32_t at; uint32_t value; unsigned width; } cases[]={
        {"signature",LIN(DRV,REC),0x33424644,4},{"version",LIN(DRV,REC)+4,1,2},
        {"truncated",LIN(DRV,REC)+6,0xcf,2},{"phase",LIN(DRV,REC)+8,2,2},
        {"stale generation",LIN(DRV,REC)+0x0c,GEN^1,4},{"driver identity",LIN(DRV,REC)+0x20,0x41,1},
        {"call overlaps stack",LIN(DRV,REC)+0x14,0x210,2},{"tiny stack",LIN(DRV,REC)+0x18,0x80,2},
        {"stack beyond resident",LIN(DRV,REC)+0x12,0x300,2},{"duplicate sites",LIN(DRV,REC)+0x1c,0x80,2},
        {"phase-1 field in phase 0",LIN(DRV,REC)+0x40,1,2},{"header name",LIN(DRV,10),0x58,1},
        {"control block",LIN(DRV,0x12),0x58,1},{"char device bit",LIN(DRV,4),0,2},
        {"call serial",LIN(DRV,CALL+8),7,4},{"call role",LIN(DRV,CALL+0x0c),1,2},
        {"call status",LIN(DRV,CALL+0x0e),0,2},{"call reserved",LIN(DRV,CALL+0x2e),1,2},
        {"wrong return site (direct)",LIN(DRV,TOP-4),0x80,2},{"return segment",LIN(DRV,TOP-2),0x1fff,2},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        fresh();
        bind_record(LIN(DRV,REC),0); prepare_call(DFV2_ROLE_EVENT,0,0,0,0);
        w16(LIN(DRV,CALL+0x20),DRV); w16(LIN(DRV,CALL+0x1a),REC);
        if (cases[i].width==1) guest[cases[i].at]=(uint8_t)cases[i].value;
        else if (cases[i].width==2) w16(cases[i].at,(uint16_t)cases[i].value); else w32(cases[i].at,cases[i].value);
        struct dos_file_frame f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC;
        snapshot();
        enum dfv2_outcome r=dos_unified_gate(&U,&f);
        if (r!=DFV2_REFUSED || !unchanged() || U.registered || B.prepared) fprintf(stderr,"case %s why=%s\n",cases[i].what,U.why);
        CHECK(r==DFV2_REFUSED && unchanged() && !U.registered && !B.prepared);
    }
    /* wrong gate (direct/local before registration), wrong stack, wrong IP */
    fresh(); bind_record(LIN(DRV,REC),0); prepare_call(DFV2_ROLE_EVENT,0,0,0,0);
    w16(LIN(DRV,CALL+0x20),DRV); w16(LIN(DRV,CALL+0x1a),REC);
    struct dos_file_frame f=live(DFV2_ROLE_DIRECT); f.ds=DRV; f.si=REC; snapshot();
    CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC; f.sp-=2; CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC; f.ss=0x2001; CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC; f.ip=DFV2_EVENT; CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC; f.cs=0xf001; CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    deny_lo=LIN(DRV,CALL); deny_hi=deny_lo+DFV2_CALL_BYTES;      /* unwritable call record */
    f=live(DFV2_ROLE_EVENT); f.ds=DRV; f.si=REC; CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    /* allocation failure at registration: contained DONE/CF/8, nothing kept */
    fresh(); B.fail_event=0; B.fail_at=1;
    CHECK(do_register()==DFV2_RESUME && status()==1 && out_cf() && out_ax()==8 && !U.registered && !B.prepared);
    serial=0;                                   /* a reloaded driver starts again */
    CHECK(do_register()==DFV2_RESUME && status()==1 && !out_cf() && U.registered && B.live==1);
    /* a second registration is a final bind with a phase-0 record: 87 */
    CHECK(do_register()==DFV2_RESUME && status()==1 && out_cf() && out_ax()==87 && U.registered && !U.bound);
    /* final bind: valid split-segment and same-segment layouts */
    CHECK(do_final()==DFV2_RESUME && status()==1 && !out_cf() && U.bound && U.layout.blocks==2 && U.layout.sft_count==40);
    CHECK(U.selected==ROOT && U.owning==ROOT && U.process[0].used && U.process[0].psp==ROOT);
    CHECK(guest[LIN(0x7000,0xb0+(0x61>>3))]==(1u<<(0x61&7)));  /* claim output */
    CHECK(do_final()==DFV2_REFUSED);                      /* binds once ("bind state") */
    fresh(); sft2_seg=DATA; sft2_off=0x1700; cds_seg=(uint16_t)(DATA+0x0e0); kernel_image();
    CHECK(do_register()==DFV2_RESUME && do_final()==DFV2_RESUME && !out_cf() && U.bound);
    /* allocation failure at the final publication, then a clean retry is
     * impossible (binding is one-shot) but nothing was published */
    fresh(); B.fail_event=0; B.fail_at=2;
    CHECK(do_register()==DFV2_RESUME && do_final()==DFV2_RESUME && out_cf() && out_ax()==8 && !U.bound && B.live==1);
    CHECK(!U.process[0].used && !U.selected);
    printf("%s: %s %s\n","H56-01 negotiation",verdict(),"old/absent/truncated/overlap/wrong-role/spoof/stale refused unchanged; same/split layouts; alloc failure at both publications");
}

static const char *layout_cases[]={"SDA field","LoL sfthead","LoL CDS","lastdrive","SFT count",
    "SFT block overlaps CDS","SFT block in driver","SFT entry size","CDS stride","CDS over root PSP",
    "root not selected","root JFT count","root JFT pointer","root JFT index","no CLOCK$",
    "driver missing from chain","country 2","code page 850","short country record","upcase not idempotent",
    "filename upcase differs","DBCS not empty","terminators","kernel identity","text in HMA (DOS=HIGH)",
    "text overlaps data","stale bind generation","driver fields changed","output bytes not zero"};
static void layout_mutate(unsigned i) {
    switch (i) {
    case 0: w16(LIN(0x7000,0x70),0x0331); break;
    case 1: wfar(LIN(DATA,0x26+4),DATA,0xd0); break;
    case 2: wfar(LIN(DATA,0x26+0x16),CDSSEG,0x10); break;
    case 3: guest[LIN(DATA,0x26+0x21)]=5; break;
    case 4: w16(LIN(0x7000,0x80),41); break;
    case 5: wfar(LIN(DATA,0xcc),CDSSEG,0x100); break;
    case 6: wfar(LIN(DATA,0xcc),DRV,0x500); break;
    case 7: w16(LIN(0x7000,0x52),53); break;
    case 8: w16(LIN(0x7000,0x88),81); break;
    case 9: wfar(LIN(0x7000,0x84),ROOT,0); break;
    case 10: w16(LIN(DATA,SDA+0x10),0x61); break;
    case 11: w16(LIN(ROOT,0x32),30); break;
    case 12: wfar(LIN(ROOT,0x34),0x0070,0); break;
    case 13: guest[LIN(ROOT,0x18+7)]=40; break;
    case 14: memcpy(guest+LIN(TEXT,0x70+10),"CLOCK#  ",8); break;
    case 15: wfar(LIN(TEXT,0x70),0xffff,0xffff); break;
    case 16: w16(LIN(DATA,0xb03),2); break;
    case 17: w16(LIN(DATA,0xb05),850); break;
    case 18: w16(LIN(DATA,0xb01),28); break;
    case 19: guest[LIN(DATA,0xb42)]=0x81; break;
    case 20: guest[LIN(DATA,0xbd2)]=0x41; break;
    case 21: w16(LIN(DATA,0xd90),2); break;
    case 22: guest[LIN(DATA,0xc60+10)]='!'; break;
    case 23: guest[LIN(0x7000,0x3f)]='9'; break;
    case 24: w16(LIN(0x7000,0x44),0xffff); break;
    case 25: w16(LIN(0x7000,0x44),DATA); break;
    case 26: w32(LIN(0x7000,0x0c),GEN+1); break;
    case 27: w16(LIN(0x7000,0x18),0x100); break;
    case 28: guest[LIN(0x7000,0xb0)]=1; break;
    }
}
static void layout_refusals(void) {
    unsigned n=sizeof(layout_cases)/sizeof(*layout_cases);
    for (unsigned i=0;i<n;i++) {
        fresh(); CHECK(do_register()==DFV2_RESUME);
        uint32_t rec=LIN(0x7000,0); bind_record(rec,1); layout_mutate(i);
        prepare_call(DFV2_ROLE_EVENT,0,0,0,0); w16(LIN(DRV,CALL+0x20),0x7000); w16(LIN(DRV,CALL+0x1a),0);
        unsigned before=B.prepared; writes=0; struct dos_file_frame f=live(DFV2_ROLE_EVENT);
        enum dfv2_outcome r=dos_unified_gate(&U,&f);
        bool ok=r==DFV2_RESUME && status()==1 && out_cf() && out_ax()==87 && !U.bound && B.prepared==before &&
            writes==1 && writes_lo[0]==LIN(DRV,CALL+0x0e);
        if (!ok) fprintf(stderr,"layout case %s: r=%d why=%s writes=%u\n",layout_cases[i],r,U.why,writes);
        CHECK(ok);
    }
    printf("H56-02 layout: %s %u invalid final layouts fail with 87, only the call record written\n",verdict(),n);
}

static void frames(void) {
    fresh(); CHECK(do_register()==DFV2_RESUME && do_final()==DFV2_RESUME && U.bound);
    /* direct: iregs on a caller stack at 3000:0FE8 must equal the record */
    uint32_t ir=LIN(0x3000,0xfe8); const uint16_t regs[12]={0x6100,0x1357,1,2,3,4,5,6,7,0x100,0x4000,0x3202};
    for (unsigned i=0;i<12;i++) w16(ir+2*i,regs[i]);
    prepare_call(DFV2_ROLE_DIRECT,0x6100,0x1357,1,2);
    uint32_t c=LIN(DRV,CALL); w16(c+0x10,0x3202); w16(c+0x1a,3); w16(c+0x1c,4); w16(c+0x1e,5); w16(c+0x20,6); w16(c+0x22,7);
    w16(c+0x24,0x3000); w16(c+0x26,0xfe8);
    B.answer=DOS_FILE_DONE; struct dos_file_frame f=live(DFV2_ROLE_DIRECT);
    CHECK(dos_unified_gate(&U,&f)==DFV2_RESUME && status()==1 && out_ax()==0x1234 && !out_cf() && r16(c+0x14)==0x1357);
    /* frame mismatch, role confusion, replay and stale generation refuse */
    prepare_call(DFV2_ROLE_DIRECT,0x6100,0x1357,1,2); w16(c+0x10,0x3202); w16(c+0x1a,3); w16(c+0x1c,4); w16(c+0x1e,5);
    w16(c+0x20,6); w16(c+0x22,7); w16(c+0x24,0x3000); w16(c+0x26,0xfe8); w16(ir+2,0x9999);
    snapshot(); f=live(DFV2_ROLE_DIRECT); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    w16(ir+2,0x1357); serial--;                                   /* replay of the previous serial */
    w32(c+8,serial); snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    w32(c+8,++serial+5); snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());   /* skip */
    w32(c+8,serial); w32(c+4,GEN^0x80); snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    w32(c+4,GEN); f=live(DFV2_ROLE_LOCAL); snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    wfar(LIN(DRV,TOP-4),DRV,0x84); f=live(DFV2_ROLE_DIRECT); snapshot();
    CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());             /* local return site at the direct gate */
    wfar(LIN(DRV,TOP-4),DRV,0x80); B.answer=DOS_FILE_CHAIN; f=live(DFV2_ROLE_DIRECT);
    CHECK(dos_unified_gate(&U,&f)==DFV2_RESUME && status()==0 && r16(c+0x12)==0x6100);   /* CHAIN: registers untouched */
    /* ordinary code calling a gate on its own stack */
    prepare_call(DFV2_ROLE_EVENT,2,ROOT,0,0); f=live(DFV2_ROLE_EVENT); f.ss=0x3000; f.sp=0xfe0;
    snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged()); serial--;
    /* local: kernel-stack frame IP,CS,FLAGS,arg returning into kernel text */
    uint32_t lf=LIN(DATA,0x800); wfar(lf,TEXT,0x1234); w16(lf+4,0x0002); w16(lf+6,0x0102);
    prepare_call(DFV2_ROLE_LOCAL,0x1116,0,0,0); w16(c+0x10,2); w16(c+0x24,DATA); w16(c+0x26,0x800); w16(c+0x28,0x0102);
    B.answer=DOS_FILE_CHAIN; f=live(DFV2_ROLE_LOCAL); CHECK(dos_unified_gate(&U,&f)==DFV2_RESUME && status()==0);
    const struct { uint32_t at; uint16_t v; } bad[]={{c+0x24,0x3000},{c+0x28,0x0103},{lf+2,0x0200},{c+0x26,0x1aff}};
    for (unsigned i=0;i<4;i++) {
        prepare_call(DFV2_ROLE_LOCAL,0x1116,0,0,0); w16(c+0x10,2); w16(c+0x24,DATA); w16(c+0x26,0x800); w16(c+0x28,0x0102);
        wfar(lf,TEXT,0x1234); uint16_t keep=r16(bad[i].at); w16(bad[i].at,bad[i].v);
        f=live(DFV2_ROLE_LOCAL); snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
        w16(bad[i].at,keep); serial--;
    }
    /* PENDING: monitor-only, resumes exactly once; nothing else enters */
    prepare_call(DFV2_ROLE_DIRECT,0x6f00,0,0,0); w16(c+0x10,0x3202); w16(c+0x24,0x3000); w16(c+0x26,0xfe8);
    for (unsigned i=0;i<12;i++) w16(ir+2*i,regs[i]); w16(ir,0x6f00); w16(ir+2,0); w16(ir+4,0); w16(ir+6,0);
    w16(c+0x1a,3); w16(c+0x1c,4); w16(c+0x1e,5); w16(c+0x20,6); w16(c+0x22,7);
    B.answer=DOS_FILE_PENDING; f=live(DFV2_ROLE_DIRECT);
    CHECK(dos_unified_gate(&U,&f)==DFV2_SUSPEND && U.pending && status()==0xffff && U.critical.ax==0x1a02);
    uint16_t ss,sp; CHECK(dos_unified_callback_stack(&U,&ss,&sp) && ss==DRV && sp==TOP-4);
    prepare_call(DFV2_ROLE_EVENT,2,ROOT,0,0); f=live(DFV2_ROLE_EVENT); snapshot();
    CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged()); serial--;
    struct dos_file_frame cr={.cs=DFV2_ROM_SEG,.ip=DFV2_CRITICAL+2,.ss=DRV,.sp=TOP-6};
    snapshot(); CHECK(dos_unified_critical(&U,&cr,1)==DFV2_REFUSED && unchanged() && U.pending);
    cr.sp=TOP-4; CHECK(dos_unified_critical(&U,&cr,1)==DFV2_RESUME && !U.pending && status()==1 && out_ax()==0x2401 && B.completes==1);
    snapshot(); CHECK(dos_unified_critical(&U,&cr,1)==DFV2_REFUSED && unchanged() && B.completes==1);
    /* VM stop while PENDING: the late completion has no effect */
    prepare_call(DFV2_ROLE_DIRECT,0x6f00,0,0,0); w16(c+0x10,0x3202); w16(c+0x24,0x3000); w16(c+0x26,0xfe8);
    w16(c+0x1a,3); w16(c+0x1c,4); w16(c+0x1e,5); w16(c+0x20,6); w16(c+0x22,7);
    f=live(DFV2_ROLE_DIRECT); CHECK(dos_unified_gate(&U,&f)==DFV2_SUSPEND);
    dos_unified_destroy(&U); dos_unified_destroy(&U);
    CHECK(U.dead && !U.pending && B.destroyed==1 && B.live==0);
    snapshot(); CHECK(dos_unified_critical(&U,&cr,1)==DFV2_REFUSED && unchanged() && B.completes==1);
    f=live(DFV2_ROLE_EVENT); snapshot(); CHECK(dos_unified_gate(&U,&f)==DFV2_REFUSED && unchanged());
    printf("%s: %s %s\n","H56-03 frames",verdict(),"direct iregs/local 11xx frames, role/return-site/stack/serial/generation spoofs, CHAIN untouched, PENDING once, late completion refused");
}

/* child PSP at seg with a 20-entry JFT inherited from parent's first 20 */
static void psp(uint16_t seg,uint16_t parent,bool inherit) {
    uint32_t p=LIN(seg,0); memset(guest+p,0,256); w16(p,0x20cd); w16(p+0x16,parent);
    w16(p+0x32,20); wfar(p+0x34,seg,0x18); memset(guest+p+0x18,0xff,20);
    if (inherit) for (unsigned i=0;i<20;i++) {
        uint8_t h=guest[LIN(parent,0x18)+i];
        if (h!=0xff && r16(LIN(DATA,0xcc+6)+59*h) && !(r16(LIN(DATA,0xcc+6)+59*h+2)&0x80)) guest[p+0x18+i]=h;
    }
}
static void arena(uint16_t seg,uint16_t owner) { uint32_t m=LIN(seg-1,0); guest[m]='M'; w16(m+1,owner); w16(m+3,0x20); }
static void select_psp(uint16_t seg) { w16(LIN(DATA,SDA+0x10),seg); }
static bool find_used(uint16_t psp) {
    for (unsigned i=0;i<DFV2_PROCESSES;i++) if (U.process[i].used && U.process[i].psp==psp) return true;
    return false;
}
static int ev_err(void) { return status()==1 && out_cf() ? out_ax() : status()==1 ? 0 : -1; }

static void lifetime(void) {
    fresh(); CHECK(do_register()==DFV2_RESUME && do_final()==DFV2_RESUME && U.bound);
    int live0=B.live;
    /* EXEC: begin before child_psp (block still the parent's), commit after */
    arena(0x1000,ROOT); CHECK(event(3,0x1000,ROOT,0)==DFV2_RESUME && !ev_err());
    CHECK(U.owning==ROOT && find_used(0x1000));
    psp(0x1000,ROOT,true); arena(0x1000,0x1000); CHECK(event(4,0x1000,ROOT,0)==DFV2_RESUME && !ev_err());
    CHECK(U.owning==0x1000 && U.selected==0x1000); select_psp(0x1000);
    /* nested EXEC under the child, then rollback */
    arena(0x1100,0x1000); CHECK(event(3,0x1100,0x1000,0)==DFV2_RESUME && !ev_err());
    CHECK(event(5,0x1100,0x1000,0)==DFV2_RESUME && !ev_err() && U.owning==0x1000);
    /* a commit whose JFT does not mirror CloneHandle() is refused */
    arena(0x1100,0x1000); CHECK(event(3,0x1100,0x1000,0)==DFV2_RESUME && !ev_err());
    psp(0x1100,0x1000,true); guest[LIN(0x1100,0x18+2)]=0xff; arena(0x1100,0x1100);
    CHECK(event(4,0x1100,0x1000,0)==DFV2_RESUME && ev_err()==6 && U.owning==0x1000);
    CHECK(event(5,0x1100,0x1000,0)==DFV2_RESUME && !ev_err());
    /* O_NOINHERIT entries must be absent in the child */
    w16(LIN(DATA,0xcc+6+59*2+2),0x80); psp(0x1100,0x1000,true);
    CHECK(guest[LIN(0x1100,0x18+4)]==0xff);
    arena(0x1100,0x1000); CHECK(event(3,0x1100,0x1000,0)==DFV2_RESUME && !ev_err());
    arena(0x1100,0x1100); CHECK(event(4,0x1100,0x1000,0)==DFV2_RESUME && !ev_err()); select_psp(0x1100);
    w16(LIN(DATA,0xcc+6+59*2+2),0);
    /* 26h copies without selecting; 55h clones and selects; 50h selects only */
    psp(0x1200,0,false); CHECK(event(1,0x1200,0x1100,0x26)==DFV2_RESUME && !ev_err() && U.selected==0x1100);
    psp(0x1300,0x1100,true); CHECK(event(1,0x1300,0x1100,0x55)==DFV2_RESUME && !ev_err() && U.selected==0x1300);
    select_psp(0x1300); CHECK(event(2,0x1100,0,0)==DFV2_RESUME && !ev_err() && U.selected==0x1100 && U.owning==0x1100);
    select_psp(0x1100);
    /* AH=50h cannot rewrite ancestry; an exit pops to the immutable parent */
    CHECK(event(2,0x1300,0,0)==DFV2_RESUME && !ev_err() && U.owning==0x1100); select_psp(0x1300);
    CHECK(event(2,0x1100,0,0)==DFV2_RESUME); select_psp(0x1100);
    CHECK(event(6,0x1100,0x1000,0)==DFV2_RESUME && !ev_err() && U.owning==0x1000 && U.selected==0x1000);
    select_psp(0x1000);
    /* TSR keeps its record resident */
    arena(0x1400,0x1000); CHECK(event(3,0x1400,0x1000,0)==DFV2_RESUME);
    psp(0x1400,0x1000,true); arena(0x1400,0x1400); CHECK(event(4,0x1400,0x1000,0)==DFV2_RESUME && !ev_err());
    select_psp(0x1400);
    CHECK(event(7,0x1400,0x1000,0)==DFV2_RESUME && !ev_err() && U.owning==0x1000); select_psp(0x1000);
    /* exits/TSR of untracked or provisional processes, bad kinds, event 8 */
    CHECK(event(6,0x1500,0x1000,0)==DFV2_RESUME && ev_err()==87);
    CHECK(event(7,0x1200,0x1100,0)==DFV2_RESUME && ev_err()==87);      /* a 26h copy cannot TSR */
    CHECK(event(1,0x1500,0x1000,0x27)==DFV2_RESUME && ev_err()==87);
    CHECK(event(1,0x1500,0x1000,0)==DFV2_RESUME && ev_err()==87);
    CHECK(event(8,ROOT,ROOT,0)==DFV2_RESUME && ev_err()==87 && !U.dead);
    CHECK(event(0,ROOT,ROOT,0)==DFV2_REFUSED); serial--;              /* bind twice */
    /* backend refusal at every publication leaves no state behind */
    static const unsigned evs[]={1,2,3,4,6,7};
    for (unsigned i=0;i<6;i++) {
        struct dfv2_process before[DFV2_PROCESSES]; memcpy(before,U.process,sizeof(before));
        uint16_t sel=U.selected,own=U.owning; B.fail_event=evs[i]; B.fail_at=B.seen[evs[i]]+1;
        int e=-1;
        switch (evs[i]) {
        case 1: psp(0x1600,0x1000,true); event(1,0x1600,0x1000,0x55); e=ev_err(); break;
        case 2: event(2,0x1400,0,0); e=ev_err(); break;
        case 3: arena(0x1600,0x1000); event(3,0x1600,0x1000,0); e=ev_err(); break;
        case 4: arena(0x1600,0x1000); B.fail_event=99; event(3,0x1600,0x1000,0); B.fail_event=4; B.fail_at=B.seen[4]+1;
                psp(0x1600,0x1000,true); arena(0x1600,0x1600); memcpy(before,U.process,sizeof(before));
                event(4,0x1600,0x1000,0); e=ev_err(); break;
        case 6: event(6,0x1000,ROOT,0); e=ev_err(); break;
        case 7: event(7,0x1000,ROOT,0); e=ev_err(); break;
        }
        CHECK(e==8 && !memcmp(before,U.process,sizeof(before)) && U.selected==sel && U.owning==own);
        B.fail_event=99;
        if (evs[i]==4) CHECK(event(5,0x1600,0x1000,0)==DFV2_RESUME && !ev_err());
    }
    /* process-record exhaustion is a contained error 8 */
    unsigned made=0;
    for (unsigned i=0;i<DFV2_PROCESSES+2;i++) {
        uint16_t seg=(uint16_t)(0x4000+0x20*i); psp(seg,0,false);
        if (event(1,seg,0x1000,0x26)==DFV2_RESUME && !ev_err()) made++; else CHECK(ev_err()==8);
    }
    CHECK(made<DFV2_PROCESSES);
    /* VM stop: every native reference released exactly once */
    CHECK(B.live>live0);
    dos_unified_destroy(&U); CHECK(B.live==0 && B.destroyed==1 && U.dead);
    CHECK(B.prepared==B.committed);
    printf("H56-04 lifetime: %s EXEC begin/commit/rollback, nested, inheritance/noinherit, 26/55/50, exit/TSR, refusals at 6 publications, exhaustion, destroy (prepared=%u committed=%u)\n",verdict(),B.prepared,B.committed);
}

/* DOS=UMB/LOADHIGH: a process may live in the VM's upper memory blocks once
 * the monitor publishes their range; nothing else may, and the final bind
 * still requires conventional kernel structures. */
static void upper_memory(void) {
    enum { UMB=0xC901, FIRST=0xC9000, END=0xE0000 };
    fresh(); CHECK(do_register()==DFV2_RESUME && do_final()==DFV2_RESUME && U.bound);
    /* no range published: an EXEC into upper memory is refused, nothing kept */
    arena(UMB,ROOT); CHECK(event(3,UMB,ROOT,0)==DFV2_RESUME && ev_err()==87 && !find_used(UMB));
    dos_unified_upper(&U,0x9F000,END); CHECK(!U.upper_first && !U.upper_end);   /* not above A0000h */
    dos_unified_upper(&U,0xA0000,END); CHECK(!U.upper_first && !U.upper_end);   /* video RAM below the window */
    dos_unified_upper(&U,0xC8000,END); CHECK(!U.upper_first && !U.upper_end);   /* below C9000h */
    dos_unified_upper(&U,FIRST,0xE8000); CHECK(!U.upper_first && !U.upper_end); /* past E0000h */
    dos_unified_upper(&U,FIRST,0xD0000); CHECK(U.upper_first==FIRST && U.upper_end==0xD0000); /* D000h frame arena */
    dos_unified_upper(&U,FIRST,0xF8000); CHECK(!U.upper_first && !U.upper_end);  /* into the system ROM */
    dos_unified_upper(&U,FIRST,END); CHECK(U.upper_first==FIRST && U.upper_end==END);
    /* LOADHIGH: begin, commit with the inherited JFT, TSR stays resident */
    CHECK(event(3,UMB,ROOT,0)==DFV2_RESUME && !ev_err() && find_used(UMB));
    psp(UMB,ROOT,true); arena(UMB,UMB); CHECK(event(4,UMB,ROOT,0)==DFV2_RESUME && !ev_err() && U.owning==UMB);
    select_psp(UMB); CHECK(event(7,UMB,ROOT,0)==DFV2_RESUME && !ev_err() && U.owning==ROOT); select_psp(ROOT);
    /* a child PSP (55h) created in upper memory, and AH=50h selecting it */
    psp(0xD001,ROOT,true); CHECK(event(1,0xD001,ROOT,0x55)==DFV2_RESUME && !ev_err() && U.selected==0xD001);
    select_psp(0xD001); CHECK(event(2,ROOT,0,0)==DFV2_RESUME && !ev_err() && U.selected==ROOT); select_psp(ROOT);
    /* a JFT moved into upper memory (AH=67h) by a conventional process */
    psp(0x1800,ROOT,true); memcpy(guest+LIN(0xDE00,0),guest+LIN(0x1800,0x18),20); wfar(LIN(0x1800,0x34),0xDE00,0);
    CHECK(event(1,0x1800,ROOT,0x26)==DFV2_RESUME && !ev_err() && find_used(0x1800));
    /* outside the range: video RAM, the EMS frame, an arena running past its end */
    static const uint16_t outside[3]={0xB801,0xE001,0xDFF8};
    for (unsigned i=0;i<3;i++) {
        arena(outside[i],ROOT);
        CHECK(event(3,outside[i],ROOT,0)==DFV2_RESUME && ev_err()==87 && !find_used(outside[i]));
    }
    psp(0xE801,ROOT,true); CHECK(event(1,0xE801,ROOT,0x26)==DFV2_RESUME && ev_err()==87);
    dos_unified_destroy(&U);
    /* the final bind keeps every kernel structure conventional */
    fresh(); dos_unified_upper(&U,FIRST,END); CHECK(do_register()==DFV2_RESUME);
    uint32_t rec=LIN(0x7000,0); bind_record(rec,1); wfar(LIN(0x7000,0x84),0xCA00,0); wfar(LIN(DATA,0x26+0x16),0xCA00,0);
    prepare_call(DFV2_ROLE_EVENT,0,0,0,0); w16(LIN(DRV,CALL+0x20),0x7000); w16(LIN(DRV,CALL+0x1a),0);
    struct dos_file_frame f=live(DFV2_ROLE_EVENT);
    CHECK(dos_unified_gate(&U,&f)==DFV2_RESUME && out_cf() && out_ax()==87 && !U.bound && !strcmp(U.why,"CDS array"));
    fresh(); dos_unified_upper(&U,FIRST,END); CHECK(do_register()==DFV2_RESUME);
    bind_record(rec,1); w16(LIN(0x7000,0x8a),0xCA00);
    memcpy(guest+LIN(0xCA00,0),guest+LIN(ROOT,0),256); wfar(LIN(0xCA00,0x34),0xCA00,0x18); w16(LIN(DATA,SDA+0x10),0xCA00);
    prepare_call(DFV2_ROLE_EVENT,0,0,0,0); w16(LIN(DRV,CALL+0x20),0x7000); w16(LIN(DRV,CALL+0x1a),0);
    f=live(DFV2_ROLE_EVENT);
    CHECK(dos_unified_gate(&U,&f)==DFV2_RESUME && out_cf() && out_ax()==87 && !U.bound && !strcmp(U.why,"root PSP extent"));
    printf("H56-07 upper memory: %s LOADHIGH begin/commit/TSR, 55h/50h and a JFT in UMBs once published; "
           "video RAM, EMS frame, overrun and unpublished range refused; CDS/root PSP in UMBs still refuse the bind\n",verdict());
}

int main(void) {
    guest=calloc(1,MEM); snap_guest=calloc(1,MEM);
    if (!guest || !snap_guest) return 2;
    rom_and_discovery(); negotiation(); layout_refusals(); frames(); lifetime(); upper_memory();
    free(guest); free(snap_guest);
    printf("H56 native: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
