/* Native glue for the NDIS scalar upper edge. Foreign callbacks use the
 * existing process dispatcher exclusively. SPDX-License-Identifier: GPL-2.0-only */
#include "native.h"
#include "execution.h"
#include <ciuki/kernel.h>
#include <ciuki/net.h>
static int queue(void *ctx,enum ndis_host_kind kind,uint32_t fn,unsigned argc,const uint32_t *args,uint32_t *token)
{ return ndis_native_enqueue(ctx,kind,fn,argc,args,ndis_execution_now(),token); }
static int result(void *ctx,uint32_t token,uint32_t *value)
{ struct ndis_native *n=ctx;return ndis_native_take_result(n,n->host.generation,token,value); }
static int interrupt(void *ctx,bool install,uint32_t isr,uint32_t dpc)
{
    struct ndis_native *n=ctx;
    if(install) { n->adapter_context=n->miniport.context;return ndis_native_irq_bind(n,isr,dpc); }
    if(!n->qualification->mask(n,true) || n->isr_callback || n->dpc_callback || n->host.irq_pending) return 0;
    /* The native registry owns synchronization and pending accounting. */
    return ndis_native_irq_unbind(n);
}
static void link(void *ctx,bool carrier)
{
    struct ndis_native *n=ctx;
    if(n->netif) {
        n->netif->link=carrier?NET_LINK_UP:NET_LINK_DOWN;
        if(!carrier && n->netif->bridge_epoch!=UINT32_MAX) n->netif->bridge_epoch++;
    }
}
static void record(void *ctx,const char *phase)
{
    struct ndis_native *n=ctx;
    klog("[ndis] phase=%s driver=%s generation=%u",phase,n->host.image->file,n->memory.generation);
}
static unsigned pci(void *ctx,bool write,unsigned offset,void *data,unsigned bytes)
{ return write?ndis_native_pci_write(ctx,offset,data,bytes):ndis_native_pci_read(ctx,offset,data,bytes); }
static uint32_t ports(void *ctx,unsigned first,unsigned bytes,bool release)
{
    (void)release;struct ndis_native *n=ctx;
    if(!bytes || first>=65536 || bytes>65536-first) return 0;
    for(unsigned i=0;i<n->host.port_count;i++) if(first==n->host.ports[i].first && first+bytes==n->host.ports[i].end) return first;
    return 0;
}
static uint32_t mmio(void *ctx,uint32_t address,unsigned bytes,bool release)
{
    struct ndis_native *n=ctx;
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) {
        struct ndis_host_map *p=&n->host.mappings[i];
        if(p->kind!=NDIS_HOST_BAR || !p->live || !bytes || bytes!=p->bytes) continue;
        if(!release && address==p->physical) return p->va;
        if(release && address==p->va) {
            if(ndis_user_mapping_apply(&n->memory,p,false)) return 0;
            p->live=false;return address;
        }
    }
    return 0;
}
static unsigned resources(void *ctx,void *out,unsigned capacity)
{
    struct ndis_native *n=ctx;uint8_t list[128]={1,0,1,0};unsigned count=0;
    for(unsigned i=0;i<6;i++) {
        uint32_t bytes=n->qualification->bar_bytes[i];if(!bytes) continue;
        uint8_t *p=list+8+count*16;
        if(count==7) return 0;
        bool io=!!(n->pci.bar[i]&1);p[0]=io?1:3;p[1]=1;pe_put16(p+2,io?1:0);
        pe_put32(p+4,n->pci.bar[i]&(io?~3u:~15u));pe_put32(p+12,bytes);count++;
    }
    if(count==7 || !n->pci.irq_line) return 0;
    uint8_t *p=list+8+count*16;p[0]=2;p[1]=3;
    pe_put32(p+4,n->pci.irq_line);pe_put32(p+8,n->pci.irq_line);pe_put32(p+12,1);count++;
    pe_put32(list+4,count);unsigned bytes=8+count*16;
    if(capacity<bytes) return 0;memcpy(out,list,bytes);return bytes;
}
static void log_event(void *ctx,uint32_t code,const uint32_t *data,unsigned count)
{ (void)ctx;klog("[ndis] event=%x words=%u first=%x",code,count,count?data[0]:0); }
int ndis_native_miniport_init(struct ndis_native *n)
{
    struct ndis_mp_ops ops={n,queue,result,interrupt,link,record};
    struct ndis_service_ops services={n,pci,ports,mmio,resources,log_event};
    if(!ndis_mp_init(&n->miniport,&n->nt,&n->channel,&ops)) return 0;
    n->miniport.raw_irq=n->pci.irq_line;
    if(!ndis_services_init(&n->ndis,&n->miniport,&services,n->runtime_base+256+NDIS_NT_EXPORTS*32)) return 0;
    /* Stable, host-owned instance name; vendor installation parameters are
     * filled only by the selected f6-07 manifest, never pack-wide defaults. */
    uint16_t name[]={'C','i','u','k','i','O','S',' ','E','t','h','e','r','n','e','t',0};
    uint32_t at=ndis_nt_pool(&n->nt,0,8+sizeof(name),0x7369644e);
    struct ndis_nt_string str={sizeof(name)-2,sizeof(name),at+8};
    if(!at || !n->nt.ops.write(n,at,&str,8) || !n->nt.ops.write(n,at+8,name,sizeof(name))) return 0;
    n->ndis.instance_string=at;return 1;
}
int ndis_native_miniport_start(struct ndis_native *n,uint32_t driver,uint32_t path)
{ return n && ndis_host_live(&n->host) && n->ndis.miniport && ndis_mp_start(&n->miniport,driver,path); }
