/* Asynchronous NDIS5 upper edge; no foreign function is called in the kernel.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "miniport.h"
#include <ciuki/kernel.h>
#include <stddef.h>
_Static_assert(sizeof(struct ndis_chars32)==124,"NDIS51 characteristics");
_Static_assert(offsetof(struct ndis_chars32,cancel_send)==96,"NDIS50 characteristics");
_Static_assert(offsetof(struct ndis_packet32,oob)==30,"NDIS packet OOB offset field");
_Static_assert(offsetof(struct ndis_packet32,time_to_send)==72,"NDIS packet OOB");
_Static_assert(offsetof(struct ndis_packet32,status)==100,"NDIS packet status");
_Static_assert(offsetof(struct ndis_block32,send_complete)==236,"NDIS macro send slot");
_Static_assert(offsetof(struct ndis_block32,eth_receive)==356,"NDIS macro Ethernet slot");
_Static_assert(offsetof(struct ndis_block32,query_complete)==392,"NDIS macro OID slot");
static bool live(struct ndis_miniport *m)
{ return m && m->nt && ndis_host_live(m->nt->host) && m->generation==m->nt->host->generation && m->state!=NDIS_MP_FAILED; }
static uint64_t now(struct ndis_miniport *m) { return m->nt->ops.now(m->nt->ops.context)/10; }
static uint64_t after(struct ndis_miniport *m,uint64_t delta)
{ uint64_t t=now(m);return delta>UINT64_MAX-t?UINT64_MAX:t+delta; }
void ndis_mp_fault(struct ndis_miniport *m,const char *why)
{
    if(!m || m->state==NDIS_MP_FAILED) return;
    m->state=NDIS_MP_FAILED;m->carrier=false;
    if(m->ops.link) m->ops.link(m->ops.context,false);
    ndis_channel_stop(m->channel,-1);
    ndis_host_stop(m->nt->host,why,0,0,now(m));
    /* Pending packets, MDLs and OID storage remain owned until idle proof. */
}
static int rd(struct ndis_miniport *m,uint32_t at,void *out,uint32_t bytes)
{
    if(!live(m) || !bytes || at<0x10000 || at>=0xc0000000 || bytes>0xc0000000-at ||
       !m->nt->ops.read(m->nt->ops.context,at,out,bytes)) { ndis_mp_fault(m,"ndis-read");return 0; }
    return 1;
}
static int wr(struct ndis_miniport *m,uint32_t at,const void *in,uint32_t bytes)
{
    if(!live(m) || !bytes || at<0x10000 || at>=0xc0000000 || bytes>0xc0000000-at ||
       !m->nt->ops.write(m->nt->ops.context,at,in,bytes)) { ndis_mp_fault(m,"ndis-write");return 0; }
    return 1;
}
static uint32_t alloc(struct ndis_miniport *m,uint32_t bytes)
{ return ndis_nt_pool(m->nt,0,bytes,0x7369644e); }
static int enqueue(struct ndis_miniport *m,enum ndis_host_kind kind,uint32_t fn,unsigned argc,const uint32_t *a,uint32_t *token)
{ return live(m) && fn && m->ops.enqueue(m->ops.context,kind,fn,argc,a,token); }
int ndis_mp_inline(struct ndis_miniport *m,uint32_t fn,unsigned argc,const uint32_t *args,bool sync)
{
    if(!live(m) || !m->nt->host->depth || m->nt->host->depth>=NDIS_HOST_DEPTH) return 0;
    struct ndis_user_host *h=m->nt->host;unsigned d=h->depth-1;
    if(m->call[d].active || h->synchronized || (sync && h->irql>h->dirql)) return 0;
    m->call[d].irql=h->irql;m->call[d].sync=sync;
    if(!enqueue(m,NDIS_HOST_INLINE,fn,argc,args,&m->call[d].token)) return 0;
    h->inline_token[d]=m->call[d].token;m->call[d].active=true;
    if(sync) { h->synchronized=true;h->irql=h->dirql; }
    return 1;
}
int ndis_mp_park(struct ndis_miniport *m,struct trap_frame *tf)
{
    if(!live(m) || !m->nt->host->depth) return 0;
    unsigned d=m->nt->host->depth-1;
    if(!m->call[d].active || m->call[d].parked) return 0;
    m->call[d].frame=*tf;m->call[d].parked=true;return 1;
}
int ndis_mp_resume(struct ndis_miniport *m,struct trap_frame *tf)
{
    if(!live(m) || !m->nt->host->depth) return 0;
    struct ndis_user_host *h=m->nt->host;unsigned d=h->depth-1;
    if(!m->call[d].active || !m->call[d].parked) return 0;
    uint32_t result;
    if(!m->ops.result(m->ops.context,m->call[d].token,&result)) return -1;
    *tf=m->call[d].frame;h->inline_token[d]=0;m->call[d].active=false;
    if(m->call[d].sync) {
        h->irql=m->call[d].irql;h->synchronized=false;tf->eax=!!result;
    } else {
        if(result==NDIS_PENDING) m->transfer.pending=true;
        else if(m->transfer.completed || !rd(m,m->scratch+28,&m->transfer.transferred,4)) {
            ndis_mp_fault(m,"ndis-transfer-result");return -1;
        } else { m->transfer.status=result;m->transfer.completed=true; }
        m->transfer.context=0;tf->eax=0; /* VOID indication */
    }
    return 1;
}
static void record(struct ndis_miniport *m,const char *s) { if(m->ops.record) m->ops.record(m->ops.context,s); }
int ndis_mp_init(struct ndis_miniport *m,struct ndis_nt_runtime *nt,struct ndis_channel *channel,const struct ndis_mp_ops *ops)
{
    if(!m || !nt || !ndis_host_live(nt->host) || !channel || !channel->live || !ops ||
       !ops->enqueue || !ops->result || !ops->interrupt || !ops->link || !ops->record) return 0;
    *m=(struct ndis_miniport){.nt=nt,.channel=channel,.ops=*ops,.generation=nt->host->generation,.serialized=true};
    m->adapter=alloc(m,sizeof(struct ndis_block32));m->scratch=alloc(m,256);
    m->control_buffer=alloc(m,NDIS_CONTROL_BYTES+8);m->wrapper=alloc(m,8);m->config=alloc(m,8);
    if(!m->adapter || !m->scratch || !m->control_buffer || !m->wrapper || !m->config) return 0;
    struct ndis_block32 block={0};block.filter[0]=m->adapter;
    uint32_t data[2]={m->generation,m->adapter};
    return wr(m,m->adapter,&block,sizeof(block)) && wr(m,m->wrapper,data,8) && wr(m,m->config,data,8);
}
int ndis_mp_start(struct ndis_miniport *m,uint32_t driver,uint32_t path)
{
    if(!live(m) || m->state!=NDIS_MP_NEW || !driver || !path) return 0;
    uint32_t a[2]={driver,path};m->state=NDIS_MP_ENTRY;
    if(!enqueue(m,NDIS_HOST_LIFECYCLE,m->nt->host->image->pe.base+m->nt->host->image->pe.entry,2,a,&m->lifecycle)) {
        ndis_mp_fault(m,"ndis-entry-queue");return 0;
    }
    record(m,"entry-queued");return 1;
}
uint32_t ndis_mp_register(struct ndis_miniport *m,uint32_t address,uint32_t bytes)
{
    if(!live(m) || m->state!=NDIS_MP_ENTRY || bytes<96 || bytes>sizeof(m->chars)) return NDIS_FAILURE;
    struct ndis_chars32 c={0};
    if(!rd(m,address,&c,bytes)) return NDIS_FAILURE;
    if(c.major!=5 || c.minor>1 || bytes!=(c.minor?124u:96u) || c.reserved || c.filler ||
       !c.initialize || !c.halt || !c.query || !c.set || !c.reset || (!c.send && !c.send_packets) ||
       (!c.return_packet && !c.transfer) || (c.minor && (!c.pnp || !c.shutdown))) return NDIS_NOT_SUPPORTED;
    for(unsigned i=0;i<6;i++) if(c.co[i]) return NDIS_NOT_SUPPORTED;
    for(unsigned i=0;i<4;i++) if(c.reserved51[i]) return NDIS_NOT_SUPPORTED;
    const uint8_t *p=(const uint8_t *)&c;
    for(unsigned off=8;off<(c.minor?108u:96u);off+=4) {
        uint32_t fn=pe_u32(p+off);
        if(fn && !ntimage_executable(m->nt->host->image,fn)) return NDIS_FAILURE;
    }
    m->chars=c;m->state=NDIS_MP_REGISTERED;record(m,"registered");return NDIS_SUCCESS;
}
int ndis_mp_attributes(struct ndis_miniport *m,uint32_t context,uint32_t hang,uint32_t flags,uint32_t bus)
{
    /* PCI, no intermediate/CO/power/timeout bypass. Accept safe-buffer APIs,
     * surprise-remove declaration and NOT_CO_NDIS; power stays host-disabled. */
    const uint32_t allowed=NDIS_BUS_MASTER|NDIS_DESERIALIZE|0x100u|0x200u|0x400u;
    if(!live(m) || m->state!=NDIS_MP_INITIALIZING || m->attributes_set || !context || bus!=5 ||
       (flags&~allowed) || hang>3600) return 0;
    uint8_t probe;
    if(!rd(m,context,&probe,1)) return 0;
    m->context=context;m->attributes=flags;m->serialized=!(flags&NDIS_DESERIALIZE);
    m->hang_seconds=hang<2?2:hang&~1u;m->attributes_set=true;
    return wr(m,m->adapter+offsetof(struct ndis_block32,adapter_context),&context,4);
}
int ndis_mp_irq(struct ndis_miniport *m,uint32_t object,uint32_t vector,uint32_t level,bool isr,bool shared,uint32_t mode)
{
    if(!live(m) || !m->attributes_set || m->interrupt_object || !object || vector!=level ||
       !vector || vector!=m->raw_irq || vector>=16 || !isr || !shared || mode!=0 || !m->chars.isr || !m->chars.handle_irq) return 0;
    /* The backend checks exact owned raw IRQ and registry sharing. */
    uint8_t zero[80]={0};
    if(!wr(m,object,zero,sizeof(zero)) || !m->ops.interrupt(m->ops.context,true,m->chars.isr,m->chars.handle_irq)) return 0;
    m->interrupt_object=object;record(m,"interrupt-registered");return 1;
}
uint32_t ndis_mp_packet_alloc(struct ndis_miniport *m,uint32_t pool,unsigned protocol_bytes)
{
    if(!live(m) || protocol_bytes>128) return 0;
    uint32_t bytes=NDIS_PACKET_BYTES+(protocol_bytes>16?protocol_bytes-16:0);
    uint32_t at=alloc(m,bytes);if(!at) return 0;
    uint8_t data[304]={0};struct ndis_packet32 *p=(struct ndis_packet32 *)(void *)data;
    p->pool=pool;p->oob=(uint16_t)((56+protocol_bytes+7)&~7u);
    if(p->oob<72) p->oob=72;
    p->packet_flags=0xc0; /* NDIS-owned descriptor and allocated OOB */
    if(!wr(m,at,data,bytes)) return 0;return at;
}
int ndis_mp_packet_free(struct ndis_miniport *m,uint32_t packet)
{
    if(!live(m)) return 0;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if((m->tx[i].live && m->tx[i].packet==packet) ||
        (m->returns[i].live && m->returns[i].packet==packet)) return 0;
    if(m->transfer.live && m->transfer.packet==packet) return 0;
    return ndis_nt_free(m->nt,packet);
}
int ndis_mp_oid(struct ndis_miniport *m,bool set,uint32_t oid,const void *buffer,uint32_t bytes)
{
    if(!live(m) || m->control.live || bytes>NDIS_CONTROL_BYTES ||
       (m->state!=NDIS_MP_RUNNING && m->state!=NDIS_MP_CONFIGURING)) return 0;
    uint32_t tail[2]={0x4e444953,0xa55a3cc3},zero[2]={0};
    if((bytes && buffer && !wr(m,m->control_buffer,buffer,bytes)) ||
       !wr(m,m->control_buffer+bytes,tail,8) || !wr(m,m->scratch+16,zero,8)) return 0;
    m->control.oid=oid;m->control.bytes=bytes;m->control.set=set;m->control.live=true;
    m->control.pending=m->control.completed=false;m->request_due=after(m,500000);
    uint32_t a[6]={m->context,oid,m->control_buffer,bytes,m->scratch+16,m->scratch+20};
    if(!enqueue(m,NDIS_HOST_DPC,set?m->chars.set:m->chars.query,6,a,&m->control.callback)) {
        m->control.live=false;return 0;
    }
    return 1;
}
int ndis_mp_oid_complete(struct ndis_miniport *m,bool set,uint32_t status)
{
    if(!live(m) || !m->control.live || m->control.set!=set || m->control.completed || status==NDIS_PENDING) return 0;
    m->control.status=status;m->control.completed=true;return 1;
}
static int control_result(struct ndis_miniport *m)
{
    if(!m->control.live) return 0;
    uint32_t result;
    if(m->control.callback) {
        if(!m->ops.result(m->ops.context,m->control.callback,&result)) return 0;
        m->control.callback=0;
        if(result==NDIS_PENDING) m->control.pending=true;
        else {
            if(m->control.completed) { ndis_mp_fault(m,"ndis-oid-double-result");return -1; }
            m->control.status=result;m->control.completed=true;
        }
    }
    if(!m->control.completed) {
        if(now(m)>=m->request_due) ndis_mp_fault(m,"ndis-oid-timeout");return 0;
    }
    uint32_t counts[2],tail[2];
    if(!rd(m,m->scratch+16,counts,8) || !rd(m,m->control_buffer+m->control.bytes,tail,8)) return -1;
    if(counts[0]>m->control.bytes || tail[0]!=0x4e444953 || tail[1]!=0xa55a3cc3) {
        ndis_mp_fault(m,"ndis-oid-bounds");return -1;
    }
    m->control.written=counts[0];m->control.needed=counts[1];m->control.live=false;return 1;
}
/* Mandatory capability discovery, then directed+broadcast filter and lookahead.
 * Optional OIDs are queried only if advertised; unsupported is never zero. */
