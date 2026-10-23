/* ND9 adapter: all frame input is copied/validated by channel.c before the
 * common network worker sees it. SPDX-License-Identifier: GPL-2.0-only */
#include "netif.h"
#include "execution.h"
#include <ciuki/kernel.h>
static struct ndis_native *native(struct netif *d) { return d->driver_context; }
static bool match(const struct pci_func *p) { (void)p;return false; /* explicit installation only */ }
static bool idle(struct netif *d) { return native(d)->host.idle_proved; }
static int init(struct netif *d) { return native(d)->miniport.state==NDIS_MP_RUNNING?0:-19; }
static int stop(struct netif *d)
{
    struct ndis_native *n=native(d);
    ndis_native_poll(n);
    if(!ndis_host_live(&n->host)) return -5;
    if(n->miniport.state==NDIS_MP_RUNNING) {
        if(!ndis_mp_halt(&n->miniport)) return -EAGAIN;
        return -EAGAIN;
    }
    if(n->miniport.state!=NDIS_MP_STOPPED) return -EAGAIN;
    /* Halt alone is no idle proof. The independent stop and PCI fence run
     * before user mappings or tasks can be reaped. Pins remain boot-owned. */
    ndis_host_stop(&n->host,"miniport-halted",0,0,ndis_execution_now());
    return n->host.idle_proved?0:-5;
}
static int transmit(struct netif *d,const uint8_t *frame,unsigned length)
{
    struct ndis_native *n=native(d);uint32_t token;
    if(!ndis_host_live(&n->host) || n->miniport.state!=NDIS_MP_RUNNING) return -19;
    unsigned slot=n->channel.tx_published%NDIS_RING_SLOTS;
    int result=ndis_channel_send(&n->channel,frame,length,&token);
    if(result<=0) return result? -5:-EAGAIN;
    n->net_token[slot]=token;n->net_length[slot]=length<60?60:length;return 0;
}
void ndis_netif_tx_done(struct ndis_native *n,uint32_t token,int status)
{
    if(!n || !n->netif) return;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(n->net_token[i]==token) {
        n->net_token[i]=0;
        if(status) n->netif->stats.tx_errors++;
        else { n->netif->stats.tx_completed++;n->netif->stats.tx_bytes+=n->net_length[i]; }
        return;
    }
}
static int receive(struct netif *d,uint8_t *frame)
{
    struct ndis_native *n=native(d);ndis_native_poll(n);
    if(!ndis_host_live(&n->host)) return -5;
    unsigned length=0;int result=ndis_channel_receive(&n->channel,frame,&length);
    if(result<0) { ndis_mp_fault(&n->miniport,"ndis-ring-validation");return -5; }
    return result?(int)length:0;
}
static enum net_link link_state(struct netif *d) { return native(d)->miniport.carrier?NET_LINK_UP:NET_LINK_DOWN; }
static void mac(struct netif *d,uint8_t address[6]) { memcpy(address,native(d)->miniport.address,6); }
static bool interrupt(struct netif *d) { (void)d;return false; /* native host IRQ chain is sole owner */ }
static void irq_enable(struct netif *d,bool enabled) { (void)d;(void)enabled; /* native dispatcher owns rearm */ }
static int promiscuous(struct netif *d,bool enabled) { (void)d;(void)enabled;return -ENOSYS; }
const struct netif_driver net_ndis={"ndis",match,0,0,0,idle,init,init,stop,transmit,receive,link_state,mac,interrupt,irq_enable,promiscuous};
static void fault(struct netif *d,const char *why) { ndis_mp_fault(&native(d)->miniport,why); }
int ndis_netif_publish(struct ndis_native *n)
{
    if(!n || !ndis_host_live(&n->host) || n->netif || n->miniport.state!=NDIS_MP_RUNNING ||
       !n->qualification || n->qualification!=ndis_qualified_profile(n->host.image,&n->pci)) return 0;
    n->netif=netif_publish_external(&n->pci,&net_ndis,n,n->miniport.address,n->host.generation,n->miniport.carrier,fault);
    return n->netif!=0;
}
