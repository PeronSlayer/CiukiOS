/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
static struct bridge_pool pool;
static struct net_bridge bridge;
static unsigned wire, on, off;
static int wire_error, promisc_error;
static uint8_t last[1514], sources[128][6];
static int transmit(void *arg,const uint8_t *p,unsigned n)
{ (void)arg; CHECK(n>=60 && n<=1514); memcpy(last,p,n); memcpy(sources[wire++%128],p+6,6); return wire_error; }
static int promiscuous(void *arg,bool enable)
{ (void)arg; if (promisc_error) return promisc_error; if (enable) on++; else off++; return 0; }
static struct bridge_key a={1,11},b={2,22};
static uint8_t ma[6],mb[6],physical[2][6]={{0x00,1,2,3,4,5},{0x00,1,2,3,4,6}};
static const uint8_t all[6]={255,255,255,255,255,255},peer[6]={2,3,4,5,6,7};
static void init(void)
{
    memset(&pool,0,sizeof(pool)); struct bridge_ops ops={0,transmit,promiscuous};
    uint8_t id[16]={1,2,3,4,5};
    CHECK(!net_bridge_init(&bridge,&pool,&ops,id,physical,2,4,99));
    net_bridge_link(&bridge,true);
    CHECK(!net_bridge_enable(&bridge,a,0,ma)); CHECK(!net_bridge_enable(&bridge,b,0,mb));
}
int main(void)
{
    uint8_t p[1514],out[1514],other[6]; init();
    CHECK(on==1 && off==0 && bridge.count==2); CHECK((ma[0]&3)==2 && memcmp(ma,mb,6));
    CHECK(net_bridge_enable(&bridge,(struct bridge_key){3,33},0,other)==-ENOSPC);
    CHECK(net_bridge_enable(&bridge,a,0,other)==-16);
    ipx_frame(p,ma,mb,64); CHECK(!net_bridge_send(&bridge,a,p,64,0));
    net_bridge_step(&bridge); CHECK(wire==0); CHECK(net_bridge_receive(&bridge,b,out,sizeof(out))==64);
    CHECK(!memcmp(p,out,64) && pool.used==0); CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==0);
    ipx_frame(p,ma,all,44); CHECK(!net_bridge_send(&bridge,a,p,44,0)); net_bridge_step(&bridge);
    CHECK(wire==1 && net_bridge_receive(&bridge,b,out,sizeof(out))==60);
    for (unsigned i=44;i<60;i++) CHECK(last[i]==0 && out[i]==0);
    CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==0);
    ipx_frame(p,peer,all,64); net_bridge_ingress(&bridge,4,99,p,64,0);
    CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==64); CHECK(net_bridge_receive(&bridge,b,out,sizeof(out))==64);
    CHECK(wire==1 && pool.used==0);
    net_bridge_ingress(&bridge,3,99,p,64,0); net_bridge_ingress(&bridge,4,98,p,64,0); CHECK(pool.used==0);
    ipx_frame(p,peer,physical[0],64); net_bridge_ingress(&bridge,4,99,p,64,0); CHECK(pool.used==0);
    ipx_frame(p,mb,all,64); CHECK(net_bridge_send(&bridge,a,p,64,0)==-13);
    net_bridge_ingress(&bridge,4,99,p,64,0); CHECK(pool.used==0);
    ipx_frame(p,ma,mb,64); p[13]=0; CHECK(net_bridge_send(&bridge,a,p,64,0)==-EINVAL);
    p[13]=0x37; p[17]=100; CHECK(net_bridge_send(&bridge,a,p,64,0)==-EINVAL);
    ipx_frame(p,ma,mb,64); p[36]^=1; CHECK(net_bridge_send(&bridge,a,p,64,0)==-EINVAL);
    net_bridge_shutdown(&bridge); CHECK(off==1 && pool.used==0);
    init(); wire=0;
    ipx_frame(p,ma,peer,64);
    for (unsigned i=0;i<32;i++) { CHECK(!net_bridge_send(&bridge,a,p,64,0)); net_bridge_step(&bridge); }
    CHECK(net_bridge_send(&bridge,a,p,64,0)==-11); CHECK(net_bridge_send(&bridge,a,p,64,3)==-11);
    CHECK(!net_bridge_send(&bridge,a,p,64,4)); net_bridge_step(&bridge);
    CHECK(net_bridge_send(&bridge,a,p,64,4)==-11);
    net_bridge_shutdown(&bridge); init(); wire=0;
    for (unsigned i=0;i<8;i++) CHECK(!net_bridge_send(&bridge,a,p,64,1000));
    CHECK(net_bridge_send(&bridge,a,p,64,1000)==-11); CHECK(pool.used==8);
    ipx_frame(p,mb,peer,64); CHECK(!net_bridge_send(&bridge,b,p,64,1000));
    net_bridge_step(&bridge); CHECK(wire==2 && memcmp(sources[0],sources[1],6));
    net_bridge_link(&bridge,false); CHECK(pool.used==0); CHECK(net_bridge_send(&bridge,a,p,64,1000)==-19);
    net_bridge_link(&bridge,true); ipx_frame(p,peer,ma,64);
    for (unsigned i=0;i<9;i++) net_bridge_ingress(&bridge,4,99,p,64,2000);
    CHECK(net_bridge_vm(&bridge,a)->rx.count==8 && net_bridge_vm(&bridge,a)->rx_overflows==1);
    CHECK(net_bridge_receive(&bridge,a,out,10)==-ENOSPC);
    net_bridge_disable(&bridge,a); CHECK(pool.used==0 && off==2);
    CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==-19);
    CHECK(!net_bridge_enable(&bridge,(struct bridge_key){1,12},3000,other)); CHECK(memcmp(ma,other,6));
    net_bridge_shutdown(&bridge); CHECK(pool.used==0 && off==3);
    /* All owners use this same 128-slot allocator; no hidden bridge fallback. */
    ipx_frame(p,peer,all,64);
    for (unsigned i=0;i<128;i++) CHECK(bridge_pool_copy(&pool,p,64,4,99)==(int)i);
    CHECK(bridge_pool_copy(&pool,p,64,4,99)==-ENOSPC);
    for (unsigned i=0;i<128;i++) bridge_pool_release(&pool,i);
    CHECK(!pool.used && pool.peak==128);
    struct bridge_ops ops={0,transmit,promiscuous}; uint8_t zero[16]={0};
    CHECK(net_bridge_init(&bridge,&pool,&ops,zero,physical,2,4,99)==-EINVAL);
    init(); memcpy(other,ma,6); net_bridge_shutdown(&bridge);
    /* A persisted host ID deterministically recreates an assignment. */
    init(); CHECK(!memcmp(other,ma,6)); net_bridge_shutdown(&bridge);
    promisc_error=-5; CHECK(net_bridge_enable(&bridge,a,0,other)==-5 && !bridge.count && !bridge.promiscuous);
    promisc_error=0; memcpy(physical[0],ma,6); init(); CHECK(memcmp(physical[0],ma,6));
    /* Wire floods have a separate bounded receive bucket; drain each frame
     * so the rate check cannot accidentally be a queue-full check. */
    ipx_frame(p,peer,ma,64);
    for(unsigned i=0;i<32;i++) {
        net_bridge_ingress(&bridge,4,99,p,64,0);
        CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==64);
    }
    net_bridge_ingress(&bridge,4,99,p,64,0);
    CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==0 && net_bridge_vm(&bridge,a)->rx_overflows==1);
    net_bridge_ingress(&bridge,4,99,p,64,4); CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==64);
    for(unsigned n=0;n<44;n++) { net_bridge_ingress(&bridge,4,99,p,n,1000); CHECK(!pool.used); }
    /* f6-25 admits length-checked raw IPX and learned network numbers. */
    p[12]=0; p[13]=50; net_bridge_ingress(&bridge,4,99,p,64,1000);
    CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==64 && !memcmp(p,out,64) && !pool.used);
    p[12]=0x81; p[13]=0x37; p[20]=1; net_bridge_ingress(&bridge,4,99,p,64,1000);
    CHECK(net_bridge_receive(&bridge,a,out,sizeof(out))==64 && !memcmp(p,out,64) && !pool.used);
    ipx_frame(p,ma,peer,64);
    for(unsigned i=0;i<128;i++) CHECK(bridge_pool_copy(&pool,p,64,4,99)==(int)i);
    CHECK(net_bridge_send(&bridge,a,p,64,1000)==-ENOSPC);
    ipx_frame(p,peer,all,64); net_bridge_ingress(&bridge,4,99,p,64,1000);
    CHECK(net_bridge_vm(&bridge,a)->rx_overflows==2 && net_bridge_vm(&bridge,b)->rx_overflows==1);
    net_bridge_shutdown(&bridge);
    for(unsigned i=0;i<128;i++) bridge_pool_release(&pool,i);
    CHECK(!pool.used);
    printf("bridge: %u checks passed\n",checks); return 0;
}
