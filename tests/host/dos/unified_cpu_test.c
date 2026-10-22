/* H56 real-CPU harness: libx86emu executes the pinned MBR/VBR, the CiukiDOS
 * K2043 fork, CIUKIDF.SYS and guest COM programs from a FAT16 disk image.
 * A minimal BIOS lives in ROM stubs reached through the guest IVT; INT FE at
 * the DFV2 gates enters the production native adapter (dos_unified.c) with a
 * fixture backend. Not a VM boot and not hardware evidence. Built normally,
 * every file operation uses FreeDOS's local FAT (f3-56). Built with
 * -DH57_IFS (f3-57), the backend is the production live IFS backend
 * (dos_unified_ifs.c) over the production VFS/FAT, mounting the same
 * in-memory disk as C: (and an optional read-only D: volume) at the bind.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <x86emu.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm/dos_unified.h"
#ifdef H57_IFS
#include "vm/dos_unified_ifs.h"
#include "vm/dos_file_volume.h"
#endif

#define TOTAL_SECTORS (63u+262144u)
#define BIOS_STUBS 0xe000u
#define IRET_STUB 0xef00u
#define MEM_TOP 0x110000u

static x86emu_t *E;
static x86emu_memio_handler_t default_memio;
static uint8_t *image; static size_t image_bytes;
static uint8_t *overlay[TOTAL_SECTORS];
static struct dos_unified U;
static char e9[1<<20],con[1<<20]; static size_t e9n,conn;
static int stopped=-1; static const char *failure; static bool vm_stopped,refusal_logged;
static char vm_record[96];
static unsigned long long ticks_base;
static unsigned bios_unhandled,ports_unknown,faults,gate_calls[5],refusals_seen;
struct saved_regs { uint32_t eax,ebx,ecx,edx,esi,edi,ebp,eflags; uint16_t ds,es,ss,sp; };
static struct saved_regs saved_cpu_regs; static bool suspended; static uint16_t suspended_ip;
static struct { unsigned patches; uint16_t patch_at[16]; uint8_t patch_byte[16]; bool host; const char *stop_on; } opt={.host=true};

/* ---- fixture backend: one reference per published native transition ---- */
struct ledger_entry { uint16_t psp; uint8_t state; };    /* 1 provisional, 2 live, 3 resident */
static struct {
    struct ledger_entry e[64]; int runtime,prepared,committed,aborted,released,bad;
    unsigned fail_event,fail_at,seen[9],requests[4],completions;
    uint8_t claim[32]; bool claims;
} L;
static struct ledger_entry *ledger_find(uint16_t psp) {
    for (unsigned i=0;i<64;i++) if (L.e[i].state && L.e[i].psp==psp) return &L.e[i];
    return 0;
}
static int b_prepare(void *a,unsigned event,uint16_t psp,uint16_t parent) {
    (void)a; (void)parent; (void)psp; if (event>8) { L.bad++; return 87; }
    L.seen[event]++;
    if (L.fail_event==event && L.fail_at==L.seen[event]) return 8;
    L.prepared++; return 0;
}
static void b_commit(void *a,unsigned event,uint16_t psp) {
    (void)a; L.committed++; L.prepared--;
    struct ledger_entry *x=ledger_find(psp);
    switch (event) {
    case 0: if (!psp) L.runtime++; else if (x) L.bad++; else goto add_live; break;
    case 1: if (x) x->state=2; else goto add_live; break;
    case 2: break;
    case 3: if (x) { L.bad++; break; }
        for (unsigned i=0;i<64;i++) if (!L.e[i].state) { L.e[i]=(struct ledger_entry){psp,1}; return; }
        L.bad++; break;
    case 4: if (!x || x->state!=1) L.bad++; else x->state=2; break;
    case 5: case 6: if (!x) L.bad++; else { x->state=0; L.released++; } break;
    case 7: if (!x || x->state!=2) L.bad++; else x->state=3; break;
    default: L.bad++;
    }
    return;
add_live:
    for (unsigned i=0;i<64;i++) if (!L.e[i].state) { L.e[i]=(struct ledger_entry){psp,2}; return; }
    L.bad++;
}
/* Direct AH=61h: DONE with sentinels; AH=62h never reaches here (unclaimed);
 * AH=6Eh: CHAIN; AH=6Fh: PENDING through INT 24h. Local: 1122h DONE, rest CHAIN. */
