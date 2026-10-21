/* Reconstructed protocol/API fixtures, no game code or payload.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include "../../../src/kernel/vm/dosvm_ipx.h"
struct memory_pin { uint32_t at; unsigned n; bool write,used; };
struct host_vm {
    struct dosvm_ipx ipx;
    struct bridge_key key;
    uint8_t memory[0x110000],mac[6];
    struct memory_pin pins[2048];
    unsigned live,pin_calls,fail_pin,callbacks,yields;
    bool safe;
    struct dosvm_ipx_upcall last;
};
static struct host_vm a,b;
static struct net_bridge bridge;
static struct bridge_pool pool;
static unsigned wires;
static uint64_t now;
static int wire_error;
static uint8_t wire_frame[1514];
static int wire(void *arg,const uint8_t *p,unsigned n)
{ (void)arg; memcpy(wire_frame,p,n); wires++; return wire_error; }
static int promisc(void *arg,bool enabled) { (void)arg; (void)enabled; return 0; }
static int mem_pin(void *arg,uint32_t at,unsigned n,bool write,uint32_t *out)
{
    struct host_vm *v=arg;
    if (++v->pin_calls==v->fail_pin || at>=sizeof(v->memory) || n>sizeof(v->memory)-at) return -EFAULT;
    for(unsigned i=0;i<2048;i++) if(!v->pins[i].used) {
        v->pins[i]=(struct memory_pin){at,n,write,true}; *out=i+1; v->live++; return 0;
    }
    return -ENOSPC;
}
static void mem_unpin(void *arg,uint32_t token)
{ struct host_vm *v=arg; CHECK(token && token<=2048 && v->pins[token-1].used); v->pins[token-1].used=false; v->live--; }
static bool pinned(struct host_vm *v,uint32_t at,unsigned n,bool write)
{
    for(unsigned i=0;i<2048;i++) if(v->pins[i].used && (!write || v->pins[i].write) && at>=v->pins[i].at &&
        n<=v->pins[i].n && at-v->pins[i].at<=v->pins[i].n-n) return true;
    return false;
}
static int mem_read(void *arg,uint32_t at,void *out,unsigned n)
{ struct host_vm *v=arg; CHECK(pinned(v,at,n,false)); memcpy(out,v->memory+at,n); return 0; }
static int mem_write(void *arg,uint32_t at,const void *p,unsigned n)
{ struct host_vm *v=arg; CHECK(pinned(v,at,n,true)); memcpy(v->memory+at,p,n); return 0; }
static int send(void *arg,const uint8_t *p,unsigned n)
{ struct host_vm *v=arg; return net_bridge_send(&bridge,v->key,p,n,now); }
static void relinquish(void *arg) { struct host_vm *v=arg; v->yields++; }
static int upcall(void *arg,const struct dosvm_ipx_upcall *call)
{
    struct host_vm *v=arg; CHECK(call->generation==v->key.generation);
    if(!v->safe) return 0;
    v->callbacks++; v->last=*call; return 1;
}
static void put16(uint8_t *p,unsigned n) { p[0]=n; p[1]=n>>8; }
static struct dosvm_ipx_regs call(struct host_vm *v,unsigned fn,unsigned ax,unsigned dx,unsigned at)
{
    struct dosvm_ipx_regs r={.ax=ax,.bx=fn,.dx=dx,.es=at>>4,.si=at&15};
    CHECK(dosvm_ipx_interrupt(&v->ipx,0x7a,&r)==1); return r;
}
static void host_init(struct host_vm *v,unsigned id,unsigned generation)
{
    memset(v,0,sizeof(*v)); v->key=(struct bridge_key){id,generation}; v->safe=true;
    CHECK(!net_bridge_enable(&bridge,v->key,now,v->mac));
    struct dosvm_ipx_ops ops={v,mem_pin,mem_unpin,mem_read,mem_write,send,relinquish,upcall};
    CHECK(!dosvm_ipx_init(&v->ipx,generation,v->mac,&ops));
}
static void ecb(struct host_vm *v,unsigned at,unsigned buffer,unsigned size,unsigned socket,bool esr)
{
    uint8_t *p=v->memory+at; memset(p,0,48); put16(p+10,((socket&255)<<8)|(socket>>8));
    put16(p+34,1); put16(p+36,buffer&15); put16(p+38,buffer>>4); put16(p+40,size);
    if(esr) { put16(p+4,0x30); put16(p+6,0x5000); }
}
static void send_ecb(struct host_vm *v,unsigned at,unsigned header,unsigned body,unsigned size,const uint8_t *to,unsigned found,int32_t tic)
{
    ecb(v,at,header,34,0x869c,false); uint8_t *e=v->memory+at;
    memcpy(e+28,to,6); put16(e+34,2); put16(e+42,body&15); put16(e+44,body>>4); put16(e+46,size);
    uint8_t frame[64]; ipx_frame(frame,v->mac,to,64); memcpy(v->memory+header,frame+14,30);
    uint8_t *p=v->memory+header+30; for(unsigned i=0;i<4;i++) p[i]=(uint8_t)((uint32_t)tic>>(8*i));
    memset(v->memory+body,0,size);
    /* DOOM setupdata_t: gameid, drone, nodesfound, nodeswanted (LE16). */
    put16(v->memory+body+4,found); put16(v->memory+body+6,2);
}
static void pump(void)
{
    dosvm_ipx_service(&a.ipx,now*182065/10000000); dosvm_ipx_service(&b.ipx,now*182065/10000000);
    net_bridge_step(&bridge); uint8_t frame[1514]; int n;
    n=net_bridge_receive(&bridge,a.key,frame,sizeof(frame)); if(n>0) CHECK(dosvm_ipx_receive(&a.ipx,frame,n)>=0);
    n=net_bridge_receive(&bridge,b.key,frame,sizeof(frame)); if(n>0) CHECK(dosvm_ipx_receive(&b.ipx,frame,n)>=0);
}
int main(void)
{
    const uint8_t id[16]={4,8,15,16,23,42}, all[6]={255,255,255,255,255,255};
    struct bridge_ops ops={0,wire,promisc}; CHECK(!net_bridge_init(&bridge,&pool,&ops,id,0,0,1,7));
    net_bridge_link(&bridge,true); host_init(&a,1,10); host_init(&b,2,20);
    struct dosvm_ipx_regs r={.ax=0x7a00}; CHECK(dosvm_ipx_interrupt(&a.ipx,0x2f,&r)==1);
    CHECK((r.ax&255)==255 && r.es==0xf000 && r.di==DOSVM_IPX_ENTRY);
    r=call(&a,0,0,0x9c86,0); CHECK((r.ax&255)==0 && r.dx==0x9c86);
    r=call(&b,0,0,0x9c86,0); CHECK((r.ax&255)==0);
    CHECK((call(&a,0,0,0x9c86,0).ax&255)==255);
    CHECK((call(&a,9,0,0,0x4000).ax&255)==0 && !memcmp(a.memory+0x4004,a.mac,6));
    for(unsigned i=0;i<4;i++) CHECK(a.memory[0x4000+i]==0);
    r=call(&a,0,0,0,0); CHECK((r.ax&255)==0 && r.dx==0x0040); call(&a,1,0,r.dx,0);
    /* DOOM IPXNET InitNetwork posts nine receive ECBs and one two-fragment sender. */
    for(unsigned i=0;i<9;i++) {
        ecb(&a,0x1000+i*0x80,0x8000+i*0x300,576,0x869c,false);
        ecb(&b,0x1000+i*0x80,0x8000+i*0x300,576,0x869c,false);
        CHECK(!(call(&a,4,0,0,0x1000+i*0x80).ax&255)); CHECK(!(call(&b,4,0,0,0x1000+i*0x80).ax&255));
    }
    /* LookForNodes: broadcast time=-1/found=1; learn sender; repost, then
     * broadcast found=2; both agree, followed by a unicast game packet. */
    send_ecb(&a,0x2000,0x5000,0x6000,12,all,1,-1); send_ecb(&b,0x2000,0x5000,0x6000,12,all,1,-1);
    call(&a,3,0,0,0x2000); call(&b,3,0,0,0x2000); pump();
    CHECK(wires==2 && a.memory[0x1008]==0 && b.memory[0x1008]==0);
    CHECK(!memcmp(a.memory+0x8000+22,b.mac,6) && !memcmp(b.memory+0x8000+22,a.mac,6));
    CHECK(a.memory[0x8000+30]==255 && a.memory[0x8000+38]==1 && a.memory[0x8000+40]==2);
    CHECK(a.memory[0x5002]==0 && a.memory[0x5003]==46); /* IPX fills length */
    call(&a,4,0,0,0x1000); call(&b,4,0,0,0x1000);
    now=1000; send_ecb(&a,0x2000,0x5000,0x6000,12,all,2,-1); send_ecb(&b,0x2000,0x5000,0x6000,12,all,2,-1);
    call(&a,3,0,0,0x2000); call(&b,3,0,0,0x2000); pump();
    CHECK(wires==4 && a.memory[0x8300+38]==2 && b.memory[0x8300+38]==2);
    send_ecb(&a,0x2000,0x5000,0x6000,12,b.mac,2,0); call(&a,3,0,0,0x2000); pump(); CHECK(wires==4);
    /* ESR publication is deferred until an eligible guest boundary. */
    ecb(&b,0x3000,0xb000,64,0x869c,true); call(&b,4,0,0,0x3000);
    uint8_t frame[64]; ipx_frame(frame,a.mac,b.mac,64); CHECK(dosvm_ipx_receive(&b.ipx,frame,64)==1);
    /* Older DOOM listeners still take FIFO priority. Close drains all then retry. */
    call(&b,1,0,0x9c86,0); CHECK(!b.live); call(&b,0,0,0x9c86,0);
    ecb(&b,0x3000,0xb000,64,0x869c,true); call(&b,4,0,0,0x3000);
    /* Mutate guest descriptors while queued: use the pinned snapshot only. */
    put16(b.memory+0x3000+38,0xc00); CHECK(dosvm_ipx_receive(&b.ipx,frame,64)==1);
    CHECK(!memcmp(b.memory+0xb000,frame+14,50) && b.memory[0xc000]==0 && b.memory[0x3008]==0);
    CHECK(!b.callbacks); b.safe=false; CHECK(dosvm_ipx_dispatch(&b.ipx)==0 && !b.callbacks);
    b.safe=true; CHECK(dosvm_ipx_dispatch(&b.ipx)==1 && b.callbacks==1 && b.last.al==255);
    CHECK(b.last.cs==0x5000 && b.last.ip==0x30 && b.last.es==0x300 && b.last.si==0 && !b.live);
    ecb(&b,0x3000,0xb000,64,0x869c,true); call(&b,4,0,0,0x3000);
    CHECK(!(call(&b,6,0,0,0x3000).ax&255) && b.memory[0x3009]==0xfc && !b.live);
    CHECK((call(&b,6,0,0,0x3000).ax&255)==255);
    ecb(&b,0x3000,0,0,0,true); call(&b,5,2,0,0x3000); CHECK(b.memory[0x3008]==0xfd);
    uint64_t tick=b.ipx.ticks; dosvm_ipx_service(&b.ipx,tick+1); CHECK(b.memory[0x3008]==0xfd);
    dosvm_ipx_service(&b.ipx,tick+2); CHECK(b.memory[0x3008]==0 && !b.memory[0x3009]);
    CHECK(dosvm_ipx_dispatch(&b.ipx)==1 && b.last.al==0 && !b.live);
    dosvm_ipx_service(&b.ipx,65537); CHECK(call(&b,8,0,0,0).ax==1);
    call(&b,10,0,0,0); CHECK(b.yields==1);
    /* pin failure unwinds every earlier pin and completes malformed ECB. */
    ecb(&b,0x3000,0xb000,64,0x869c,true); b.fail_pin=b.pin_calls+4;
    CHECK((call(&b,4,0,0,0x3000).ax&255)==0xfd && !b.live); b.fail_pin=0;
    put16(b.memory+0x3000+34,17); CHECK((call(&b,4,0,0,0x3000).ax&255)==0xfd && !b.live);
    ecb(&b,0x3000,0xb000,30,0x869c,false); call(&b,4,0,0,0x3000);
    CHECK(dosvm_ipx_receive(&b.ipx,frame,64)==-ENOSPC && b.memory[0x3009]==0xfd && !b.live);
    ecb(&b,0x3000,0xb000,64,0x869c,false); call(&b,4,0,0,0x3000); CHECK(b.live==2);
    CHECK((call(&b,4,0,0,0x3000).ax&255)==255 && b.live==2);
    dosvm_ipx_disconnect(&b.ipx); CHECK(!b.live && b.memory[0x3009]==0xfe);
    for(unsigned i=0;i<64;i++) {
        ecb(&b,0x10000+i*0x80,0x20000+i*0x100,64,0x869c,false);
        CHECK(!(call(&b,4,0,0,0x10000+i*0x80).ax&255));
    }
    ecb(&b,0x14000,0x24000,64,0x869c,false); CHECK((call(&b,4,0,0,0x14000).ax&255)==0xfe);
    call(&b,1,0,0x9c86,0); CHECK(!b.live);
    for(unsigned i=0;i<32;i++) CHECK(!(call(&b,0,0,0,0).ax&255));
    CHECK((call(&b,0,0,0,0).ax&255)==0xfe);
    uint32_t gen=b.ipx.generation; dosvm_ipx_destroy(&a.ipx); dosvm_ipx_destroy(&b.ipx);
    CHECK(!a.live && !b.live && b.ipx.generation!=gen && dosvm_ipx_dispatch(&b.ipx)==0);
    r=(struct dosvm_ipx_regs){.ax=0x7a00}; CHECK(!dosvm_ipx_interrupt(&b.ipx,0x2f,&r));
    net_bridge_shutdown(&bridge); CHECK(!pool.used);
    printf("ipx: %u checks passed; DOOM discovery and local unicast replay passed\n",checks); return 0;
}
