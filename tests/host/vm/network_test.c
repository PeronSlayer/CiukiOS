/* Production V86 adapter with guest-memory and virtual allocator seams.
 * SPDX-License-Identifier: GPL-2.0-only */
#define PORT_TEST_MAIN unused_port_suite_main
#include "port_test.c"
#include "../../../src/kernel/vm/dosvm_network.c"
static struct dosvm guest[3];
static uint8_t low[2][0xa0000];
static unsigned registered_ports[3],preemptions;
static int port_error;
static unsigned wire_count;
static uint8_t wire_copy[1514];
static int physical_send(struct netif *d,const uint8_t *p,unsigned bytes)
{ CHECK(d==&devices[0] && bytes>=60); wire_count++; memcpy(wire_copy,p,bytes); return 0; }
static const struct netif_driver physical_driver={.transmit=physical_send};
void kpreempt_point(void) { preemptions++; }
bool dosvm_range(const struct dosvm *v,uint32_t at,unsigned n,bool write)
{ (void)write; return v->info.key.id && v->info.key.id<=2 && at<sizeof(low[0]) && n<=sizeof(low[0])-at; }
int dosvm_read(struct dosvm *v,uint32_t at,void *out,unsigned n)
{ if(!dosvm_range(v,at,n,false)) return -EFAULT; memcpy(out,low[v->info.key.id-1]+at,n); return 0; }
int dosvm_write(struct dosvm *v,uint32_t at,const void *data,unsigned n)
{ if(!dosvm_range(v,at,n,true)) return -EFAULT; memcpy(low[v->info.key.id-1]+at,data,n); return 0; }
bool dosvm_irq_raise(struct dosvm_key key,unsigned irq)
{
    CHECK(key.id && key.id<=2 && irq==3);
    struct dosvm *v=&guest[key.id-1];
    if(v->info.key.generation!=key.generation) return false;
    v->monitor.pic[0].irr|=1u<<irq; return true;
}
int dosvm_port_register(struct dosvm_key key,uint16_t first,uint16_t last,dosvm_port_handler handler,void *arg)
{
    CHECK(key.id && key.id<=2 && first==0x300 && last==0x31f && handler==virtual_port && arg);
    if(port_error) return port_error;
    if(registered_ports[key.id-1]) return -16;
    registered_ports[key.id-1]++; return 0;
}
int dosvm_port_unregister(struct dosvm_key key,uint16_t first)
{ CHECK(key.id && key.id<=2 && first==0x300); registered_ports[key.id-1]=0; return 0; }
static void put16(uint8_t *p,uint16_t x) { p[0]=x; p[1]=x>>8; }
static void command(struct dosvm *v,struct v86_frame *f,unsigned fn,unsigned at)
{
    f->tf.ebx=fn; f->tf.esi=at&15; f->es=at>>4;
    CHECK(!dosvm_network_interrupt(v,f,0x7a));
}
int main(void)
{
    uint8_t id[16]={42}; static struct bridge_pool pool; struct net_bridge_port port={0};
    struct netif *d=&devices[0]; d->generation=77; d->ready=d->running=true; d->link=NET_LINK_UP;
    d->driver=&physical_driver;
    kmutex_init(&registration_mutex); CHECK(!net_bridge_port_open(&port,&pool,d,1,id,0,0,mode));
    for(unsigned i=0;i<3;i++) {
        guest[i].info.key=(struct dosvm_key){i+1,100+i}; guest[i].info.state=DOSVM_RUNNABLE;
        guest[i].frames[0xf1]=HOST_PHYS+i*PAGE_SIZE; guest[i].audio.sb.irq=5;
        guest[i].monitor.vif=true;
    }
    struct dosvm *v=&guest[0]; struct v86_frame f={0};
    CHECK(!dosvm_network_launch(v,CIUKI_DOSVM_NETWORK_OFF));
    CHECK(dosvm_network_launch(v,CIUKI_DOSVM_NETWORK_IPX)==-19 && dosvm_network_launch(v,2)==-EINVAL);
    CHECK(!dosvm_network_bind_port(&port));
    CHECK(dosvm_network_interrupt(v,&f,0x2f)==-ENOSYS);
    v->audio.sb.irq=3; CHECK(dosvm_network_attach(v,&port)==-EINVAL); v->audio.sb.irq=5;
    port_error=-16; CHECK(dosvm_network_attach(v,&port)==-16 && !port.bridge.count && !network(v));
    port_error=0; CHECK(!dosvm_network_launch(v,CIUKI_DOSVM_NETWORK_IPX)); CHECK(registered_ports[0]==1);
    CHECK(dosvm_network_bind_port(0)==-16);
    CHECK(!dosvm_network_attach(&guest[1],&port));
    CHECK(dosvm_network_attach(&guest[2],&port)==-ENOSPC);
    struct dosvm_network *n=network(v); CHECK(n && !memcmp(P2V(v->frames[0xf1])+0xc00,"\xcd\x7a\xcb",3));
    f.tf.eax=0x7a00; CHECK(!dosvm_network_interrupt(v,&f,0x2f));
    CHECK((f.tf.eax&255)==255 && f.es==0xf000 && f.tf.edi==DOSVM_IPX_ENTRY);
    f.tf.edx=0x9c86; f.tf.eax=0; command(v,&f,0,0); CHECK(!(f.tf.eax&255));
    uint8_t *e=low[0]+0x2000; put16(e+10,0x9c86); put16(e+34,1);
    put16(e+4,0x30); put16(e+6,0x5000); put16(e+36,0); put16(e+38,0x600); put16(e+40,100);
    command(v,&f,4,0x2000); CHECK(e[8]==0xfe);
    uint8_t frame[64]={0}; memcpy(frame,n->ne.assigned,6); frame[6]=2; frame[7]=99;
    frame[12]=0x81; frame[13]=0x37; frame[14]=frame[15]=255; frame[17]=50;
    memcpy(frame+24,frame,6); memcpy(frame+36,frame+6,6); frame[30]=0x86; frame[31]=0x9c;
    frame_tap(d,frame,64,&port); dosvm_network_service(v,1000);
    CHECK(e[8]==0 && e[9]==0 && !memcmp(low[0]+0x6000,frame+14,50));
    f=(struct v86_frame){0}; f.tf.cs=0x123; f.tf.eip=0x456; f.tf.user_ss=0x700; f.tf.user_esp=0x100;
    f.tf.eax=0x12340000; f.tf.eflags=0x400; f.es=0x234; f.tf.esi=0x345;
    struct v86_frame saved=f;
    v->monitor.vif=false; CHECK(!dosvm_network_dispatch(v,&f));
    v->monitor.vif=true; v->monitor.shadow=true; CHECK(!dosvm_network_dispatch(v,&f));
    v->monitor.shadow=false; v->mouse.active=true; CHECK(!dosvm_network_dispatch(v,&f));
    v->mouse.active=false; v->frame_pm=true; CHECK(!dosvm_network_dispatch(v,&f));
    v->frame_pm=false; v->dpmi.depth=1; CHECK(!dosvm_network_dispatch(v,&f)); v->dpmi.depth=0;
    CHECK(dosvm_network_dispatch(v,&f)==1 && n->active && !v->monitor.vif);
    CHECK(f.tf.cs==0x5000 && f.tf.eip==0x30 && f.es==0x200 && f.tf.esi==0x2000%16);
    CHECK(f.tf.eax==0x123400ff && !(f.tf.eflags&0x400) && f.tf.user_esp==0xfc);
    CHECK(low[0][0x70fc]==(DOSVM_IPX_RETURN&255) && low[0][0x70ff]==0xf0);
    CHECK(dosvm_network_interrupt(v,&f,0xfc)==-V86_EPERM);
    f.tf.cs=0xf000; f.tf.eip=DOSVM_IPX_RETURN+2; f.tf.user_esp=0x100;
    CHECK(!dosvm_network_interrupt(v,&f,0xfc) && !memcmp(&f,&saved,sizeof(f)) && v->monitor.vif && !n->active);
    uint32_t value=0; CHECK(!virtual_port(v,0x300,1,false,&value,n)); CHECK(value==0x21);
    value=0x7f; CHECK(!virtual_port(v,0x30f,1,true,&value,n)); dosvm_ne2000_overflow(&n->ne);
    CHECK(v->monitor.pic[0].irr&8); CHECK(!virtual_port(v,0x31f,1,false,&value,n)); CHECK(!(v->monitor.pic[0].irr&8));
    CHECK(virtual_port(v,0x310,4,false,&value,n)==-V86_EPERM);
    uint32_t ticket=0; CHECK(guest_pin(n,0x9fff0,32,true,&ticket)==-EFAULT);
    CHECK(guest_pin(n,0xa0000,1,false,&ticket)==-EFAULT);
    CHECK(!guest_pin(n,0x5000,16,false,&ticket)); CHECK(guest_write(n,0x5000,"x",1)==-EFAULT); guest_unpin(n,ticket);
    /* Two adapters, actual type tap, physical TX copy, and a LAN peer reply. */
    uint8_t *send_ecb=low[0]+0x3000;
    put16(send_ecb+10,0x9c86); put16(send_ecb+34,1); put16(send_ecb+38,0x800); put16(send_ecb+40,50);
    memset(send_ecb+28,255,6); memcpy(low[0]+0x8000,frame+14,50); memset(low[0]+0x8000+10,255,6);
    struct v86_frame peer_f={0}; peer_f.tf.edx=0x9c86; command(&guest[1],&peer_f,0,0);
    uint8_t *peer_ecb=low[1]+0x2000; put16(peer_ecb+10,0x9c86); put16(peer_ecb+34,1);
    put16(peer_ecb+38,0x600); put16(peer_ecb+40,100); command(&guest[1],&peer_f,4,0x2000);
    command(v,&f,3,0x3000); dosvm_network_service(v,2000); dosvm_network_service(&guest[1],2000);
    CHECK(wire_count==1 && !send_ecb[8] && !send_ecb[9] && !peer_ecb[8] && !peer_ecb[9]);
    CHECK(!memcmp(wire_copy+6,n->ne.assigned,6) && !memcmp(low[1]+0x6000,wire_copy+14,50));
    memcpy(wire_copy,n->ne.assigned,6); memcpy(wire_copy+24,n->ne.assigned,6);
    memset(wire_copy+6,0x22,6); memcpy(wire_copy+36,wire_copy+6,6);
    put16(e+4,0); put16(e+6,0); command(v,&f,4,0x2000);
    frame_tap(d,wire_copy,64,&port); dosvm_network_service(v,2000);
    CHECK(!e[8] && !e[9] && !memcmp(low[0]+0x6000,wire_copy+14,50) && wire_count==1);
    host_flags=0; dosvm_network_service(v,3000); CHECK(n->host_ms==2000); host_flags=0x200;
    command(v,&f,4,0x2000); d->link=NET_LINK_DOWN; dosvm_network_service(v,3000);
    CHECK(!e[8] && e[9]==0xfe && n->ipx.ticks==interval_ticks(3000) && !pool.used);
    CHECK(net_bridge_port_acquire(&port)); CHECK(dosvm_network_detach(v)==-16); net_bridge_port_release(&port);
    CHECK(!dosvm_network_detach(v) && !network(v) && !registered_ports[0]);
    CHECK(dosvm_network_dispatch(v,&f)==0); CHECK(!dosvm_network_detach(&guest[1]));
    CHECK(!dosvm_network_bind_port(0)); CHECK(!net_bridge_port_close(&port) && !pool.used);
    printf("network adapter: %u checks passed, %u safe points\n",checks,preemptions); return 0;
}