static enum dos_file_result b_request(void *a,unsigned role,struct dfv2_call *k,struct dfv2_critical *c) {
    (void)a; L.requests[role]++;
    if (role==DFV2_ROLE_DIRECT) {
        unsigned ah=k->ax>>8;
        if (ah==0x61) { k->ax=0x5a5a; k->bx=(uint16_t)~k->bx; k->cx=0x1234; k->dx=0xfedc; k->flags&=~1u; return DOS_FILE_DONE; }
        if (ah==0x6f) { *c=(struct dfv2_critical){0x1a02,0x000c,0x0000,0x0000,false}; return DOS_FILE_PENDING; }
        return DOS_FILE_CHAIN;
    }
    if (role==DFV2_ROLE_LOCAL && k->ax==0x1122) { k->flags&=~1u; return DOS_FILE_DONE; }
    return DOS_FILE_CHAIN;
}
static enum dos_file_result b_complete(void *a,unsigned reply,struct dfv2_call *k) {
    (void)a; L.completions++; k->ax=(uint16_t)(0x2400|reply); k->flags&=~1u; return DOS_FILE_DONE;
}
static void b_destroy(void *a) {
    (void)a;
    for (unsigned i=0;i<64;i++) if (L.e[i].state) { L.e[i].state=0; L.released++; }
    if (L.runtime) L.runtime=0;
}

/* ---- guest memory for the adapter ---- */
static bool m_check(void *a,uint32_t lin,size_t n,bool w) { (void)a; (void)w; return n && lin<MEM_TOP && n<=MEM_TOP-lin; }
static void m_read(void *a,uint32_t lin,void *b,size_t n) { (void)a; for (size_t i=0;i<n;i++) ((uint8_t *)b)[i]=(uint8_t)x86emu_read_byte_noperm(E,lin+(unsigned)i); }
/* Every native guest write is classified: the registered call record, the
 * claim output of the final bind record, or a violation. */
static unsigned writes_call,writes_claim,writes_other;
static void m_write(void *a,uint32_t lin,const void *b,size_t n) {
    (void)a;
    uint32_t call=((uint32_t)U.layout.driver_seg<<4)+U.layout.call_off;
    if (U.registered && lin>=call && lin+n<=call+DFV2_CALL_BYTES) writes_call++;
    else if (n==32) writes_claim++;
    else if (!U.registered && n==22) writes_call++;   /* registration result, before publication */
    else writes_other++;
    for (size_t i=0;i<n;i++) x86emu_write_byte_noperm(E,lin+(unsigned)i,((const uint8_t *)b)[i]);
}
static void m_release(void *a) { (void)a; }

static void fail(const char *why) { if (!failure) failure=why; x86emu_stop(E); }
static uint16_t rw(unsigned seg,unsigned off) { return (uint16_t)x86emu_read_word(E,(seg<<4)+off); }

/* ---- disk ---- */
static void sector_read(uint32_t lba,uint8_t *out) {
    if (lba<TOTAL_SECTORS && overlay[lba]) { memcpy(out,overlay[lba],512); return; }
    memset(out,0,512);
    size_t off=(size_t)lba*512;
    if (off<image_bytes) memcpy(out,image+off,image_bytes-off<512 ? image_bytes-off : 512);
}
static bool sector_write(uint32_t lba,const uint8_t *in) {
    if (lba>=TOTAL_SECTORS) return false;
    if (!overlay[lba] && !(overlay[lba]=malloc(512))) return false;
    memcpy(overlay[lba],in,512); return true;
}
static void set_cf(bool on) { if (on) E->x86.R_EFLG|=1; else E->x86.R_EFLG&=~1u; }
#ifdef H57_IFS
static unsigned int13_before,int13_after;
/* ---- f3-57 live backend: production IFS over production VFS/FAT ---- */
static struct dfv2_ifs IFS;
static struct block_cache CACHE;
static struct dos_file_volume CVOL,DVOL;
static uint8_t *dimage; static size_t dimage_bytes;
static unsigned fault_reads,faults_hit,mounts,unmounts,held_samples[64],held_n;
static bool fault_sticky,dvol_open;
static const char *ifs_fail;
static int c_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    (void)d;
    if (fault_sticky || fault_reads) { if (fault_reads) fault_reads--; faults_hit++; return -FS_EIO; }
    for (uint32_t i=0;i<n;i++) sector_read(63u+(uint32_t)lba+i,(uint8_t *)buf+512u*i);
    return 0;
}
static int c_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    (void)d;
    for (uint32_t i=0;i<n;i++) if (!sector_write(63u+(uint32_t)lba+i,(const uint8_t *)buf+512u*i)) return -FS_EIO;
    return 0;
}
static int c_flush(struct blkdev *d) { (void)d; return 0; }
static struct blkdev CPROV={.read=c_read,.write=c_write,.flush=c_flush,.capacity=262144u-63u,.sector_size=512,
    .write_cache_state=BLKDEV_CACHE_DISABLED,.logical=true};
