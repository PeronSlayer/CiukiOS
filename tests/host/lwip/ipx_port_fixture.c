/* Physical-port fake for the production IPX runtime. Protocols, fd ownership,
 * worker requests and poll remain production code. */
#include <ciuki/net_bridge_port.h>
#include <ciuki/ipx.h>
#include <ciuki/ipx_config.h>
#include <pthread.h>
#include <stdatomic.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static struct net_bridge_port port;
static struct bridge_pool pool;
static struct netif device;
static _Atomic unsigned busy,wire_framing;
static unsigned probes[IPX_FRAMINGS];
unsigned cu_ipx_test_wire(void) { return wire_framing; }
void cu_ipx_test_busy(unsigned count) { busy=count; }
void cu_ipx_test_network(uint32_t network) { __atomic_store_n(&port.bridge.ipx_network,network,__ATOMIC_RELEASE); }
static int send_frame(void *arg,const uint8_t *p,unsigned n)
{ (void)arg;unsigned f;if(!ipx_decode(p,n,&f))return -EINVAL;wire_framing=f;probes[f]++;return 0; }
static int mode(void *arg,bool on) { (void)arg;(void)on;return 0; }
static void initialize(void)
{
    const uint8_t id[16]={3};const uint8_t mac[6]={2,0,0,0,0,77};
    memcpy(device.address,mac,6);device.running=device.ready=true;device.generation=1;
    port.device=&device;port.generation=1;port.registered=true;
    struct bridge_ops ops={NULL,send_frame,mode};
    (void)net_bridge_init(&port.bridge,&pool,&ops,id,NULL,0,0,1);net_bridge_link(&port.bridge,true);
}
int net_bridge_primary_port(struct net_bridge_port **out)
{ pthread_once(&once,initialize);*out=&port;return 0; }
bool net_bridge_port_acquire(struct net_bridge_port *p)
{
    (void)p;unsigned n=busy;
    if(n && atomic_compare_exchange_strong(&busy,&n,n-1))return false;
    return !pthread_mutex_trylock(&gate);
}
void net_bridge_port_release(struct net_bridge_port *p)
{ (void)p;pthread_mutex_unlock(&gate); }
void net_bridge_port_carrier(struct net_bridge_port *p) { (void)p; }

int cu_ipx_test_dos_only(void)
{
    pthread_once(&once,initialize);pthread_mutex_lock(&gate);
    uint8_t mac[6];struct bridge_key key={99,1};int result=0;
    if(net_bridge_enable(&port.bridge,key,0,mac))result=1;
    port.bridge.wire_framing=IPX_RAW; /* A retired Auto hub must not keep a stale router choice. */
    net_ipx_port_start(&port);
    if(!port.bridge.host_tick)result=2;
    else {
        port.bridge.host_tick(port.bridge.host_arg,true,2000);
        for(unsigned f=0;f<IPX_FRAMINGS;f++)if(probes[f]!=1)result=3;
        port.bridge.host_tick(port.bridge.host_arg,true,3001);
        if(port.bridge.wire_framing!=IPX_LLC)result=4;
        net_bridge_disable(&port.bridge,key);
        port.bridge.host_tick(port.bridge.host_arg,true,3002);
        if(port.bridge.host_input)result=5;
        net_ipx_port_start(&port); /* No endpoints and the new discovery has not run yet. */
        port.bridge.host_tick(port.bridge.host_arg,false,3003);
        if(port.bridge.host_input)result=6;
    }
    pthread_mutex_unlock(&gate);return result;
}
