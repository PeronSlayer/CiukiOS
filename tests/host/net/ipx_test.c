/* Five real endpoint hubs and the production DOS bridge/API on a fake segment.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/ipx.h>
#include <ciuki/net_bridge.h>
#include "../../../src/kernel/vm/dosvm_ipx.h"
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"IPX line %u: %s\n",__LINE__,#c); exit(1); } checks++; } while(0)
static unsigned checks,active_nodes=5;
static bool windows98_llc;
static struct ipx_stack nodes[5];
struct queued { uint8_t frame[1514]; unsigned size,from; };
static struct queued queue[256];
static unsigned head,count,drop,wire_count;
static uint8_t duplicate[1514]; static unsigned duplicate_size;
static struct net_bridge bridge;
static struct bridge_pool pool;
static uint64_t now;
static int segment(void *arg,const uint8_t *p,unsigned n)
{
    CHECK(count<256 && n<=1514); wire_count++;
    if(drop) { drop--; return 0; }
    struct queued *q=&queue[(head+count++)%256]; memcpy(q->frame,p,n);q->size=n;q->from=(unsigned)(uintptr_t)arg;
    return 0;
}
static int native_send(void *unused,const uint8_t *p,unsigned n)
{ (void)unused;return net_bridge_host_send(&bridge,p,n); }
static void native_receive(void *unused,const uint8_t *p,unsigned n,uint64_t ticks)
{ (void)unused;(void)ticks;ipx_input(&nodes[0],p,n); }
static int promiscuous(void *unused,bool on) { (void)unused;(void)on;return 0; }
static void pump(void)
{
    unsigned budget=10000;
    while(count) {
        CHECK(budget--); struct queued q=queue[head];head=(head+1)%256;count--;
        for(unsigned i=0;i<active_nodes;i++)if(i!=q.from) {
            unsigned format;CHECK(ipx_decode(q.frame,q.size,&format));
            if(windows98_llc && i==1 && format!=IPX_LLC)continue;
            if(i==0)net_bridge_ingress(&bridge,0,1,q.frame,q.size,now);
            else ipx_input(&nodes[i],q.frame,q.size);
        }
    }
}
static void tick(unsigned elapsed)
{ now+=elapsed; for(unsigned i=0;i<5;i++)ipx_tick(&nodes[i],now); pump(); }
static struct ciuki_sockaddr_ipx address(unsigned node,unsigned socket)
{
    struct ciuki_sockaddr_ipx a={.family=AF_IPX};memcpy(a.node,nodes[node].node,6);ipx_put16((uint8_t *)&a.socket,socket);return a;
}
static int bound(unsigned node,unsigned type,unsigned socket)
{
    int e=ipx_open(&nodes[node],type,type==SOCK_DGRAM?NSPROTO_IPX+4:NSPROTO_SPX);CHECK(e>=0);
    struct ciuki_sockaddr_ipx a=address(node,socket);CHECK(!ipx_bind(&nodes[node],(unsigned)e,&a));return e;
}
static void set(unsigned node,unsigned e,int level,unsigned option,unsigned value)
{ unsigned n=4;CHECK(!ipx_option(&nodes[node],e,true,level,option,&value,&n)); }
static int receive(unsigned node,unsigned e,void *data,unsigned n,unsigned flags,unsigned *out)
{ return ipx_recv(&nodes[node],e,data,n,flags,NULL,out); }
static void datagrams(void)
{
    unsigned fd[5];for(unsigned i=0;i<active_nodes;i++)fd[i]=(unsigned)bound(i,SOCK_DGRAM,0x869c);
    struct ciuki_sockaddr_ipx to=address(0,0x869c);memset(to.node,255,6);
    CHECK(ipx_send(&nodes[0],fd[0],"find",4,&to)==-EACCES);set(0,fd[0],SOL_SOCKET,SO_BROADCAST,1);
    CHECK(ipx_send(&nodes[0],fd[0],"find",4,&to)==4);pump();
    char b[32];unsigned out;
    for(unsigned i=0;i<active_nodes;i++) { CHECK(ipx_events(&nodes[i],fd[i])&POLLIN);CHECK(receive(i,fd[i],b,32,0,&out)==4 && !memcmp(b,"find",4)); }
    set(1,fd[1],NSPROTO_IPX,IPX_FILTERPTYPE,7);
    to=address(1,0x869c);CHECK(ipx_send(&nodes[0],fd[0],"bad",3,&to)==3);pump();CHECK(receive(1,fd[1],b,32,0,&out)==-EAGAIN);
    set(0,fd[0],NSPROTO_IPX,IPX_PTYPE,7);CHECK(ipx_send(&nodes[0],fd[0],"typed",5,&to)==5);pump();
    CHECK(receive(1,fd[1],b,2,MSG_PEEK,&out)==2 && out==MSG_TRUNC);
    CHECK(receive(1,fd[1],b,32,0,&out)==5 && !out);
    for(unsigned f=0;f<IPX_FRAMINGS;f++) {
        CHECK(!ipx_set_framing(&nodes[0],CIUKI_IPX_FRAME_AUTO)); nodes[0].framing=f; memset(&nodes[0].frames,0,sizeof(nodes[0].frames)); CHECK(ipx_send(&nodes[0],fd[0],"frame",5,&to)==5);pump();CHECK(receive(1,fd[1],b,32,0,&out)==5);
        CHECK(ipx_frame_reply(&nodes[1].frames,nodes[0].node,IPX_FRAMINGS)==f);
        struct ciuki_sockaddr_ipx reply=address(0,0x869c);
        CHECK(ipx_send(&nodes[1],fd[1],"reply",5,&reply)==5);pump();
        CHECK(receive(0,fd[0],b,32,0,&out)==5);
    }
    struct ciuki_ipx_address_data a={0};unsigned n=sizeof(a);CHECK(!ipx_option(&nodes[0],fd[0],false,NSPROTO_IPX,IPX_ADDRESS,&a,&n));
    CHECK(n==24 && !a.wan && !a.status && !memcmp(a.nodenum,nodes[0].node,6));
    a.adapternum=1;n=sizeof(a);CHECK(ipx_option(&nodes[0],fd[0],false,NSPROTO_IPX,IPX_ADDRESS,&a,&n)==-EINVAL);
    for(unsigned i=0;i<active_nodes;i++)ipx_close(&nodes[i],fd[i]);
    printf("PASS %u-node IPX broadcast, directed datagrams, ptype filtering, peek/truncation, four framings, adapter query\n",active_nodes);
}
static void mixed_framings(void)
{
    const unsigned types[]={CIUKI_IPX_FRAME_LLC,CIUKI_IPX_FRAME_LLC,CIUKI_IPX_FRAME_RAW,CIUKI_IPX_FRAME_II,CIUKI_IPX_FRAME_SNAP};
    windows98_llc=true;
    for(active_nodes=2;active_nodes<=5;active_nodes++) {
        unsigned fd[5];char b[16];unsigned flags;
        for(unsigned i=0;i<active_nodes;i++) {CHECK(!ipx_set_framing(&nodes[i],types[i]));fd[i]=(unsigned)bound(i,SOCK_DGRAM,0x6c32);}
        struct ciuki_sockaddr_ipx broadcast=address(1,0x6c32);memset(broadcast.node,255,6);set(1,fd[1],SOL_SOCKET,SO_BROADCAST,1);
        CHECK(ipx_send(&nodes[1],fd[1],"discover",8,&broadcast)==8);pump();
        for(unsigned i=0;i<active_nodes;i++)CHECK(receive(i,fd[i],b,sizeof(b),0,&flags)==8);
        struct ciuki_sockaddr_ipx peer=address(1,0x6c32);
        for(unsigned i=0;i<active_nodes;i++)if(i!=1)CHECK(ipx_send(&nodes[i],fd[i],"reply",5,&peer)==5);
        pump();
        for(unsigned i=1;i<active_nodes;i++)CHECK(receive(1,fd[1],b,sizeof(b),0,&flags)==5);
        CHECK(receive(1,fd[1],b,sizeof(b),0,&flags)==-EAGAIN);
        if(active_nodes>2) {
            set(2,fd[2],SOL_SOCKET,SO_BROADCAST,1);
            CHECK(ipx_send(&nodes[2],fd[2],"raw",3,&broadcast)==3);pump();
            CHECK(receive(1,fd[1],b,sizeof(b),0,&flags)==-EAGAIN); /* A Win98 802.2 binding refuses raw. */
            for(unsigned i=0;i<active_nodes;i++)if(i!=1)CHECK(receive(i,fd[i],b,sizeof(b),0,&flags)==3);
        }
        for(unsigned i=0;i<active_nodes;i++)ipx_close(&nodes[i],fd[i]);
    }
    active_nodes=5;windows98_llc=false;
    puts("PASS 2-5 mixed-framing peers: strict Windows 98-style LLC binding receives every directed reply; manual broadcasts retain their framing");
}
static void framing(void)
{
    struct ipx_stack fallback;uint8_t fallback_node[6]={2,1,2,3,4,5};
    ipx_init(&fallback,fallback_node,NULL,NULL);CHECK(fallback.framing==IPX_LLC);
    ipx_tick(&fallback,0);ipx_tick(&fallback,1001);CHECK(fallback.framing==IPX_LLC);
    for(unsigned type=0;type<=CIUKI_IPX_FRAME_AUTO;type++) {
        CHECK(!ipx_set_framing(&fallback,type));CHECK(fallback.framing==ipx_frame_wire(type));
        ipx_tick(&fallback,2000+type*2000);CHECK(fallback.probed==(type==CIUKI_IPX_FRAME_AUTO));
    }
    CHECK(ipx_set_framing(&fallback,5)==-EINVAL);
    struct ipx_stack s;uint8_t node[6]={2,3,4,5,6,7};ipx_init(&s,node,NULL,NULL);s.probed=true;s.discovery_end=1000;
    uint8_t p[60]={0},wire[1514];memcpy(p,node,6);p[6]=2;p[7]=99;ipx_put16(p+12,0x8137);
    p[14]=p[15]=255;ipx_put16(p+16,40);p[19]=1;p[32]=0x12;memcpy(p+24,p,6);memcpy(p+36,p+6,6);
    ipx_put16(p+30,0x453);ipx_put16(p+42,0x453);ipx_put16(p+44,2);memset(p+46,0xff,4);
    for(unsigned f=0;f<IPX_FRAMINGS;f++) {
        int n=ipx_encode(wire,p,54,f);CHECK(n>=60);unsigned format=99;CHECK(ipx_decode(wire,(unsigned)n,&format) && format==f);
        ipx_input(&s,wire,(unsigned)n);CHECK(s.scores[f]==1);ipx_input(&s,wire,(unsigned)n);CHECK(s.scores[f]==1);
        wire[16]^=0x80;CHECK(!ipx_decode(wire,(unsigned)n,NULL));
    }
    CHECK(s.framing==IPX_LLC && s.network[0]==0x12);
    p[7]++;memcpy(p+36,p+6,6);int n=ipx_encode(wire,p,54,IPX_RAW);ipx_input(&s,wire,(unsigned)n);CHECK(s.framing==IPX_RAW);
    s.now=1001;p[7]++;memcpy(p+36,p+6,6);n=ipx_encode(wire,p,54,IPX_II);ipx_input(&s,wire,(unsigned)n);CHECK(s.framing==IPX_RAW);
    memset(wire,0,60);wire[12]=8;wire[13]=0;CHECK(!ipx_decode(wire,60,NULL));
    puts("PASS RIP most-prevalent framing, unique-response votes, deadline, malformed length/discriminator refusal");
}
static void exhaustion(void)
{
    struct ipx_stack s;uint8_t node[6]={2,0,0,0,0,42};ipx_init(&s,node,NULL,NULL);
    for(unsigned i=0;i<IPX_ENDPOINTS;i++) { int e=ipx_open(&s,SOCK_DGRAM,1000);CHECK(e==(int)i);struct ciuki_sockaddr_ipx a={.family=AF_IPX};CHECK(!ipx_bind(&s,i,&a));
        unsigned num=ipx_be16((const uint8_t *)&s.endpoint[i].local.socket);CHECK(num>=0x4000 && num<=0x7fff); }
    CHECK(ipx_open(&s,SOCK_DGRAM,1000)==-ENOBUFS);
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)ipx_close(&s,i);
    int e=ipx_open(&s,SOCK_DGRAM,1000);CHECK(e==0);ipx_close(&s,0);
    puts("PASS bounded socket exhaustion, unique dynamic allocation, reuse after close");
}
static void spx(unsigned type)
{
    int listener=bound(1,type,0x5000);CHECK(!ipx_listen(&nodes[1],listener,4));
    int client=bound(0,type,0x5001);struct ciuki_sockaddr_ipx to=address(1,0x5000);
    drop=1;CHECK(ipx_connect(&nodes[0],client,&to)==-EINPROGRESS);pump();tick(250);
    CHECK(nodes[0].endpoint[client].connected);CHECK(ipx_events(&nodes[1],listener)&POLLIN);
    int server=ipx_accept(&nodes[1],listener);CHECK(server>=0);
    char b[1024];unsigned out;drop=1;CHECK(ipx_send(&nodes[0],client,"lost",4,NULL)==4);pump();
    CHECK(receive(1,server,b,sizeof(b),0,&out)==-EAGAIN);tick(250);
    CHECK(receive(1,server,b,sizeof(b),0,&out)==4 && !memcmp(b,"lost",4));pump();
    CHECK(ipx_send(&nodes[0],client,"one",3,NULL)==3);CHECK(count==1);
    struct queued saved=queue[head];memcpy(duplicate,saved.frame,saved.size);duplicate_size=saved.size;
    /* Future sequence must not be delivered; the real sequence still works. */
    unsigned off=ipx_decode(saved.frame,saved.size,NULL);ipx_put16(saved.frame+off+36,300);
    unsigned allocation=nodes[1].endpoint[server].allocation;
    ipx_put16(saved.frame+off+40,(uint16_t)(allocation-1));
    ipx_input(&nodes[1],saved.frame,saved.size);CHECK(receive(1,server,b,sizeof(b),0,&out)==-EAGAIN);
    CHECK(nodes[1].endpoint[server].allocation==allocation);
    pump();ipx_input(&nodes[1],duplicate,duplicate_size);pump();
    CHECK(receive(1,server,b,sizeof(b),0,&out)==3 && !memcmp(b,"one",3));pump();CHECK(receive(1,server,b,sizeof(b),0,&out)==-EAGAIN);
    /* Drop the ACK instead of the data; retransmission cannot duplicate RX. */
    CHECK(ipx_send(&nodes[0],client,"ack",3,NULL)==3);saved=queue[head];head=(head+1)%256;count--;drop=1;
    ipx_input(&nodes[1],saved.frame,saved.size);pump();tick(250);CHECK(receive(1,server,b,sizeof(b),0,&out)==3);pump();CHECK(receive(1,server,b,sizeof(b),0,&out)==-EAGAIN);
    memset(b,'x',sizeof(b));int sent=ipx_send(&nodes[0],client,b,sizeof(b),NULL);
    if(type==SOCK_STREAM) { CHECK(sent==SPX_DATA_MAX);pump();CHECK(receive(1,server,b,3,0,&out)==3);pump();CHECK(receive(1,server,b,sizeof(b),0,&out)==SPX_DATA_MAX-3);pump(); }
    else CHECK(sent==-EMSGSIZE);
    for(unsigned i=0;i<IPX_QUEUE;i++) { CHECK(ipx_send(&nodes[0],client,"window",6,NULL)==6);pump(); }
    CHECK(ipx_send(&nodes[0],client,"full",4,NULL)==-EAGAIN);
    CHECK(receive(1,server,b,sizeof(b),0,&out)==6);pump();
    CHECK(ipx_send(&nodes[0],client,"space",5,NULL)==5);pump();
    for(unsigned i=0;i<IPX_QUEUE;i++)CHECK(receive(1,server,b,sizeof(b),0,&out)>0);
    pump();
    if(type==SOCK_SEQPACKET) {
        CHECK(ipx_send(&nodes[0],client,"record",6,NULL)==6);pump();
        CHECK(receive(1,server,b,2,0,&out)==2 && out==MSG_TRUNC);pump();
        CHECK(receive(1,server,b,sizeof(b),0,&out)==-EAGAIN);
    }
    /* Healthy watchdog probes leave the connection ready. */
    tick(10000);CHECK(nodes[0].endpoint[client].connected);
    CHECK(!ipx_shutdown(&nodes[0],client,SHUT_WR));pump();CHECK(receive(1,server,b,sizeof(b),0,&out)==0);
    ipx_close(&nodes[0],client);ipx_close(&nodes[1],server);ipx_close(&nodes[1],listener);pump();tick(4000);
    for(unsigned i=0;i<5;i++)ipx_link(&nodes[i],false);
    for(unsigned i=0;i<5;i++)ipx_link(&nodes[i],true);
    puts(type==SOCK_STREAM?"PASS SPX stream handshake/data/ACK loss, reorder, duplicates, partial reads, watchdog, orderly close":"PASS SPX sequenced record limit, retransmit/duplicate/reorder, readiness, watchdog, orderly close");
}
static void timeouts(void)
{
    int client=bound(0,SOCK_STREAM,0x5101);struct ciuki_sockaddr_ipx to=address(1,0x5100);
    CHECK(ipx_connect(&nodes[0],client,&to)==-EINPROGRESS);pump();
    for(unsigned i=0;i<10;i++)tick(4000);
    CHECK(nodes[0].endpoint[client].error==ETIMEDOUT && !nodes[0].endpoint[client].tx);
    CHECK(ipx_events(&nodes[0],client)&POLLERR);
    unsigned error=0,bytes=4;CHECK(!ipx_option(&nodes[0],client,false,SOL_SOCKET,SO_ERROR,&error,&bytes) && error==ETIMEDOUT);
    bytes=4;CHECK(!ipx_option(&nodes[0],client,false,SOL_SOCKET,SO_ERROR,&error,&bytes) && !error);
    CHECK(nodes[0].endpoint[client].connect_error==ETIMEDOUT);
    ipx_close(&nodes[0],client);
    int listener=bound(1,SOCK_STREAM,0x5100);CHECK(!ipx_listen(&nodes[1],listener,1));
    client=bound(0,SOCK_STREAM,0x5101);CHECK(ipx_connect(&nodes[0],client,&to)==-EINPROGRESS);pump();
    int server=ipx_accept(&nodes[1],listener);CHECK(server>=0);
    drop=1000;tick(10000);for(unsigned i=0;i<4;i++)tick(5000);drop=0;
    CHECK(nodes[0].endpoint[client].error==ETIMEDOUT && nodes[1].endpoint[server].error==ETIMEDOUT);
    ipx_close(&nodes[0],client);ipx_close(&nodes[1],server);ipx_close(&nodes[1],listener);
    puts("PASS bounded SPX handshake retries and lost-peer watchdog termination, retransmit pool drain");
}
/* DOS legacy calls exercise real ECB parsing, pin ownership and completion. */
static struct dosvm_ipx dos;
static uint8_t memory[0x10000],mac[6];static unsigned pins;
static const struct bridge_key key={1,2};
static int pin(void *a,uint32_t at,unsigned n,bool write,uint32_t *token)
{ (void)a;(void)write;if(at>sizeof(memory) || n>sizeof(memory)-at)return -EFAULT;*token=++pins;return 0; }
static void unpin(void *a,uint32_t token) { (void)a;CHECK(token && pins);pins--; }
static int read_mem(void *a,uint32_t at,void *out,unsigned n) { (void)a;memcpy(out,memory+at,n);return 0; }
static int write_mem(void *a,uint32_t at,const void *p,unsigned n) { (void)a;memcpy(memory+at,p,n);return 0; }
static int dos_send(void *a,const uint8_t *p,unsigned n) { (void)a;return net_bridge_send_ipx(&bridge,key,p,n,now); }
static int upcall(void *a,const struct dosvm_ipx_upcall *c) { (void)a;(void)c;return 1; }
static void little(uint8_t *p,unsigned v) { p[0]=v;p[1]=v>>8; }
static void dos_call(unsigned fn,unsigned at)
{ struct dosvm_ipx_regs r={.bx=fn,.dx=0x9c86,.es=at>>4,.si=at&15};CHECK(dosvm_ipx_interrupt(&dos,0x7a,&r)==1 && !(r.ax&255)); }
static void ecb(unsigned at,unsigned size)
{ memset(memory+at,0,48);memory[at+10]=0x86;memory[at+11]=0x9c;little(memory+at+34,1);little(memory+at+36,0);little(memory+at+38,0x500);little(memory+at+40,size); }
static void dos_test(void)
{
    CHECK(!net_bridge_enable(&bridge,key,now,mac));struct dosvm_ipx_ops ops={NULL,pin,unpin,read_mem,write_mem,dos_send,NULL,upcall};
    CHECK(!dosvm_ipx_init(&dos,2,mac,&ops));dos_call(0,0);
    unsigned native=(unsigned)bound(0,SOCK_DGRAM,0x869c),remote=(unsigned)bound(2,SOCK_DGRAM,0x869c);
    set(0,native,SOL_SOCKET,SO_BROADCAST,1);
    for(unsigned f=0;f<IPX_FRAMINGS;f++) {
        ecb(0x1000,576);dos_call(4,0x1000);struct ciuki_sockaddr_ipx to=address(0,0x869c);memcpy(to.node,mac,6);
        nodes[0].framing=f;bridge.wire_framing=f;unsigned before=wire_count;
        CHECK(ipx_send(&nodes[0],native,"DOS",3,&to)==3 && before==wire_count);
        uint8_t p[1514];int n=net_bridge_receive(&bridge,key,p,sizeof(p));CHECK(n>0 && dosvm_ipx_receive(&dos,p,(unsigned)n)==1);
        CHECK(!memory[0x1008] && !memcmp(memory+0x501e,"DOS",3));
        ecb(0x1000,33);memcpy(memory+0x500a,nodes[0].node,6);memcpy(memory+0x101c,nodes[0].node,6);dos_call(3,0x1000);dosvm_ipx_service(&dos,0);net_bridge_step(&bridge);
        char b[16];unsigned out;CHECK(receive(0,native,b,sizeof(b),0,&out)==3 && !memcmp(b,"DOS",3));
    }
    ecb(0x1000,576);dos_call(4,0x1000);
    struct ciuki_sockaddr_ipx to=address(0,0x869c);memcpy(to.node,mac,6);nodes[2].framing=IPX_RAW;
    CHECK(ipx_send(&nodes[2],remote,"LAN",3,&to)==3);pump();uint8_t p[1514];int n=net_bridge_receive(&bridge,key,p,sizeof(p));CHECK(n>0 && dosvm_ipx_receive(&dos,p,(unsigned)n)==1);
    ecb(0x1000,33);memcpy(memory+0x500a,nodes[2].node,6);memcpy(memory+0x101c,nodes[2].node,6);
    dos_call(3,0x1000);dosvm_ipx_service(&dos,0);net_bridge_step(&bridge);pump();
    char reply[8];unsigned out;CHECK(receive(2,remote,reply,sizeof(reply),0,&out)==3 && !memcmp(reply,"LAN",3));
    uint8_t learned_frame[1514];int learned_size=ipx_encode(learned_frame,p,(unsigned)n,IPX_II);
    CHECK(learned_size>0);learned_frame[32]=0x12;
    ecb(0x1000,576);dos_call(4,0x1000);
    CHECK(dosvm_ipx_receive(&dos,learned_frame,(unsigned)learned_size)==1);
    dos_call(9,0x6000);CHECK(memory[0x6000]==0x12 && !memcmp(memory+0x6004,mac,6));
    dosvm_ipx_destroy(&dos);CHECK(!pins);net_bridge_disable(&bridge,key);CHECK(!pool.used);
    ipx_close(&nodes[0],native);ipx_close(&nodes[2],remote);
    puts("PASS Win32-hub to/from real DOS ECB API, local unicast suppression, four framings and remote raw ingress, pin/pool drain");
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    const uint8_t id[16]={1};struct bridge_ops ops={NULL,segment,promiscuous};
    CHECK(!net_bridge_init(&bridge,&pool,&ops,id,NULL,0,0,1));net_bridge_link(&bridge,true);
    for(unsigned i=0;i<5;i++) { uint8_t node[6]={2,0,0,0,0,(uint8_t)(i+1)};
        ipx_init(&nodes[i],node,(void *)(uintptr_t)i,i?segment:native_send); }
    memcpy(bridge.host_node,nodes[0].node,6);bridge.host_input=native_receive;
    tick(0);for(active_nodes=2;active_nodes<=5;active_nodes++)datagrams();active_nodes=5;mixed_framings();framing();exhaustion();spx(SOCK_STREAM);spx(SOCK_SEQPACKET);timeouts();dos_test();
    for(unsigned i=0;i<5;i++)for(unsigned e=0;e<IPX_ENDPOINTS;e++)if(nodes[i].endpoint[e].used)ipx_close(&nodes[i],e);
    for(unsigned i=0;i<5;i++)ipx_link(&nodes[i],false);
    net_bridge_shutdown(&bridge);CHECK(!pool.used);printf("PASS f6-25 IPX/SPX: %u checks\n",checks);return 0;
}