static int d_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    (void)d; memset(buf,0,512u*n);
    for (uint32_t i=0;i<n;i++) { size_t off=(size_t)(lba+i)*512u; if (off<dimage_bytes)
        memcpy((uint8_t *)buf+512u*i,dimage+off,dimage_bytes-off<512 ? dimage_bytes-off : 512); }
    return 0;
}
static struct blkdev DPROV={.read=d_read,.sector_size=512,.logical=true};
static int h_mount(void *a,struct dfv2_ifs *s) {
    (void)a;
    if (ifs_fail && !strcmp(ifs_fail,"mount")) return -FS_EIO;
    int e=dos_file_volume_open(&CVOL,&CACHE,&CPROV,true);
    if (e) return e;
    e=dfv2_ifs_map(s,2,&CVOL.table,2,false);
    if (!e && dimage) {
        uint32_t total=dimage_bytes>=0x24 ? (uint32_t)(dimage[0x13]|dimage[0x14]<<8) : 0;
        if (!total && dimage_bytes>=0x24) total=(uint32_t)dimage[0x20]|(uint32_t)dimage[0x21]<<8|(uint32_t)dimage[0x22]<<16|(uint32_t)dimage[0x23]<<24;
        DPROV.capacity=total;            /* the BPB's extent; unwritten tail reads as zeros */
        e=dos_file_volume_open(&DVOL,&CACHE,&DPROV,false);
        /* --ifs-fail=lastdrive: the read-only view is offered as Z: (beyond
         * LASTDRIVE=Y), so the barrier refuses after a successful mount. */
        unsigned guest=ifs_fail && !strcmp(ifs_fail,"lastdrive") ? 25 : 3;
        if (!e) { dvol_open=true; e=dfv2_ifs_map(s,guest,&DVOL.table,2,true); }
    }
    if (e) { if (dvol_open) (void)dos_file_volume_close(&DVOL); dvol_open=false; (void)dos_file_volume_close(&CVOL); return e; }
    mounts++; return 0;
}
static void h_unmount(void *a,struct dfv2_ifs *s) {
    (void)a; (void)s;
    if (dvol_open) (void)dos_file_volume_close(&DVOL);
    dvol_open=false;
    (void)dos_file_volume_close(&CVOL); unmounts++;
}
static uint64_t h_wall(void *a) { (void)a; return E->x86.R_TSC/10; }
static bool h_stopped(void *a) { (void)a; return false; }
static void h_pause(void *a) { (void)a; }
static bool hreq_cancelled(void *a) { (void)a; return false; }
static uint64_t hreq_now(void *a) { (void)a; return 0; }
static void hreq_pause(void *a) { (void)a; }
/* Guest control port EBh: 00 disarm, 1n arm n failing C: provider reads,
 * 1F persistent faults, 20 sample the native ledger. Arming first commits
 * and drops the clean cache so the next data read reaches the provider. */