static const uint32_t startup[]={0x00010101,0x00010102,0x00010103,0x00010104,
    0x00010106,0x00010111,0x00010105,0x01010101,0x01010102,0x01010104,
    0x00010107,0x00010114,0x00010113,0x0001010c,0x0001010d,0x00010115,
    0x00010116,0x0001010f,0x0001010e,0x00010110,0x01010103};
static bool has_oid(struct ndis_miniport *m,uint32_t oid)
{ for(unsigned i=0;i<m->supported_count;i++) if(m->supported[i]==oid) return true;return false; }
static int startup_result(struct ndis_miniport *m)
{
    uint32_t oid=m->control.oid,value=0;unsigned bytes=m->control.written;
    if(m->control.status!=NDIS_SUCCESS) return 0;
    if(m->control.set) return bytes==m->control.bytes;
    if(oid==0x00010101) {
        if(!bytes || (bytes&3) || bytes>sizeof(m->supported) || !rd(m,m->control_buffer,m->supported,bytes)) return 0;
        m->supported_count=bytes/4;return 1;
    }
    if(oid==0x01010101 || oid==0x01010102) {
        uint8_t mac[6];if(bytes!=6 || !rd(m,m->control_buffer,mac,6) || (mac[0]&1)) return 0;
        unsigned any=0;for(unsigned i=0;i<6;i++) any|=mac[i];if(!any) return 0;
        memcpy(oid==0x01010101?m->permanent:m->address,mac,6);return 1;
    }
    if(oid==0x00010110) return bytes==2; /* DRIVER_VERSION */
    if(oid==0x0001010d) return bytes>0 && bytes<=NDIS_CONTROL_BYTES; /* description */
    if(bytes<4 || !rd(m,m->control_buffer,&value,4)) return 0;
    switch(oid) {
    case 0x00010102:case 0x00010103:case 0x00010104:return value==0;
    case 0x00010106:m->mtu=value;return value==1500;
    case 0x00010111:m->total=value;return value==1514;
    case 0x00010105:m->lookahead=value<1500?value:1500;return value<=65535 && (value>=1500 || m->chars.transfer);
    case 0x01010104:m->multicast_limit=value;return value<=4096;
    case 0x00010107:m->link_speed=value;break;
    case 0x00010113:m->mac_options=value;break;
    case 0x00010114:if(value>1) return 0;m->carrier=!value;break;
    case 0x00010115:m->max_sends=value;return value>0;
    default:break;
    }
    return bytes==4;
}
static int packet_chain(struct ndis_miniport *m,uint32_t address,uint8_t *out,unsigned *length,uint32_t *status)
{
    struct ndis_packet32 p;
    uint32_t allocation_bytes=0;
    for(unsigned i=0;i<NDIS_NT_ALLOCS;i++) if(m->nt->allocation[i].live && m->nt->allocation[i].address==address)
        allocation_bytes=m->nt->allocation[i].bytes;
    if(!rd(m,address,&p,56) || p.oob<56 || p.oob>512 || address>UINT32_MAX-p.oob-32 ||
       (uint32_t)p.oob+32>allocation_bytes ||
       !rd(m,address+p.oob+28,status,4)) return 0;
    uint32_t next=p.head,seen[32];unsigned count=0,total=0;
    while(next) {
        if(count==32) return 0;
        for(unsigned j=0;j<count;j++) if(seen[j]==next) return 0;
        seen[count++]=next;struct ndis_nt_mdl32 b;
        if(!rd(m,next,&b,sizeof(b)) || b.size<28 || b.offset>=4096 || (b.start&4095) ||
           b.start>UINT32_MAX-b.offset || b.mapped!=b.start+b.offset ||
           b.bytes>NDIS_RING_FRAME-total || !b.bytes || !rd(m,b.mapped,out+total,b.bytes)) return 0;
        bool owned=false;
        for(unsigned j=0;j<NDIS_NT_OBJECTS;j++) if(m->nt->mdl[j].live && m->nt->mdl[j].address==next &&
            m->nt->mdl[j].buffer==b.mapped && b.bytes<=m->nt->mdl[j].bytes) owned=true;
        if(!owned) return 0;
        total+=b.bytes;next=b.next;
    }
    if(total<14 || total>NDIS_RING_FRAME || !count || seen[count-1]!=p.tail ||
       (p.valid && (p.total!=total || p.count!=count))) return 0;
    /* Reject 802.1Q tags; offloads/jumbos are never enabled. */
    if((out[12]==0x81 && out[13]==0) || (out[12]==0x88 && out[13]==0xa8)) return 0;
    *length=total;return 1;
}
static int publish_rx(struct ndis_miniport *m,const uint8_t *frame,unsigned length)
{
    struct ndis_ring_shared *s=&m->channel->shared->rx;
    unsigned slot=NDIS_RING_SLOTS;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) {
        uint32_t credit=__atomic_load_n(&m->channel->shared->rx_credit[i],__ATOMIC_ACQUIRE);
        if(credit && credit!=m->rx_credit[i]) { slot=i;break; }
    }
    if(slot==NDIS_RING_SLOTS) return 0;
    uint32_t token=__atomic_load_n(&m->channel->shared->rx_credit[slot],__ATOMIC_ACQUIRE);
    memcpy(s->frames[slot],frame,length);
    s->descriptor[m->rx_published%NDIS_RING_SLOTS]=(struct ndis_ring_descriptor){m->generation,token,slot,
        offsetof(struct ndis_ring_shared,frames)+slot*NDIS_RING_STRIDE,length};
    m->rx_credit[slot]=token;
    __atomic_store_n(&s->published,++m->rx_published,__ATOMIC_RELEASE);
    return ndis_channel_doorbell(m->channel,m->generation);
}
int ndis_mp_receive(struct ndis_miniport *m,const uint32_t *packets,unsigned count)
{
    if(!live(m) || m->state!=NDIS_MP_RUNNING || !packets || !count || count>NDIS_RING_SLOTS) return 0;
    uint8_t frame[NDIS_RING_FRAME];
    for(unsigned i=0;i<count;i++) {
        unsigned length;uint32_t status;
        for(unsigned j=0;j<i;j++) if(packets[j]==packets[i]) return 0;
        for(unsigned j=0;j<NDIS_RING_SLOTS;j++) if(m->returns[j].live && m->returns[j].packet==packets[i]) return 0;
        if(!packet_chain(m,packets[i],frame,&length,&status) || (status!=NDIS_SUCCESS && status!=NDIS_RESOURCES)) return 0;
        (void)publish_rx(m,frame,length); /* copied drop on bounded backpressure */
        if(!m->serialized && status==NDIS_SUCCESS) {
            unsigned j=0;while(j<NDIS_RING_SLOTS && m->returns[j].live) j++;
            if(j==NDIS_RING_SLOTS || !m->chars.return_packet) return 0;
            uint32_t a[2]={m->context,packets[i]};
            m->returns[j].packet=packets[i];m->returns[j].live=true;
            if(!enqueue(m,NDIS_HOST_DPC,m->chars.return_packet,2,a,&m->returns[j].token)) return 0;
        }
        /* Serialized SUCCESS and all RESOURCES descriptors stay with miniport.
         * Copying synchronously means this host never requests serialized PENDING. */
    }
    return 1;
}
static int finish_tx(struct ndis_miniport *m,unsigned i,uint32_t status)
{
    struct ndis_mp_tx *t=&m->tx[i];
    if(!t->live || t->completed || status==NDIS_PENDING || status==NDIS_RESOURCES) return 0;
    t->completed=true;
    if(!ndis_channel_tx_complete(m->channel,m->generation,t->slot,t->token,status==NDIS_SUCCESS?0:-1)) return 0;
    m->tx_blocked=false;return 1;
}
int ndis_mp_send_complete(struct ndis_miniport *m,uint32_t packet,uint32_t status)
{
    if(!live(m)) return 0;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(m->tx[i].live && m->tx[i].packet==packet && m->tx[i].submitted)
        return finish_tx(m,i,status);
    return 0;
}
int ndis_mp_send_resources(struct ndis_miniport *m)
{ if(!live(m) || !m->serialized) return 0;m->tx_blocked=false;return 1; }
static void release_tx(struct ndis_miniport *m,struct ndis_mp_tx *t)
{
    if(t->sg) {
        unsigned index=t->slot;
        if(!ndis_nt_dma_complete(m->nt,index,m->generation,m->nt->dma[index].pin.ticket)) { ndis_mp_fault(m,"ndis-sg-unpin");return; }
        (void)ndis_nt_free(m->nt,t->sg);
    }
    uint32_t p=t->packet,b=t->buffer,mdl=t->mdl;
    if(!ndis_nt_free_mdl(m->nt,mdl) || !ndis_nt_free(m->nt,b)) { ndis_mp_fault(m,"ndis-tx-release");return; }
    t->live=false;
    if(!ndis_nt_free(m->nt,p)) ndis_mp_fault(m,"ndis-packet-release");
}
static int make_tx(struct ndis_miniport *m,unsigned slot,const struct ndis_ring_descriptor *d)
{
    struct ndis_mp_tx *t=&m->tx[slot];
    *t=(struct ndis_mp_tx){.slot=slot,.token=d->token,.deadline=after(m,1000000)};
    t->packet=ndis_mp_packet_alloc(m,0,16);t->buffer=alloc(m,d->length);
    if(!t->packet || !t->buffer || !wr(m,t->buffer,m->channel->shared->tx.frames[slot],d->length)) return 0;
    t->mdl=ndis_nt_mdl(m->nt,t->buffer,d->length);if(!t->mdl) return 0;
    struct ndis_packet32 p={.head=t->mdl,.tail=t->mdl,.count=1,.physical_count=1,.total=d->length,
        .valid=1,.packet_flags=0xc0,.oob=72,.status=NDIS_PENDING};
    if(m->sg) {
        struct ndis_nt_segment segment;unsigned count=1;
        if(!ndis_nt_dma_start(m->nt,slot,t->mdl,MM_PIN_TO_DEVICE,UINT32_MAX,1,0,NDIS_RING_FRAME,&segment,&count)) return 0;
        uint32_t list[6]={1,0,segment.physical,0,segment.bytes,0};
        t->sg=alloc(m,sizeof(list));
        if(!t->sg || !wr(m,t->sg,list,sizeof(list))) return 0;
        p.extension[5]=t->sg;
    }
    if(!wr(m,t->packet,&p,sizeof(p))) return 0;
    t->live=true;return 1;
}
static void send_poll(struct ndis_miniport *m)
{
    /* One SG packet at a time follows NT5 SG resource ordering. Ordered
     * single-element arrays also preserve RESOURCES suffix ordering. */
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) {
        struct ndis_mp_tx *t=&m->tx[i];if(!t->live) continue;
        if(t->callback) {
            uint32_t result;if(!m->ops.result(m->ops.context,t->callback,&result)) continue;
            t->callback=0;
            if(m->serialized) {
                if(m->chars.send_packets && !rd(m,t->packet+100,&result,4)) return;
                if(t->completed) {
                    if(result!=NDIS_PENDING) { ndis_mp_fault(m,"ndis-send-double-result");return; }
                } else if(result==NDIS_RESOURCES) { t->submitted=false;t->resources=true;m->tx_blocked=true; }
                else if(result!=NDIS_PENDING && !finish_tx(m,i,result)) { ndis_mp_fault(m,"ndis-send-result");return; }
            }
        }
        if(t->completed && !t->callback) release_tx(m,t);
        else if(now(m)>=t->deadline) { ndis_mp_fault(m,"ndis-send-timeout");return; }
    }
    if(!live(m) || m->tx_blocked) return;
    struct ndis_ring_shared *s=&m->channel->shared->tx;
    uint32_t published=__atomic_load_n(&s->published,__ATOMIC_ACQUIRE);
    if(published-m->tx_consumed>NDIS_RING_SLOTS) { ndis_mp_fault(m,"ndis-tx-cursor");return; }
    /* Retry the oldest resource-rejected packet before later publications. */
    struct ndis_mp_tx *t=0;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(m->tx[i].live && !m->tx[i].submitted &&
        (!t || m->tx[i].token<t->token)) t=&m->tx[i];
    if(!t && published!=m->tx_consumed) {
        unsigned slot=m->tx_consumed%NDIS_RING_SLOTS;
        if(m->tx[slot].live) return;
        struct ndis_ring_descriptor d=s->descriptor[slot];
        if(d.generation!=m->generation || d.slot!=slot || !d.token || d.length<60 || d.length>NDIS_RING_FRAME ||
           d.offset!=offsetof(struct ndis_ring_shared,frames)+slot*NDIS_RING_STRIDE) { ndis_mp_fault(m,"ndis-tx-descriptor");return; }
        if(!make_tx(m,slot,&d)) { ndis_mp_fault(m,"ndis-tx-allocation");return; }
        t=&m->tx[slot];m->tx_consumed++;
    }
    if(!t) return;
    t->submitted=true;t->resources=false;
    uint32_t pending=NDIS_PENDING;
    if(!wr(m,t->packet+100,&pending,4)) return;
    uint32_t a[3]={m->context,t->packet,0};
    if(m->chars.send_packets) {
        if(!wr(m,t->packet+176,&t->packet,4)) return;
        a[1]=t->packet+176;a[2]=1;
    }
    if(!enqueue(m,NDIS_HOST_DPC,m->chars.send_packets?m->chars.send_packets:m->chars.send,3,a,&t->callback))
        ndis_mp_fault(m,"ndis-send-queue");
}
int ndis_mp_lookahead(struct ndis_miniport *m,uint32_t context,uint32_t header,unsigned header_bytes,
                       uint32_t lookahead,unsigned lookahead_bytes,unsigned packet_bytes)
{
    if(!live(m) || m->state!=NDIS_MP_RUNNING || header_bytes!=14 || packet_bytes>1500 ||
       lookahead_bytes>packet_bytes || m->transfer.live) return 0;
    uint8_t frame[NDIS_RING_FRAME];
    if(!rd(m,header,frame,14) || (lookahead_bytes && !rd(m,lookahead,frame+14,lookahead_bytes))) return 0;
    if(lookahead_bytes==packet_bytes) { (void)publish_rx(m,frame,packet_bytes+14);return 1; }
    if(!m->chars.transfer || !packet_bytes) return 0;
    memset(&m->transfer,0,sizeof(m->transfer));
    m->transfer.packet=ndis_mp_packet_alloc(m,0,16);m->transfer.buffer=alloc(m,packet_bytes);
    if(!m->transfer.packet || !m->transfer.buffer) return 0;
    m->transfer.mdl=ndis_nt_mdl(m->nt,m->transfer.buffer,packet_bytes);
    if(!m->transfer.mdl) return 0;
    struct ndis_packet32 p={.head=m->transfer.mdl,.tail=m->transfer.mdl,.total=packet_bytes,
        .count=1,.valid=1,.packet_flags=0xc0,.oob=72};
    uint32_t zero=0;
    if(!wr(m,m->transfer.packet,&p,sizeof(p)) || !wr(m,m->scratch+28,&zero,4)) return 0;
    m->transfer.live=true;m->transfer.bytes=packet_bytes;m->transfer.context=context;
    memcpy(m->transfer.header,frame,14);
    uint32_t a[6]={m->transfer.packet,m->scratch+28,m->context,context,0,packet_bytes};
    if(!ndis_mp_inline(m,m->chars.transfer,6,a,false)) return 0;
    return 1; /* indication's syscall continuation waits for TransferData return */
}
int ndis_mp_transfer_complete(struct ndis_miniport *m,uint32_t packet,uint32_t status,unsigned transferred)
{
    if(!live(m) || !m->transfer.live || m->transfer.completed || packet!=m->transfer.packet || status==NDIS_PENDING) return 0;
    m->transfer.completed=true;m->transfer.status=status;m->transfer.transferred=transferred;return 1;
}
static void transfer_poll(struct ndis_miniport *m)
{
    if(!m->transfer.live || !m->transfer.completed || m->transfer.context) return;
    if(m->transfer.transferred>m->transfer.bytes) { ndis_mp_fault(m,"ndis-transfer-overrun");return; }
    if(m->transfer.status==NDIS_SUCCESS && m->transfer.transferred==m->transfer.bytes) {
        uint8_t frame[NDIS_RING_FRAME];memcpy(frame,m->transfer.header,14);
        if(!rd(m,m->transfer.buffer,frame+14,m->transfer.bytes)) return;
        (void)publish_rx(m,frame,m->transfer.bytes+14);
    }
    /* A short or failed completion is a drop, never an uninitialized frame. */
    if(!ndis_nt_free_mdl(m->nt,m->transfer.mdl) || !ndis_nt_free(m->nt,m->transfer.buffer)) {
        ndis_mp_fault(m,"ndis-transfer-release");return;
    }
    uint32_t packet=m->transfer.packet;m->transfer.live=false;
    if(!ndis_nt_free(m->nt,packet)) ndis_mp_fault(m,"ndis-transfer-packet-release");
}
int ndis_mp_reset(struct ndis_miniport *m)
{
    if(!live(m) || m->state!=NDIS_MP_RUNNING || m->control.live || m->lifecycle) return 0;
    uint32_t a[2]={m->scratch+24,m->context},zero=0;
    if(!wr(m,m->scratch+24,&zero,4)) return 0;
    m->state=NDIS_MP_RESETTING;m->reset_pending=m->reset_completed=false;m->request_due=after(m,10000000);
    if(!enqueue(m,NDIS_HOST_LIFECYCLE,m->chars.reset,2,a,&m->lifecycle)) return 0;
    record(m,"reset-queued");return 1;
}
int ndis_mp_reset_complete(struct ndis_miniport *m,uint32_t status,bool addressing)
{
    if(!live(m) || m->state!=NDIS_MP_RESETTING || m->reset_completed || status==NDIS_PENDING) return 0;
    m->reset_result=status;m->addressing_reset=addressing;m->reset_completed=true;return 1;
}
int ndis_mp_halt(struct ndis_miniport *m)
{
    if(!live(m) || m->state!=NDIS_MP_RUNNING || m->control.live || m->lifecycle) return 0;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(m->tx[i].live || m->returns[i].live) return 0;
    if(m->transfer.live) return 0;
    m->state=NDIS_MP_HALTING;uint32_t a[1]={m->context};
    return enqueue(m,NDIS_HOST_LIFECYCLE,m->chars.halt,1,a,&m->lifecycle);
}
int ndis_mp_shutdown(struct ndis_miniport *m)
{
    if(!live(m) || m->state!=NDIS_MP_RUNNING || m->lifecycle || m->control.live) return 0;
    uint32_t fn=m->chars.minor?m->chars.shutdown:m->shutdown;
    uint32_t context=m->chars.minor?m->context:m->shutdown_context;
    if(!fn || !enqueue(m,NDIS_HOST_LIFECYCLE,fn,1,&context,&m->lifecycle)) return 0;
    m->state=NDIS_MP_SHUTTING;return 1;
}
void ndis_mp_status(struct ndis_miniport *m,uint32_t status,bool complete)
{
    if(!live(m)) return;
    if(complete) {
        if(m->status_pending) { m->carrier=m->status_carrier;m->ops.link(m->ops.context,m->carrier);m->status_pending=false; }
    } else if(status==0x4001000bu || status==0x4001000cu) {
        m->status_pending=true;m->status_carrier=status==0x4001000bu;
    }
}
void ndis_mp_poll(struct ndis_miniport *m)
{
    if(!live(m)) return;
    if(m->lifecycle) {
        uint32_t result;if(!m->ops.result(m->ops.context,m->lifecycle,&result)) return;
        m->lifecycle=0;
        if(m->state==NDIS_MP_REGISTERED) {
            if(result!=NDIS_SUCCESS) { ndis_mp_fault(m,"ndis-driver-entry-failed");return; }
            uint32_t init[3]={NDIS_FAILURE,UINT32_MAX,0};
            if(!wr(m,m->scratch,init,12)) return;
            uint32_t a[6]={m->scratch,m->scratch+4,m->scratch+8,1,m->adapter,m->config};
            m->state=NDIS_MP_INITIALIZING;
            if(!enqueue(m,NDIS_HOST_LIFECYCLE,m->chars.initialize,6,a,&m->lifecycle)) ndis_mp_fault(m,"ndis-init-queue");
            record(m,"initialize-queued");return;
        }
        if(m->state==NDIS_MP_INITIALIZING) {
            uint32_t selected;
            if(result!=NDIS_SUCCESS || !m->attributes_set || !rd(m,m->scratch+4,&selected,4) || selected) {
                ndis_mp_fault(m,"ndis-initialize-failed");return;
            }
            m->state=NDIS_MP_CONFIGURING;m->startup_oid=0;record(m,"initialized");
        } else if(m->state==NDIS_MP_RESETTING) {
            if(result==NDIS_PENDING) m->reset_pending=true;
            else {
                uint32_t addressing;
                if(m->reset_completed || !rd(m,m->scratch+24,&addressing,4) || addressing>1) { ndis_mp_fault(m,"ndis-reset-result");return; }
                m->reset_result=result;m->addressing_reset=addressing;m->reset_completed=true;
            }
        } else if(m->state==NDIS_MP_HALTING || m->state==NDIS_MP_SHUTTING) {
            bool shutdown=m->state==NDIS_MP_SHUTTING;
            m->state=NDIS_MP_STOPPED;m->carrier=false;m->ops.link(m->ops.context,false);
            record(m,shutdown?"shutdown-retained":"halted-retained");return; /* Neither is an idle proof. */
        } else { ndis_mp_fault(m,"ndis-entry-without-registration");return; }
    }
    if(m->state==NDIS_MP_RESETTING) {
        if(m->reset_completed) {
            if(m->reset_result!=NDIS_SUCCESS) { ndis_mp_fault(m,"ndis-reset-failed");return; }
            m->state=m->addressing_reset?NDIS_MP_CONFIGURING:NDIS_MP_RUNNING;
            m->startup_oid=m->addressing_reset?16:0;record(m,"reset-complete");
        } else if(now(m)>=m->request_due) ndis_mp_fault(m,"ndis-reset-timeout");
        return;
    }
    bool had_control=m->control.live;
    int done=control_result(m);
    if(!live(m)) return;
    if(m->state==NDIS_MP_CONFIGURING) {
        if(had_control && !done) return;
        if(done && !startup_result(m)) { ndis_mp_fault(m,"ndis-mandatory-oid");return; }
        while(m->startup_oid<sizeof(startup)/sizeof(startup[0])) {
            unsigned i=m->startup_oid++;uint32_t oid=startup[i],value=0;bool set=i==17 || i==18 || i==20;
            if(i>=13 && !has_oid(m,oid) && !set) continue;
            if(i==17) value=m->lookahead;
            if(i==18) { value=9;m->filter=value; }
            uint32_t bytes=i==0?NDIS_CONTROL_BYTES:i==7 || i==8?6:i==14?256:i==19?2:i==20?0:4;
            if(!ndis_mp_oid(m,set,oid,set?&value:0,bytes)) ndis_mp_fault(m,"ndis-startup-oid-queue");
            return;
        }
        m->state=NDIS_MP_RUNNING;m->hang_due=after(m,(uint64_t)m->hang_seconds*1000000);m->counter_due=after(m,1000000);
        m->ops.link(m->ops.context,m->carrier);record(m,"running");
    }
    if(m->state!=NDIS_MP_RUNNING) return;
    if(done && m->counter_request) {
        unsigned i=(m->counter_next+4)%5;
        if(m->control.status==NDIS_SUCCESS) {
            uint32_t value[2]={0};unsigned bytes=m->control.written;
            if((bytes!=4 && bytes!=8) || !rd(m,m->control_buffer,value,bytes)) { ndis_mp_fault(m,"ndis-counter-width");return; }
            m->counters[i]=value[0]|(uint64_t)value[1]<<32;m->counter_width[i]=(uint8_t)bytes;
        }
        m->counter_request=false;
    }
    transfer_poll(m);
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(m->returns[i].live) {
        uint32_t ignored;
        if(m->ops.result(m->ops.context,m->returns[i].token,&ignored)) m->returns[i].live=false;
    }
    if(m->hang_token) {
        uint32_t hung;
        if(m->ops.result(m->ops.context,m->hang_token,&hung)) {
            m->hang_token=0;
            if(hung && !ndis_mp_reset(m)) ndis_mp_fault(m,"ndis-hang-reset-busy");
        }
    } else if(m->chars.check_hang && now(m)>=m->hang_due) {
        uint32_t a[1]={m->context};m->hang_due=after(m,(uint64_t)m->hang_seconds*1000000);
        if(!enqueue(m,NDIS_HOST_DPC,m->chars.check_hang,1,a,&m->hang_token)) ndis_mp_fault(m,"ndis-hang-queue");
    }
    if(m->state==NDIS_MP_RUNNING) send_poll(m);
    if(live(m) && !m->control.live && !m->counter_request && now(m)>=m->counter_due) {
        unsigned i=m->counter_next;m->counter_next=(i+1)%5;m->counter_due=after(m,200000);
        uint32_t oid=0x00020101u+i;
        if(has_oid(m,oid)) m->counter_request=ndis_mp_oid(m,false,oid,0,8)!=0;
    }
}
