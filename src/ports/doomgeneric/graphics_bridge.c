/* CiukiOS cooperative graphics client. No physical display/input ownership. */
#include "graphics_bridge.h"
#include <dos.h>
#include <string.h>
#include <stddef.h>

#pragma pack(push,1)
typedef struct {
    uint32_t edi,esi,ebp,reserved,ebx,edx,ecx,eax;
    uint16_t flags,es,ds,fs,gs,ip,cs,sp,ss;
} rm_registers;
#pragma pack(pop)
typedef char rm_register_size_must_be_50[(sizeof(rm_registers)==50)?1:-1];
static volatile cg_descriptor *bridge;
static unsigned char *pixels,*palette;
static int bridge_error;
static unsigned last_dpmi_error,last_dpmi_service;

static unsigned long physical(cg_far p) {
    return ((unsigned long)p.segment << 4) + p.offset;
}
static int conventional(cg_far p,unsigned length) {
    unsigned long a=physical(p);
    return a>=0x500 && a+length<=0xA0000UL &&
           (unsigned long)p.offset+length<=65536UL;
}
static int rm_call(unsigned service,unsigned interrupt_number,rm_registers *r) {
    union REGS in,out;
    struct SREGS seg;
    memset(&in,0,sizeof(in));
    segread(&seg);
    seg.es=seg.ds;
    in.x.eax=service;
    in.x.ebx=interrupt_number;
    in.x.edi=(unsigned long)r;
    int386x(0x31,&in,&out,&seg);
    if(out.x.cflag) {
        last_dpmi_error=out.w.ax;
        last_dpmi_service=service;
    }
    return out.x.cflag ? 0 : 1;
}
static int far_call(cg_far entry,rm_registers *r) {
    /* DOS/4GW 1.97 implements real-mode INT simulation but rejects0301.
       Use the host-advertised multiplex transport for the same operations. */
    if(bridge && (bridge->flags&CG_FLAG_MULTIPLEX)) {
        r->ecx=r->eax;
        r->eax=(entry.offset==bridge->input.offset)?0xD74C:0xD74B;
        r->ebx=CG_DISCOVER_BX;
        r->flags=0x0202;
        return rm_call(0x0300,0x2f,r);
    }
    r->cs=entry.segment;
    r->ip=entry.offset;
    r->flags=0x0202;
    /* DPMI supplies an initial stack; the host immediately switches to its
       private presentation stack. No child-owned transfer allocations. */
    r->ss=r->sp=0;
    return rm_call(0x0301,0,r);
}
int cg_open(void) {
    rm_registers r;
    union REGS in,out;
    struct SREGS seg;
    cg_far location;
    bridge=0;pixels=palette=0;bridge_error=0;
    last_dpmi_error=last_dpmi_service=0;
    /* This Watcom DOS/4GW client requires flat DS base0 for conventional
       shared pointers. Refuse unsupported layouts instead of guessing. */
    segread(&seg);
    memset(&in,0,sizeof(in));
    in.x.eax=0x0006;in.x.ebx=seg.ds;
    int386(0x31,&in,&out);
    if(out.x.cflag || out.w.cx || out.w.dx) return 0;
    memset(&r,0,sizeof(r));
    r.eax=CG_DISCOVER_AX;r.ebx=CG_DISCOVER_BX;r.flags=0x0202;
    if(!rm_call(0x0300,0x2f,&r) || (r.eax&0xffff)!=CG_DISCOVER_REPLY) return 0;
    location.segment=r.es;location.offset=(uint16_t)r.edi;
    if(!conventional(location,sizeof(cg_descriptor))) return 0;
    bridge=(volatile cg_descriptor *)physical(location);
    if(bridge->version!=1 || bridge->size<48 || bridge->width!=320 ||
       bridge->height!=200 || bridge->pitch!=320 ||
       !conventional(bridge->pixels,64000) ||
       !conventional(bridge->palette,768) ||
       !conventional(bridge->present,1) || !conventional(bridge->input,1)) {
        bridge=0;return 0;
    }
    pixels=(unsigned char *)physical(bridge->pixels);
    palette=(unsigned char *)physical(bridge->palette);
    return 1;
}
int cg_present(int palette_changed) {
    rm_registers r;
    if(!bridge || bridge_error) return 0;
    memset(&r,0,sizeof(r));r.eax=palette_changed?1:0;
    return far_call(bridge->present,&r) && bridge->status==0;
}
int cg_key(unsigned *scan,int *pressed) {
    rm_registers r;
    if(!bridge || bridge_error) return -1;
    memset(&r,0,sizeof(r));
    if(!far_call(bridge->input,&r)) return -1;
    if(r.flags&1) return 0;
    *scan=(unsigned)(r.eax&0x017f);
    *pressed=(r.ebx&1)!=0;
    return 1;
}
uint32_t cg_ticks(void) {
    rm_registers r;
    if(!bridge || bridge_error) return 0;
    memset(&r,0,sizeof(r));r.eax=CG_PRESENT_POLL;
    if(!far_call(bridge->present,&r) || bridge->status) bridge_error=1;
    return bridge->milliseconds;
}
int cg_close_requested(void) { return bridge && (bridge->flags&CG_FLAG_CLOSE); }
int cg_failed(void) { return bridge_error; }
unsigned cg_error_code(void) { return last_dpmi_error; }
unsigned cg_error_service(void) { return last_dpmi_service; }
unsigned char *cg_pixels(void) { return pixels; }
unsigned char *cg_palette(void) { return palette; }
const volatile cg_descriptor *cg_info(void) { return bridge; }