static void ifs_control(unsigned v) {
    if (v==0x20) { if (held_n<64) held_samples[held_n++]=dfv2_ifs_held(&IFS); return; }
    if (!v) { fault_reads=0; fault_sticky=false; return; }
    if (v<0x11 || v>0x1f || !IFS.active) return;
    static struct vfs_request hreq={.deadline=UINT64_MAX,.now=hreq_now,.cancelled=hreq_cancelled,.pause=hreq_pause};
    CVOL.table.request=&hreq; (void)vfs_flush_drive(&CVOL.table,2);
    cache_invalidate(&CACHE,&CPROV,false);
    if (v==0x1f) fault_sticky=true; else fault_reads=v-0x10;
}
#endif
static int disk(void) {
    unsigned ah=E->x86.R_AH,dl=E->x86.R_DL; set_cf(false);
#ifdef H57_IFS
    if (IFS.active) int13_after++; else int13_before++;
#endif
    if (dl!=0x80) { E->x86.R_AH=ah==0x15 ? 0 : 1; set_cf(ah!=0x15); return 1; }
    if (ah==0x00) { E->x86.R_AH=0; return 1; }
    if (ah==0x08) { unsigned cyl=TOTAL_SECTORS/(16*63)-1; E->x86.R_CH=(uint8_t)cyl; E->x86.R_CL=(uint8_t)(63|((cyl>>2)&0xc0));
        E->x86.R_DH=15; E->x86.R_DL=1; E->x86.R_AH=0; E->x86.R_BX=0; return 1; }
    if (ah==0x15) { E->x86.R_AH=3; E->x86.R_CX=(uint16_t)(TOTAL_SECTORS>>16); E->x86.R_DX=(uint16_t)TOTAL_SECTORS; return 1; }
    if (ah==0x41) { if (E->x86.R_BX!=0x55aa) { set_cf(true); E->x86.R_AH=1; return 1; }
        E->x86.R_BX=0xaa55; E->x86.R_CX=1; E->x86.R_AH=0x21; return 1; }
    if (ah==0x48) { unsigned p=(E->x86.R_DS<<4)+E->x86.R_SI;
        x86emu_write_word(E,p,0x1a); x86emu_write_word(E,p+2,0); x86emu_write_dword(E,p+4,TOTAL_SECTORS/(16*63));
        x86emu_write_dword(E,p+8,16); x86emu_write_dword(E,p+12,63); x86emu_write_dword(E,p+16,TOTAL_SECTORS);
        x86emu_write_dword(E,p+20,0); x86emu_write_word(E,p+24,512); E->x86.R_AH=0; return 1; }
    uint32_t lba,dest; unsigned count; bool write;
    if (ah==0x42 || ah==0x43) {
        unsigned p=(E->x86.R_DS<<4)+E->x86.R_SI;
        if (x86emu_read_byte(E,p)<16 || x86emu_read_dword(E,p+12)) goto bad;
        count=x86emu_read_word(E,p+2); lba=x86emu_read_dword(E,p+8);
        dest=(x86emu_read_word(E,p+6)<<4)+x86emu_read_word(E,p+4); write=ah==0x43;
    } else if (ah==0x02 || ah==0x03) {
        unsigned cyl=E->x86.R_CH+((E->x86.R_CL&0xc0u)<<2),s=E->x86.R_CL&63u;
        if (!s || E->x86.R_DH>=16) goto bad;
        count=E->x86.R_AL; lba=(cyl*16u+E->x86.R_DH)*63u+s-1;
        dest=(E->x86.R_ES<<4)+E->x86.R_BX; write=ah==0x03;
    } else goto bad;
    if (!count || count>127 || dest+count*512u>0xa0000 || lba>=TOTAL_SECTORS || count>TOTAL_SECTORS-lba) goto bad;
    for (unsigned n=0;n<count;n++) {
        uint8_t b[512];
        if (write) { for (unsigned i=0;i<512;i++) b[i]=(uint8_t)x86emu_read_byte(E,dest+n*512+i); if (!sector_write(lba+n,b)) goto bad; }
        else { sector_read(lba+n,b); for (unsigned i=0;i<512;i++) x86emu_write_byte(E,dest+n*512+i,b[i]); }
    }
    E->x86.R_AH=0; if (ah==0x02 || ah==0x03) E->x86.R_AL=(uint8_t)count; return 1;
bad:
    E->x86.R_AH=1; set_cf(true); return 1;
}
static void put_con(char c) { if (conn<sizeof(con)-1) con[conn++]=c; }
static int bios(unsigned n) {
    switch (n) {
    case 0x10:
        switch (E->x86.R_AH) {
        case 0x0e: case 0x09: case 0x0a: put_con((char)E->x86.R_AL); return 1;
        case 0x0f: E->x86.R_AX=0x5003; E->x86.R_BH=0; return 1;
        case 0x03: E->x86.R_DX=0; E->x86.R_CX=0x0607; return 1;
        case 0x08: E->x86.R_AX=0x0720; return 1;
        case 0x12: return 1;                         /* BL unchanged: no EGA info */
        case 0x1a: E->x86.R_AL=0; return 1;
        default: return 1;
        }
    case 0x11: E->x86.R_AX=0x0020; return 1;
    case 0x12: E->x86.R_AX=640; return 1;
    case 0x13: return disk();
    case 0x14: E->x86.R_AX=0x6000; return 1;
    case 0x15: E->x86.R_AH=0x86; set_cf(true); return 1;
    case 0x16:
        switch (E->x86.R_AH) {
        case 0x00: case 0x10: E->x86.R_AX=0x1c0d; return 1;
        case 0x01: case 0x11: E->x86.R_EFLG|=0x40; return 1;   /* ZF: no key */
        case 0x02: case 0x12: E->x86.R_AX&=0xff00; return 1;
        default: return 1;
        }
    case 0x17: E->x86.R_AH=0x90; return 1;
    case 0x1a: {
        unsigned long long t=E->x86.R_TSC/20000+ticks_base;
        if (E->x86.R_AH==0) { E->x86.R_CX=(uint16_t)(t>>16); E->x86.R_DX=(uint16_t)t; E->x86.R_AL=0; return 1; }
        if (E->x86.R_AH==2) { E->x86.R_CX=0x1200; E->x86.R_DX=0x0000; set_cf(false); return 1; }
        if (E->x86.R_AH==4) { E->x86.R_CX=0x2021; E->x86.R_DX=0x0514; set_cf(false); return 1; }
        set_cf(true); return 1;
    }
    case 0x18: case 0x19: fail("BIOS bootstrap/ROM BASIC reached"); return 1;
    default: bios_unhandled++; return 1;
    }
}

