/* Native sockets and DOS VMs meet at the one physical bridge owner.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/ipx.h>
#include <ciuki/net_bridge_port.h>
#include <ciuki/net_stack.h>
#include <ciuki/ipx_config.h>
static struct ipx_stack stack;
static struct net_bridge_port *port;
static int transmit(void *arg,const uint8_t *p,unsigned n)
{ return net_bridge_host_send(&((struct net_bridge_port *)arg)->bridge,p,n); }
static void publish_network(void)
{
    uint32_t number;memcpy(&number,stack.network,4);
    if(number)__atomic_store_n(&port->bridge.ipx_network,number,__ATOMIC_RELEASE);
}
static void import_network(void)
{
    uint32_t number=__atomic_load_n(&port->bridge.ipx_network,__ATOMIC_ACQUIRE);
    if(!number || !memcmp(stack.network,&number,4))return;
    memcpy(stack.network,&number,4);
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)if(stack.endpoint[i].used && stack.endpoint[i].bound)
        memcpy(stack.endpoint[i].local.network,&number,4);
}
static void receive(void *arg,const uint8_t *p,unsigned n,uint64_t now)
{
    (void)ipx_set_framing(&stack,net_ipx_frame_type());
    (void)arg; if(now>stack.now)stack.now=now;
    ipx_input(&stack,p,n); publish_network(); port->bridge.wire_framing=stack.framing; net_input_changed();
}
static void retire_idle(void)
{
    if (port->bridge.count || (stack.link && stack.frame_type==CIUKI_IPX_FRAME_AUTO &&
        (!stack.probed || stack.now<=stack.discovery_end))) return;
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)if(stack.endpoint[i].used)return;
    port->bridge.host_input=NULL;port->bridge.host_tick=NULL;
}
static void tick(void *arg,bool link,uint64_t now)
{
    (void)ipx_set_framing(&stack,net_ipx_frame_type());
    (void)arg; int before[IPX_ENDPOINTS];
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)before[i]=ipx_events(&stack,i);
    import_network(); stack.linkspeed=port->device->speed*1250u;
    ipx_link(&stack,link); ipx_tick(&stack,now); publish_network();
    bool changed=false;
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)changed|=before[i]!=ipx_events(&stack,i);
    port->bridge.wire_framing=stack.framing; retire_idle();
    if(changed)net_input_changed();
}
static int acquire(bool create)
{
    struct net_bridge_port *p=port;
    if(create) { int e=net_bridge_primary_port(&p);if(e)return e; }
    if(!p)return -EBADF;
    if(!net_bridge_port_acquire(p))return IPX_PORT_BUSY;
    net_ipx_port_start(p);
    (void)ipx_set_framing(&stack,net_ipx_frame_type()); import_network();return 0;
}
static void release(void) { net_bridge_port_release(port); net_input_changed(); }
void net_ipx_port_start(struct net_bridge_port *p)
{
    if (!p) return;
    if (!p->bridge.host_input) {
        port=p; ipx_init(&stack,p->device->address,p,transmit);
        unsigned setting=net_ipx_frame_type();
        (void)ipx_set_framing(&stack,setting);
        p->bridge.frame_type=setting; p->bridge.wire_framing=stack.framing;
        ipx_link(&stack,p->bridge.link); import_network();
        memcpy(p->bridge.host_node,stack.node,6);
        p->bridge.host_arg=p; p->bridge.host_input=receive; p->bridge.host_tick=tick;
    }
}
int net_ipx_open(unsigned type,unsigned protocol)
{
    int r=acquire(true); if(r)return r;
    r=ipx_open(&stack,type,protocol); release(); return r<0?r:r+1;
}
int net_ipx_call(unsigned token,unsigned op,void *data,unsigned n,unsigned flags,
                 struct ciuki_sockaddr_ipx *a,unsigned *out)
{
    int r=acquire(false); if(r)return r;
    unsigned i=token-1;
    switch(op) {
    case IPX_CALL_BIND:r=ipx_bind(&stack,i,a);break;
    case IPX_CALL_CONNECT:r=ipx_connect(&stack,i,a);break;
    case IPX_CALL_LISTEN:r=ipx_listen(&stack,i,n);break;
    case IPX_CALL_ACCEPT:
        r=ipx_accept(&stack,i);
        if(r>=0) {
            if(a)*a=stack.endpoint[r].peer;
            if(data && n==sizeof(struct ciuki_sockaddr_ipx))memcpy(data,&stack.endpoint[r].local,n);
            r++;
        }
        break;
    case IPX_CALL_SEND:r=ipx_send(&stack,i,data,n,a);break;
    case IPX_CALL_RECV:r=ipx_recv(&stack,i,data,n,flags,a,out);break;
    case IPX_CALL_SHUTDOWN:r=ipx_shutdown(&stack,i,n);break;
    case IPX_CALL_STATUS:
        if(i>=IPX_ENDPOINTS || !stack.endpoint[i].used)r=-EBADF;
        else { r=(int)stack.endpoint[i].state; if(out)*out=(unsigned)stack.endpoint[i].connect_error; } break;
    default:r=-EINVAL;
    }
    if(op==IPX_CALL_STATUS)net_bridge_port_release(port);else release(); return r;
}
int net_ipx_names(unsigned token,struct ciuki_sockaddr_ipx *local,struct ciuki_sockaddr_ipx *peer)
{
    int r=acquire(false); if(r)return r;
    if(!token || token>IPX_ENDPOINTS || !stack.endpoint[token-1].used)r=-EBADF;
    else { *local=stack.endpoint[token-1].local; *peer=stack.endpoint[token-1].peer; }
    net_bridge_port_release(port); return r;
}
int net_ipx_option(unsigned token,bool set,int level,unsigned name,void *value,unsigned *length)
{
    int r=acquire(false); if(r)return r;
    r=ipx_option(&stack,token-1,set,level,name,value,length); release(); return r;
}
int net_ipx_events(unsigned token)
{
    int r=acquire(false); if(r)return 0;
    int events=ipx_events(&stack,token-1); net_bridge_port_release(port); return events;
}
int net_ipx_close(unsigned token)
{
    int r=acquire(false); if(r)return r;
    ipx_close(&stack,token-1); retire_idle(); release(); return 0;
}
