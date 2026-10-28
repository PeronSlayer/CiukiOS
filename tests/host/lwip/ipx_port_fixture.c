/* Physical-port fake for the production IPX runtime. Protocols, fd ownership,
 * worker requests and poll remain production code. */
#include <ciuki/net_bridge_port.h>
#include <pthread.h>
#include <stdatomic.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static struct net_bridge_port port;
static struct bridge_pool pool;
static struct netif device;
static _Atomic unsigned busy;
void cu_ipx_test_busy(unsigned count) { busy=count; }
void cu_ipx_test_network(uint32_t network) { __atomic_store_n(&port.bridge.ipx_network,network,__ATOMIC_RELEASE); }
static int send_frame(void *arg,const uint8_t *p,unsigned n)
{ (void)arg;(void)p;(void)n;return 0; }
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