/* ---- DFV2 gates ---- */
static struct dos_file_frame live_frame(void) {
    return (struct dos_file_frame){E->x86.R_AX,E->x86.R_BX,E->x86.R_CX,E->x86.R_DX,E->x86.R_DS,E->x86.R_ES,
        E->x86.R_SI,E->x86.R_DI,E->x86.R_BP,(uint16_t)E->x86.R_EFLG,E->x86.R_CS,E->x86.R_IP,E->x86.R_SS,E->x86.R_SP};
}
static void set_cs_ip(uint16_t cs,uint16_t ip) { x86emu_set_seg_register(E,E->x86.seg+R_CS_INDEX,cs); E->x86.R_IP=ip; }
static void set_ss_sp(uint16_t ss,uint16_t sp) { x86emu_set_seg_register(E,E->x86.seg+R_SS_INDEX,ss); E->x86.R_SP=sp; }
static int gate(void) {
    struct dos_file_frame f=live_frame();
    for (unsigned i=0;i<4;i++) if (f.cs==DFV2_ROM_SEG && f.ip==DFV2_DIRECT+4*i+2) gate_calls[i]++;
    if (f.cs==DFV2_ROM_SEG && f.ip==DFV2_CRITICAL+2) {
        enum dfv2_outcome r=dos_unified_critical(&U,&f,f.ax&255);
        if (r!=DFV2_RESUME || !suspended) { fail("critical continuation refused"); return 1; }
        /* restore the suspended gate frame exactly; resume after its INT FE */
        const struct saved_regs *v=&saved_cpu_regs;
        E->x86.R_EAX=v->eax; E->x86.R_EBX=v->ebx; E->x86.R_ECX=v->ecx; E->x86.R_EDX=v->edx;
        E->x86.R_ESI=v->esi; E->x86.R_EDI=v->edi; E->x86.R_EBP=v->ebp; E->x86.R_EFLG=v->eflags;
        x86emu_set_seg_register(E,E->x86.seg+R_DS_INDEX,v->ds);
        x86emu_set_seg_register(E,E->x86.seg+R_ES_INDEX,v->es);
        set_ss_sp(v->ss,v->sp);
        set_cs_ip(DFV2_ROM_SEG,suspended_ip); suspended=false;
        return 1;
    }
    enum dfv2_outcome r=dos_unified_gate(&U,&f);
    if (U.bind_refusal && !refusal_logged) {
        /* As dos_file_vm.c: a refused activation without /LOCAL stops the VM
         * at this lifecycle gate call with one record (f3-57 Amendment 1). */
        char token[64]; refusal_logged=true;
#ifdef H57_IFS
        const char *detail=IFS.why;
#else
        const char *detail=0;
#endif
        if (dos_unified_activation_stop(&U,detail,token,sizeof(token))) {
            snprintf(vm_record,sizeof(vm_record),"[dosvm] unified_activation_refused reason=%s",token);
            vm_stopped=true; x86emu_stop(E); return 1;
        }
    }
    if (getenv("H56_TRACE"))       /* diagnostics: every gate call and its outcome */
    {
        uint32_t rec=((uint32_t)U.layout.driver_seg<<4)+U.layout.call_off;
        fprintf(stderr,"GATE %04x:%04x ss:sp=%04x:%04x ax=%04x -> %d why=%s record ax=%04x status=%04x retry=%04x flags=%04x\n",
                f.cs,f.ip,f.ss,f.sp,f.ax,r,U.why ? U.why : "-",rw(rec>>4,(rec&15)+0x12),rw(rec>>4,(rec&15)+0x0e),
                rw(rec>>4,(rec&15)+0x2a),rw(rec>>4,(rec&15)+0x10));
    }
    if (r==DFV2_REFUSED) refusals_seen++;
    if (r==DFV2_SUSPEND) {
        uint16_t ss,sp;
        if (!dos_unified_callback_stack(&U,&ss,&sp) || ss!=f.ss || sp!=f.sp) { fail("suspend stack mismatch"); return 1; }
        saved_cpu_regs=(struct saved_regs){E->x86.R_EAX,E->x86.R_EBX,E->x86.R_ECX,E->x86.R_EDX,E->x86.R_ESI,
            E->x86.R_EDI,E->x86.R_EBP,E->x86.R_EFLG,E->x86.R_DS,E->x86.R_ES,E->x86.R_SS,E->x86.R_SP};
        suspended=true; suspended_ip=f.ip;
        /* INT 24h callback on the driver stack, returning to the C2 gate. */
        uint32_t base=(uint32_t)ss<<4; sp=(uint16_t)(sp-6);
        x86emu_write_word(E,base+sp,DFV2_CRITICAL); x86emu_write_word(E,base+sp+2,DFV2_ROM_SEG);
        x86emu_write_word(E,base+sp+4,(uint16_t)E->x86.R_EFLG);
        set_ss_sp(ss,sp);
        E->x86.R_AX=U.critical.ax; E->x86.R_DI=U.critical.di; E->x86.R_BP=U.critical.bp; E->x86.R_SI=U.critical.si;
        E->x86.R_EFLG&=~0x300u;
        set_cs_ip(rw(0,0x24*4+2),rw(0,0x24*4));
    }
    return 1;
}

static int intr(x86emu_t *e,u8 n,unsigned type) {
    (void)e;
    if (type&INTR_TYPE_FAULT) { faults++; fail("CPU exception"); return 1; }
    if (n==0xfe) return gate();
    if (E->x86.R_CS==0xf000 && E->x86.R_IP>=BIOS_STUBS && E->x86.R_IP<BIOS_STUBS+0x800) return bios(n);
    if (n==0x2f && opt.host) {
        struct dos_file_frame f=live_frame();
        if (dos_unified_discover(&f)) {
            E->x86.R_AX=f.ax; E->x86.R_CX=f.cx; E->x86.R_DI=f.di;
            x86emu_set_seg_register(E,E->x86.seg+R_ES_INDEX,f.es); return 1;
        }
    }
    return 0;
}
static unsigned memio(x86emu_t *e,u32 addr,u32 *val,unsigned type) {
    unsigned kind=type&~0xffu;
    if (kind==X86EMU_MEMIO_O) {
        if (addr==0xe9) {
            if (e9n<sizeof(e9)-1) e9[e9n++]=(char)*val;
            if (opt.stop_on && *val=='\n' && strstr(e9,opt.stop_on)) { stopped=0; x86emu_stop(E); }
        }
        else if (addr==0xea) { stopped=(int)(*val&255); x86emu_stop(E); }
#ifdef H57_IFS
        else if (addr==0xeb) ifs_control(*val&255);
#endif
        else if (!(addr>=0x3f8 && addr<=0x3ff)) ports_unknown++;
        return 0;
    }
    if (kind==X86EMU_MEMIO_I) {
        *val=addr==0x3fd ? 0x60 : 0xffffffffu;
        if (!(addr>=0x3f8 && addr<=0x3ff)) ports_unknown++;
        return 0;
    }
    return default_memio(e,addr,val,type);
}

/* libx86emu's real-mode PUSHF masks FLAGS bits 12-15. A 386 pushes IOPL
 * and NT (Intel SDM Vol. 2, PUSHF), which K2043's query_cpu (cpu.asm)
 * relies on for its 386 check. Model only the unprefixed PUSHF. */
static int code_check(x86emu_t *e) {
    uint32_t pc=((uint32_t)e->x86.R_CS<<4)+e->x86.R_IP;
    if (x86emu_read_byte_noperm(e,pc)==0x9c) {
        e->x86.R_SP=(uint16_t)(e->x86.R_SP-2);
        x86emu_write_word(e,((uint32_t)e->x86.R_SS<<4)+e->x86.R_SP,(e->x86.R_EFLG&0x7fd5u)|2u);
        e->x86.R_IP=(uint16_t)(e->x86.R_IP+1);
    }
    return 0;
}
static size_t load(const char *path,uint8_t **out,size_t bound) {
    FILE *f=fopen(path,"rb"); if (!f) return 0;
    *out=malloc(bound); if (!*out) { fclose(f); return 0; }
    size_t n=fread(*out,1,bound,f); int extra=fgetc(f); fclose(f);
    return extra==EOF ? n : 0;
}
static void dump_text(const char *tag,const char *b,size_t n) {
    printf("%s<<", tag); for (size_t i=0;i<n;i++) { char c=b[i]; if (c=='\r') continue; putchar(c=='\n' ? '\n' : (c>=32 && c<127) ? c : '.'); } printf(">>\n");
}

int main(int argc,char **argv) {
    if (argc<2) { fprintf(stderr,"usage: %s IMAGE [options]\n",argv[0]); return 2; }
    unsigned long long budget=400000000ull;
    for (int i=2;i<argc;i++) {
        if (!strcmp(argv[i],"--nohost")) opt.host=false;
        else if (!strncmp(argv[i],"--desc=",7) || !strncmp(argv[i],"--rom=",6)) {
            /* ROM byte patch: --desc= is relative to the descriptor, --rom= to F000:0 */
            unsigned o,b; bool desc=argv[i][2]=='d';
            if (opt.patches==16 || sscanf(argv[i]+(desc ? 7 : 6),"%x:%x",&o,&b)!=2) return 2;
            opt.patch_at[opt.patches]=(uint16_t)(desc ? DFV2_DESCRIPTOR+o : o); opt.patch_byte[opt.patches++]=(uint8_t)b;
        }
        else if (!strncmp(argv[i],"--stop-on=",10)) opt.stop_on=argv[i]+10;
        else if (!strncmp(argv[i],"--fail=",7)) { if (sscanf(argv[i]+7,"%u:%u",&L.fail_event,&L.fail_at)!=2) return 2; }
        else if (!strncmp(argv[i],"--budget=",9)) budget=strtoull(argv[i]+9,0,10);
        else if (!strcmp(argv[i],"--claims")) L.claims=true;
#ifdef H57_IFS
        else if (!strncmp(argv[i],"--d=",4)) { dimage_bytes=load(argv[i]+4,&dimage,64u<<20); if (!dimage_bytes) return 3; }
        else if (!strncmp(argv[i],"--ifs-fail=",11)) ifs_fail=argv[i]+11;
#endif
        else return 2;
    }
    L.fail_event=L.fail_event ? L.fail_event : (L.fail_at ? 0 : 99);
    image_bytes=load(argv[1],&image,64u<<20);
    if (!image_bytes || image_bytes<512 || image[510]!=0x55 || image[511]!=0xaa) { fprintf(stderr,"bad image\n"); return 3; }
    E=x86emu_new(X86EMU_PERM_RWX,X86EMU_PERM_RW); if (!E) return 4;
    default_memio=x86emu_set_memio_handler(E,memio);
    x86emu_set_intr_handler(E,intr);
    x86emu_set_code_handler(E,code_check);
    /* IVT: BIOS vectors -> ROM stubs "INT n; RETF 2"; everything else IRET. */
    x86emu_write_byte(E,0xf0000+IRET_STUB,0xcf);
    for (unsigned n=0;n<256;n++) { x86emu_write_word(E,n*4,IRET_STUB); x86emu_write_word(E,n*4+2,0xf000); }
    static const uint8_t bios_vectors[]={0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a};
    for (unsigned i=0;i<sizeof(bios_vectors);i++) {
        unsigned n=bios_vectors[i],at=0xf0000+BIOS_STUBS+8*n;
        x86emu_write_byte(E,at,0xcd); x86emu_write_byte(E,at+1,n); x86emu_write_byte(E,at+2,0xca);
        x86emu_write_byte(E,at+3,2); x86emu_write_byte(E,at+4,0);
        x86emu_write_word(E,n*4,(uint16_t)(BIOS_STUBS+8*n));
    }
    x86emu_write_word(E,0x1e*4,0); x86emu_write_word(E,0x1e*4+2,0);
    x86emu_write_word(E,0x410,0x0020); x86emu_write_word(E,0x413,640); x86emu_write_byte(E,0x449,3);
    x86emu_write_word(E,0x44a,80); x86emu_write_byte(E,0x484,24); x86emu_write_byte(E,0x475,1);
    x86emu_write_word(E,0x41a,0x1e); x86emu_write_word(E,0x41c,0x1e);
    /* DFV2 ROM, written as the VMM does for the unified profile only. */
    static uint8_t page[4096];
    uint32_t generation=0x5600a001u;
    dos_unified_rom(page,generation);
    for (unsigned i=0;i<opt.patches;i++) {
        if (opt.patch_at[i]<DFV2_DIRECT || opt.patch_at[i]>=DFV2_END) return 2;
        page[opt.patch_at[i]-0x1000u]=opt.patch_byte[i];
    }
    for (unsigned i=0x1c00;i<0x1c40;i++) x86emu_write_byte(E,0xf0000+i,page[i-0x1000]);
    struct dos_file_ops ops={.check=m_check,.read=m_read,.write=m_write,.release=m_release};
    if (L.claims) { L.claim[0x61>>3]|=(uint8_t)(1u<<(0x61&7)); L.claim[0x6e>>3]|=(uint8_t)(1u<<(0x6e&7)); L.claim[0x6f>>3]|=(uint8_t)(1u<<(0x6f&7)); }
    struct dfv2_backend backend={b_prepare,b_commit,b_request,b_complete,b_destroy,L.claims ? L.claim : 0,0};
#ifdef H57_IFS
    if (cache_init(&CACHE,1u<<20)) return 4;
    struct dos_file_ops ifs_ops=ops; ifs_ops.wall=h_wall; ifs_ops.monitor=h_wall; ifs_ops.pause=h_pause; ifs_ops.stopped=h_stopped;
    struct dfv2_ifs_host host={h_mount,h_unmount,0};
    dfv2_ifs_init(&IFS,&U,&ifs_ops,&host,0x57,generation);
    dfv2_ifs_backend(&IFS,&backend);
    (void)b_prepare; (void)b_commit; (void)b_request; (void)b_complete; (void)b_destroy;
#endif
    dos_unified_init(&U,generation,&ops,&backend);
    for (unsigned i=0;i<512;i++) x86emu_write_byte(E,0x7c00+i,image[i]);
    x86emu_set_seg_register(E,E->x86.seg+R_CS_INDEX,0);
    E->x86.R_IP=0x7c00; E->x86.R_SP=0x7c00; E->x86.R_DL=0x80; E->x86.R_EFLG=0x202;
    for (unsigned long long done=0;stopped<0 && !failure && !vm_stopped && done<budget;) {
        E->max_instr=E->x86.R_TSC+100000;
        x86emu_run(E,X86EMU_RUN_MAX_INSTR);
        x86emu_write_dword(E,0x46c,(uint32_t)(E->x86.R_TSC/20000));
        if (stopped<0 && !failure && !vm_stopped && E->x86.R_TSC<E->max_instr) {
            uint32_t pc=((uint32_t)E->x86.R_CS<<4)+E->x86.R_IP;
            if (x86emu_read_byte(E,pc-1)==0xf4) { fail("guest HLT"); break; }
        }
        done=E->x86.R_TSC;
    }
    if (stopped<0 && !failure && !vm_stopped) failure="instruction budget exhausted";
    printf("H56 VMSTOP stopped=%u record=%s\n",vm_stopped,vm_stopped ? vm_record : "none");
    dump_text("E9",e9,e9n); dump_text("CON",con,conn);
    printf("H56 CPU instructions=%llu stop=%d failure=%s faults=%u bios_unhandled=%u ports_unknown=%u\n",
           (unsigned long long)E->x86.R_TSC,stopped,failure ? failure : "none",faults,bios_unhandled,ports_unknown);
    printf("H56 WHY %s\n",U.why ? U.why : "none");
    printf("H56 NATIVE registered=%u bound=%u dead=%u pending=%u selected=%04x owning=%04x serial=%u refused=%u chained=%u done=%u suspended=%u completed=%u events=%u binds=%u\n",
           U.registered,U.bound,U.dead,U.pending,U.selected,U.owning,U.serial,U.stats.refused,U.stats.chained,U.stats.done,
           U.stats.suspended,U.stats.completed,U.stats.events,U.stats.binds);
    printf("H56 WRITES call=%u claim=%u other=%u\n",writes_call,writes_claim,writes_other);
    printf("H56 GATES direct=%u local=%u event=%u critical=%u refusals=%u\n",gate_calls[0],gate_calls[1],gate_calls[2],gate_calls[3],refusals_seen);
    printf("H56 LAYOUT driver=%04x:%04x data=%04x:%04x text=%04x:%04x lol=%04x:%04x sda=%04x:%04x sft=%u blocks=%u cds=%04x:%04x/%u root=%04x\n",
           U.layout.driver_seg,U.layout.driver_bytes,U.layout.data_seg,U.layout.data_bytes,U.layout.text_seg,U.layout.text_bytes,
           U.layout.lol.seg,U.layout.lol.off,U.layout.sda.seg,U.layout.sda.off,U.layout.sft_count,U.layout.blocks,
           U.layout.cds.seg,U.layout.cds.off,U.layout.cds_count,U.layout.root);
    printf("H56 SNAPSHOT country=");
    for (unsigned i=0;i<41;i++) printf("%02X",U.layout.country[i]);
    printf(" upcase=");
    for (unsigned i=0;i<128;i++) printf("%02X",U.layout.upcase[2+i]);
    printf("\n");
    for (unsigned i=0;i<DFV2_PROCESSES;i++) if (U.process[i].used)
        printf("H56 PROCESS psp=%04x parent=%04x kind=%02x provisional=%u resident=%u\n",U.process[i].psp,U.process[i].parent,
               U.process[i].kind,U.process[i].provisional,U.process[i].resident);
    int live=0; for (unsigned i=0;i<64;i++) if (L.e[i].state) { live++; printf("H56 LEDGER psp=%04x state=%u\n",L.e[i].psp,L.e[i].state); }
    printf("H56 BACKEND runtime=%d live=%d prepared=%d committed=%d aborted=%d released=%d bad=%d direct=%u local=%u completions=%u seen=%u,%u,%u,%u,%u,%u,%u,%u\n",
           L.runtime,live,L.prepared,L.committed,L.aborted,L.released,L.bad,L.requests[1],L.requests[2],L.completions,
           L.seen[0],L.seen[1],L.seen[2],L.seen[3],L.seen[4],L.seen[5],L.seen[6],L.seen[7]);
#ifdef H57_IFS
    printf("H57 IFS active=%u owned=%x requests=%u owned_requests=%u chained=%u failed=%u critical=%u retries=%u expired=%u forged=%u reclaimed=%u opens=%u closes=%u reads=%u writes=%u finds=%u read_bytes=%llu written_bytes=%llu activations=%u refusals=%u why=%s\n",
           IFS.active,IFS.owned,IFS.stats.requests,IFS.stats.owned,IFS.stats.chained,IFS.stats.failed,IFS.stats.critical,
           IFS.stats.retries,IFS.stats.expired,IFS.stats.forged,IFS.stats.reclaimed,IFS.stats.opens,IFS.stats.closes,
           IFS.stats.reads,IFS.stats.writes,IFS.stats.finds,(unsigned long long)IFS.stats.read_bytes,
           (unsigned long long)IFS.stats.written_bytes,IFS.stats.activations,IFS.stats.refusals,IFS.why ? IFS.why : "none");
    printf("H57 HELD");
    for (unsigned i=0;i<held_n;i++) printf(" %u",held_samples[i]);
    printf(" final=%u\n",dfv2_ifs_held(&IFS));
    printf("H57 IO int13_before=%u int13_after=%u faults_hit=%u mounts=%u unmounts=%u critical_status=%u adapter_retries=%u\n",
           int13_before,int13_after,faults_hit,mounts,unmounts,U.stats.critical,U.stats.retries);
#endif
    /* VM stop: event 8, then a late completion must be refused. */
    dos_unified_destroy(&U);
    struct dos_file_frame late={.cs=DFV2_ROM_SEG,.ip=DFV2_CRITICAL+2,.ss=U.layout.driver_seg,.sp=(uint16_t)(U.layout.stack_off+U.layout.stack_bytes-4)};
    enum dfv2_outcome late_r=dos_unified_critical(&U,&late,1);
    live=0; for (unsigned i=0;i<64;i++) if (L.e[i].state) live++;
    printf("H56 DESTROY dead=%u live=%d late_completion=%s prepared=%d bad=%d\n",U.dead,live,late_r==DFV2_REFUSED ? "refused" : "ACCEPTED",L.prepared,L.bad);
#ifdef H57_IFS
    printf("H57 DESTROY held=%u mounted=%u active=%u unmounts=%u\n",dfv2_ifs_held(&IFS),IFS.mounted,IFS.active,unmounts);
    cache_destroy(&CACHE); free(dimage);
#endif
    for (unsigned i=0;i<TOTAL_SECTORS;i++) free(overlay[i]);
    free(image); x86emu_done(E);
    return failure ? 1 : 0;
}
