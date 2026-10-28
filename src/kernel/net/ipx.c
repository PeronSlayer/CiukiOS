/* Classic IPX/SPX; see docs/design/win32-ipx.md. No allocation outside the
 * shared packet pool, no blocking, no untrusted assertions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/ipx.h>
#define SPX_SYS 0x80
#define SPX_ACK 0x40
#define SPX_EOM 0x10
#define SPX_RETRIES 8
#define SPX_IDLE_MS 10000
#define SPX_WATCH_MS 5000
static struct ipx_endpoint *endpoint(struct ipx_stack *s, unsigned i)
{ return s && i < IPX_ENDPOINTS && s->endpoint[i].used ? &s->endpoint[i] : NULL; }
static bool zero(const void *p, unsigned n)
{ const uint8_t *b = p; while (n--) if (*b++) return false; return true; }
static bool same_peer(const struct ciuki_sockaddr_ipx *a, const struct ciuki_sockaddr_ipx *b)
{ return a->socket == b->socket && !memcmp(a->node, b->node, 6) &&
    (zero(a->network,4) || zero(b->network,4) || !memcmp(a->network,b->network,4)); }
static void read_address(struct ciuki_sockaddr_ipx *a, const uint8_t *p)
{ memset(a,0,sizeof(*a)); a->family=AF_IPX; memcpy(a->network,p,4); memcpy(a->node,p+4,6); memcpy(&a->socket,p+10,2); }
static void write_address(uint8_t *p, const struct ciuki_sockaddr_ipx *a)
{ memcpy(p,a->network,4); memcpy(p+4,a->node,6); memcpy(p+10,&a->socket,2); }
static void drain(struct ipx_endpoint *e)
{
    while(e->count) { net_packet_free(e->rx[e->head]); e->head=(e->head+1)%IPX_QUEUE; e->count--; }
    if(e->tx)net_packet_free(e->tx);
    e->tx=NULL; e->offset=0;
}
static void destroy(struct ipx_endpoint *e) { drain(e); memset(e,0,sizeof(*e)); }
static void failed(struct ipx_endpoint *e, int error)
{ drain(e); e->error=error; e->connect_error=error; e->state=SPX_DEAD; e->connected=false; }
void ipx_init(struct ipx_stack *s, const uint8_t node[6], void *arg,
              int (*send)(void *,const uint8_t *,unsigned))
{
    memset(s,0,sizeof(*s)); memcpy(s->node,node,6); s->arg=arg; s->send=send;
    s->next_socket=0x4000; s->next_id=1; s->framing=IPX_II; s->link=true;
}
int ipx_open(struct ipx_stack *s, unsigned type, unsigned protocol)
{
    if(type!=SOCK_DGRAM && type!=SOCK_STREAM && type!=SOCK_SEQPACKET)return -ESOCKTNOSUPPORT;
    if(protocol && (type==SOCK_DGRAM ? protocol<NSPROTO_IPX || protocol>NSPROTO_IPX+255 : protocol!=NSPROTO_SPX))return -EPROTONOSUPPORT;
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)if(!s->endpoint[i].used) {
        struct ipx_endpoint *e=&s->endpoint[i]; memset(e,0,sizeof(*e));
        e->used=true; e->type=type; e->local.family=AF_IPX; e->peer.family=AF_IPX;
        e->packet_type=type==SOCK_DGRAM ? protocol ? protocol-NSPROTO_IPX : 0 : 5;
        e->receive_broadcast=true; e->last_rx=s->now;
        /* Never reuse a live connection ID, including detached close timers. */
        do { if(!++s->next_id || s->next_id==0xffff)s->next_id=1;
            bool busy=false; for(unsigned j=0;j<IPX_ENDPOINTS;j++)busy|=s->endpoint[j].used && s->endpoint[j].id==s->next_id;
            if(!busy)break;
        } while(true);
        e->id=s->next_id; return (int)i;
    }
    return -ENOBUFS;
}
static bool socket_busy(struct ipx_stack *s, uint16_t number)
{ for(unsigned i=0;i<IPX_ENDPOINTS;i++)if(s->endpoint[i].used && s->endpoint[i].bound && s->endpoint[i].local.socket==number)return true; return false; }
int ipx_bind(struct ipx_stack *s, unsigned i, const struct ciuki_sockaddr_ipx *a)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(e->bound)return -EINVAL;
    if(a->family!=AF_IPX || a->reserved)return -EAFNOSUPPORT;
    if((!zero(a->node,6) && memcmp(a->node,s->node,6)) ||
       (!zero(a->network,4) && memcmp(a->network,s->network,4)))return -EADDRNOTAVAIL;
    uint16_t number=a->socket;
    if(number && socket_busy(s,number))return -EADDRINUSE;
    if(!number) {
        unsigned tries=0;
        do { uint8_t b[2]; ipx_put16(b,s->next_socket); memcpy(&number,b,2);
            if(++s->next_socket>0x7fff)s->next_socket=0x4000;
            if(!socket_busy(s,number))break;
        } while(++tries<0x4000);
        if(tries==0x4000)return -EADDRINUSE;
    }
    e->local=*a; e->local.socket=number; memcpy(e->local.node,s->node,6);
    memcpy(e->local.network,s->network,4); e->bound=true; return 0;
}
static int autobind(struct ipx_stack *s,unsigned i)
{ struct ciuki_sockaddr_ipx a={.family=AF_IPX}; return s->endpoint[i].bound ? 0 : ipx_bind(s,i,&a); }
static int remote_check(struct ipx_stack *s,const struct ciuki_sockaddr_ipx *a)
{
    if(a->family!=AF_IPX || a->reserved)return -EAFNOSUPPORT;
    if(!a->socket || zero(a->node,6))return -EADDRNOTAVAIL;
    if(!zero(a->network,4) && memcmp(a->network,s->network,4))return -ENETUNREACH;
    return 0;
}
static unsigned header(uint8_t *p,struct ipx_endpoint *e,const struct ciuki_sockaddr_ipx *to,unsigned data,unsigned ptype)
{
    memset(p,0,44); memcpy(p,to->node,6); memcpy(p+6,e->local.node,6); ipx_put16(p+12,0x8137);
    p[14]=p[15]=255; ipx_put16(p+16,30+data); p[19]=ptype;
    write_address(p+20,to); write_address(p+32,&e->local); return 44;
}
static int emit_as(struct ipx_stack *s,const uint8_t *p,unsigned n,unsigned f)
{
    /* Local native loopback precedes the wire/VM fanout, exactly once. */
    if(ipx_all(p) || !memcmp(p,s->node,6))ipx_input(s,p,n);
    if(!memcmp(p,s->node,6))return 0;
    uint8_t wire[1514]; int bytes=ipx_encode(wire,p,n,f);
    return bytes<0 ? bytes : s->send ? s->send(s->arg,wire,(unsigned)bytes) : -ENETDOWN;
}
static int emit(struct ipx_stack *s,const uint8_t *p,unsigned n)
{ return emit_as(s,p,n,s->framing); }
static unsigned window(struct ipx_endpoint *e)
{ return (uint16_t)(e->expected+(e->shut_read ? IPX_QUEUE : IPX_QUEUE-e->count)-1); }
static int spx_packet(struct ipx_stack *s,struct ipx_endpoint *e,unsigned ctrl,unsigned stream,
                      const void *data,unsigned bytes,bool reliable)
{
    uint8_t p[576+14]; header(p,e,&e->peer,12+bytes,5);
    p[44]=ctrl; p[45]=stream; ipx_put16(p+46,e->id); ipx_put16(p+48,e->state==SPX_CONNECTING?0xffff:e->remote_id);
    ipx_put16(p+50,e->sequence); ipx_put16(p+52,e->expected); ipx_put16(p+54,window(e));
    if(bytes)memcpy(p+56,data,bytes);
    if(reliable) {
        if(e->tx)return -EAGAIN;
        e->tx=net_packet_copy(p,56+bytes); if(!e->tx)return -ENOBUFS;
        e->retries=0; e->due=s->now+250;
    }
    int result=emit(s,p,56+bytes);
    /* A committed reliable send survives transient device pressure. */
    return reliable ? 0 : result;
}
int ipx_connect(struct ipx_stack *s,unsigned i,const struct ciuki_sockaddr_ipx *a)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(e->error)return -e->error;
    if(e->connect_error && e->type!=SOCK_DGRAM)return -e->connect_error;
    if(e->type==SOCK_DGRAM && a->family==AF_UNSPEC) { e->connected=false; return 0; }
    int r=remote_check(s,a); if(r)return r;
    if(e->state==SPX_CONNECTING)return -EALREADY;
    if(e->type!=SOCK_DGRAM && e->state!=SPX_IDLE)return -EISCONN;
    if(e->type!=SOCK_DGRAM && ipx_all(a->node))return -EADDRNOTAVAIL;
    if(!s->link)return -ENETDOWN;
    if((r=autobind(s,i)))return r;
    e->peer=*a;
    if(e->type==SOCK_DGRAM) { e->connected=true; return 0; }
    e->state=SPX_CONNECTING; e->last_rx=s->now;
    r=spx_packet(s,e,SPX_SYS|SPX_ACK,0,NULL,0,true);
    if(r) { e->state=SPX_IDLE; return r; }
    return e->connected ? 0 : -EINPROGRESS;
}
int ipx_listen(struct ipx_stack *s,unsigned i,unsigned backlog)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(e->error)return -e->error;
    if(e->type==SOCK_DGRAM || !backlog)return -EOPNOTSUPP;
    if(e->state!=SPX_IDLE && e->state!=SPX_LISTEN)return -EINVAL;
    int r=autobind(s,i); if(r)return r;
    e->state=SPX_LISTEN; e->backlog=backlog>IPX_QUEUE?IPX_QUEUE:backlog; return 0;
}
int ipx_accept(struct ipx_stack *s,unsigned i)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(e->error)return -e->error;
    if(e->state!=SPX_LISTEN)return -EINVAL;
    for(unsigned j=0;j<IPX_ENDPOINTS;j++)if(s->endpoint[j].used && s->endpoint[j].parent==i+1 && s->endpoint[j].state==SPX_ESTABLISHED) {
        s->endpoint[j].parent=0; return (int)j;
    }
    return -EAGAIN;
}
int ipx_send(struct ipx_stack *s,unsigned i,const void *data,unsigned bytes,const struct ciuki_sockaddr_ipx *to)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(e->error)return -e->error;
    if(!s->link)return -ENETDOWN;
    if(e->shut_write)return -EPIPE;
    int r=autobind(s,i); if(r)return r;
    if(e->type==SOCK_DGRAM) {
        if(!to) { if(!e->connected)return -EDESTADDRREQ; to=&e->peer; }
        if((r=remote_check(s,to)))return r;
        if(ipx_all(to->node) && !e->broadcast)return -EACCES;
        /* Fits every supported frame format without per-route size changes. */
        if(bytes>1462)return -EMSGSIZE;
        uint8_t p[1514]; header(p,e,to,bytes,e->packet_type);
        if(bytes)memcpy(p+44,data,bytes);
        r=emit(s,p,44+bytes); return r ? r : (int)bytes;
    }
    if(to && !same_peer(to,&e->peer))return -EISCONN;
    if(!e->connected || e->state!=SPX_ESTABLISHED)return -ENOTCONN;
    if(e->tx || (int16_t)(e->sequence-e->allocation)>0)return -EAGAIN;
    if(bytes>SPX_DATA_MAX) { if(e->type==SOCK_SEQPACKET)return -EMSGSIZE; bytes=SPX_DATA_MAX; }
    if(!bytes && e->type==SOCK_STREAM)return 0;
    r=spx_packet(s,e,SPX_ACK|SPX_EOM,e->datastream,data,bytes,true); return r ? r : (int)bytes;
}
int ipx_recv(struct ipx_stack *s,unsigned i,void *data,unsigned bytes,unsigned flags,
             struct ciuki_sockaddr_ipx *from,unsigned *out)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(out)*out=0;
    if(e->error)return -e->error;
    if(!s->link)return -ENETDOWN;
    if(e->shut_read)return 0;
    if(!e->bound)return -EINVAL;
    if(e->type!=SOCK_DGRAM && !e->connected && !e->eof)return -ENOTCONN;
    if(e->type==SOCK_STREAM && !bytes)return 0;
    if(!e->count)return e->eof ? 0 : -EAGAIN;
    uint8_t *p=net_packet_data(e->rx[e->head]);
    unsigned offset=e->type==SOCK_DGRAM?44:56, size=ipx_be16(p+16)+14-offset;
    unsigned available=size-e->offset, n=bytes<available?bytes:available;
    if(n)memcpy(data,p+offset+e->offset,n);
    if(from)read_address(from,p+32);
    if(n<available && e->type!=SOCK_STREAM && out)*out=MSG_TRUNC;
    if(!(flags&MSG_PEEK)) {
        if(e->type==SOCK_STREAM && n<available)e->offset+=n;
        else {
            net_packet_free(e->rx[e->head]); e->head=(e->head+1)%IPX_QUEUE; e->count--; e->offset=0;
            if(e->type!=SOCK_DGRAM)spx_packet(s,e,SPX_SYS,0,NULL,0,false);
        }
    }
    return (int)n;
}
static bool enqueue(struct ipx_endpoint *e,const uint8_t *p,unsigned n)
{
    if(e->count==IPX_QUEUE)return false;
    struct net_frame *copy=net_packet_copy(p,n); if(!copy)return false;
    e->rx[(e->head+e->count++)%IPX_QUEUE]=copy; return true;
}
static void learn(struct ipx_stack *s,const uint8_t *p,unsigned f,unsigned bytes)
{
    /* Only a valid directly attached RIP response is a discovery vote. */
    if(s->now>s->discovery_end || p[19]!=1 || ipx_be16(p+30)!=0x453 ||
       ipx_be16(p+42)!=0x453 || bytes<54 || ipx_be16(p+44)!=2 ||
       (bytes-46)%8 || zero(p+32,4))return;
    for(unsigned i=0;i<s->routers;i++)if(s->router_framing[i]==f && !memcmp(s->router_node[i],p+6,6))return;
    if(s->routers==16)return;
    if(!s->scores[f])memcpy(s->networks[f],p+32,4);
    if(memcmp(s->networks[f],p+32,4))return;
    memcpy(s->router_node[s->routers],p+6,6); s->router_framing[s->routers++]=f; s->scores[f]++;
    unsigned best=0; for(unsigned j=1;j<IPX_FRAMINGS;j++)if(s->scores[j]>s->scores[best])best=j;
    if(!s->scores[best])return;
    s->framing=best;
    memcpy(s->network,s->networks[best],4);
    for(unsigned i=0;i<IPX_ENDPOINTS;i++)if(s->endpoint[i].used && s->endpoint[i].bound)
        memcpy(s->endpoint[i].local.network,s->network,4);
}
static void input_canonical(struct ipx_stack *s,const uint8_t *p,unsigned bytes,unsigned f)
{
    if(!s->link || (!ipx_all(p) && memcmp(p,s->node,6)))return;
    learn(s,p,f,bytes);
    if(!zero(p+20,4) && !zero(s->network,4) && memcmp(p+20,s->network,4))return;
    struct ciuki_sockaddr_ipx peer; read_address(&peer,p+32);
    for(unsigned i=0;i<IPX_ENDPOINTS;i++) {
        struct ipx_endpoint *e=&s->endpoint[i];
        if(!e->used || !e->bound || e->type!=SOCK_DGRAM || memcmp(&e->local.socket,p+30,2) ||
           (ipx_all(p) && !e->receive_broadcast) || (e->filtering && e->filter!=p[19]) ||
           (e->connected && !same_peer(&e->peer,&peer)))continue;
        enqueue(e,p,bytes);
    }
    if(p[19]==5 && bytes>=56 && !ipx_all(p)) {
        unsigned ctrl=p[44], dst=ipx_be16(p+48), src=ipx_be16(p+46);
        if(ctrl&0x0f || !src || src==0xffff)return; /* classic SPX only */
        struct ipx_endpoint *e=NULL;
        for(unsigned i=0;i<IPX_ENDPOINTS;i++) {
            struct ipx_endpoint *v=&s->endpoint[i];
            if(v->used && v->type!=SOCK_DGRAM && v->bound && !memcmp(&v->local.socket,p+30,2) &&
               ((dst==v->id && (same_peer(&v->peer,&peer) ||
                   (v->state==SPX_CONNECTING && !memcmp(v->peer.node,peer.node,6) &&
                    (zero(v->peer.network,4) || zero(peer.network,4) || !memcmp(v->peer.network,peer.network,4))))) ||
                (dst==0xffff && v->remote_id==src && same_peer(&v->peer,&peer)))) { e=v; break; }
        }
        if(!e && dst==0xffff && ctrl==(SPX_SYS|SPX_ACK) && bytes==56 && !ipx_be16(p+50)) {
            for(unsigned i=0;i<IPX_ENDPOINTS;i++) {
                struct ipx_endpoint *l=&s->endpoint[i];
                if(!l->used || l->state!=SPX_LISTEN || memcmp(&l->local.socket,p+30,2))continue;
                unsigned pending=0; for(unsigned j=0;j<IPX_ENDPOINTS;j++)pending+=s->endpoint[j].used && s->endpoint[j].parent==i+1;
                if(pending>=l->backlog)return;
                int child=ipx_open(s,l->type,NSPROTO_SPX); if(child<0)return;
                e=&s->endpoint[child]; e->local=l->local; e->peer=peer; e->bound=e->connected=true;
                e->state=SPX_ESTABLISHED; e->remote_id=src; e->datastream=l->datastream; e->abortive=l->abortive; e->parent=i+1; e->last_rx=s->now;
                e->allocation=ipx_be16(p+54); spx_packet(s,e,SPX_SYS,0,NULL,0,false); return;
            }
        }
        if(!e)return;
        if(e->state==SPX_CONNECTING) {
            if(!(ctrl&SPX_SYS) || (ctrl&SPX_ACK) || dst!=e->id)return;
            if(e->tx)net_packet_free(e->tx); e->tx=NULL; e->remote_id=src; e->peer=peer;
            e->state=SPX_ESTABLISHED; e->connected=true; e->allocation=ipx_be16(p+54);
        } else if(src!=e->remote_id)return;
        e->last_rx=s->now; e->probes=0;
        uint16_t allocation=(uint16_t)ipx_be16(p+54);
        if(dst!=0xffff && (int16_t)(allocation-e->allocation)>=0)e->allocation=allocation;
        unsigned ack=ipx_be16(p+52);
        if(e->tx && e->state!=SPX_CONNECTING && ack==(uint16_t)(e->sequence+1)) {
            net_packet_free(e->tx); e->tx=NULL; e->sequence++;
        }
        if(p[45]==0xff) {
            if(e->state==SPX_END && !e->tx && ack==e->sequence) {
                e->state=SPX_DEAD; e->eof=true; if(e->detached)destroy(e);
            }
            return;
        }
        if(dst==0xffff) { spx_packet(s,e,SPX_SYS,0,NULL,0,false); return; }
        if(!(ctrl&SPX_SYS)) {
            unsigned seq=ipx_be16(p+50);
            if(p[45]==0xfe) {
                if(bytes!=56)return;
                if(seq==e->expected) { e->expected++; e->eof=true; }
                if(e->eof && seq==(uint16_t)(e->expected-1))spx_packet(s,e,SPX_EOM,0xff,NULL,0,false);
                else spx_packet(s,e,SPX_SYS,0,NULL,0,false);
                return;
            }
            if(seq==e->expected && !e->eof && (e->shut_read || enqueue(e,p,bytes)))e->expected++;
            /* Future and duplicate packets never consume a receive slot. */
            spx_packet(s,e,SPX_SYS,0,NULL,0,false);
        } else if(ctrl&SPX_ACK)spx_packet(s,e,SPX_SYS,0,NULL,0,false);
        return;
    }

}
void ipx_input(struct ipx_stack *s,const uint8_t *p,unsigned n)
{
    unsigned f=0, off=ipx_decode(p,n,&f); if(!off)return;
    if(off==14)input_canonical(s,p,14+ipx_be16(p+16),f);
    else {
        uint8_t canonical[1514]; int bytes=ipx_encode(canonical,p,n,IPX_II);
        if(bytes>0)input_canonical(s,canonical,14+ipx_be16(canonical+16),f);
    }
}
static void end_connection(struct ipx_stack *s,struct ipx_endpoint *e)
{
    if(e->state==SPX_ESTABLISHED && !e->tx && e->shut_write) {
        e->state=SPX_END;
        if(spx_packet(s,e,SPX_ACK|SPX_EOM,0xfe,NULL,0,true))e->state=SPX_ESTABLISHED;
    }
}
void ipx_close(struct ipx_stack *s,unsigned i)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return;
    if(e->state==SPX_LISTEN)for(unsigned j=0;j<IPX_ENDPOINTS;j++)if(s->endpoint[j].used && s->endpoint[j].parent==i+1)destroy(&s->endpoint[j]);
    if(e->type==SOCK_DGRAM || e->state==SPX_IDLE || e->state==SPX_LISTEN || e->state==SPX_DEAD || e->state==SPX_CONNECTING) { destroy(e); return; }
    e->detached=true; e->shut_write=true; e->shut_read=true;
    while(e->count) { net_packet_free(e->rx[e->head]); e->head=(e->head+1)%IPX_QUEUE; e->count--; }
    if(e->abortive) {
        /* SPX has no TCP RST bit: terminate immediately without draining data. */
        if(e->tx)net_packet_free(e->tx); e->tx=NULL;
        spx_packet(s,e,SPX_ACK|SPX_EOM,0xfe,NULL,0,false); destroy(e); return;
    }
    end_connection(s,e);
}
int ipx_shutdown(struct ipx_stack *s,unsigned i,unsigned how)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(how>2)return -EINVAL;
    if(!e->connected || e->type==SOCK_DGRAM)return -ENOTCONN;
    if(how!=SHUT_WR) {
        e->shut_read=true; while(e->count) { net_packet_free(e->rx[e->head]); e->head=(e->head+1)%IPX_QUEUE; e->count--; }
    }
    if(how!=SHUT_RD)e->shut_write=true;
    end_connection(s,e); return 0;
}
void ipx_tick(struct ipx_stack *s,uint64_t now)
{
    if(now<s->now)return;
    s->now=now; if(!s->link)return;
    if(!s->probed) {
        s->probed=true; s->discovery_end=now+1000;
        struct ipx_endpoint probe={0}; probe.local.family=AF_IPX;
        memcpy(probe.local.node,s->node,6); uint8_t socket[2]={4,0x53}; memcpy(&probe.local.socket,socket,2);
        struct ciuki_sockaddr_ipx to=probe.local; memset(to.node,255,6);
        uint8_t p[54]; header(p,&probe,&to,10,1); ipx_put16(p+44,1); memset(p+46,255,4); memset(p+50,0xff,4);
        for(unsigned f=0;f<IPX_FRAMINGS;f++)emit_as(s,p,sizeof(p),f);
    }
    for(unsigned i=0;i<IPX_ENDPOINTS;i++) {
        struct ipx_endpoint *e=&s->endpoint[i];
        if(!e->used || e->type==SOCK_DGRAM)continue;
        if(e->tx && now>=e->due) {
            if(e->retries++>=SPX_RETRIES) { failed(e,ETIMEDOUT); if(e->detached)destroy(e); continue; }
            uint8_t copy[590]; uint8_t *p=net_packet_data(e->tx); unsigned n=14+ipx_be16(p+16);
            memcpy(copy,p,n); ipx_put16(copy+52,e->expected); ipx_put16(copy+54,window(e));
            e->due=now+(e->retries<4 ? 250u<<e->retries : 4000u); emit(s,copy,n);
        }
        if(!e->used)continue;
        end_connection(s,e);
        if(e->state==SPX_ESTABLISHED && now-e->last_rx>=SPX_IDLE_MS && now>=e->watchdog) {
            if(e->probes++>=3) { failed(e,ETIMEDOUT); if(e->detached)destroy(e); continue; }
            e->watchdog=now+SPX_WATCH_MS; spx_packet(s,e,SPX_SYS|SPX_ACK,0,NULL,0,false);
        }
        if(e->parent && now-e->last_rx>=30000)destroy(e);
    }
}
void ipx_link(struct ipx_stack *s,bool up)
{
    if(s->link==up)return;
    s->link=up;
    if(!up) { for(unsigned i=0;i<IPX_ENDPOINTS;i++)if(s->endpoint[i].used) {
        failed(&s->endpoint[i],ENETDOWN); if(s->endpoint[i].detached)destroy(&s->endpoint[i]);
    } }
    else { s->probed=false; s->routers=0; memset(s->scores,0,sizeof(s->scores)); }
}
int ipx_events(struct ipx_stack *s,unsigned i)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return POLLNVAL;
    int events=e->count || e->eof || e->shut_read ? POLLIN : 0;
    if(e->state==SPX_LISTEN)for(unsigned j=0;j<IPX_ENDPOINTS;j++)if(s->endpoint[j].used && s->endpoint[j].parent==i+1 && s->endpoint[j].state==SPX_ESTABLISHED)events|=POLLIN;
    if(e->type==SOCK_DGRAM || (e->connected && !e->tx && (int16_t)(e->sequence-e->allocation)<=0) || e->error || e->shut_write)events|=POLLOUT;
    if(e->error)events|=POLLERR|POLLHUP;
    if(e->state==SPX_DEAD || e->eof)events|=POLLIN|POLLHUP;
    return events;
}
int ipx_option(struct ipx_stack *s,unsigned i,bool set,int level,unsigned name,void *value,unsigned *length)
{
    struct ipx_endpoint *e=endpoint(s,i); if(!e)return -EBADF;
    if(*length<4)return -EINVAL;
    unsigned v=0, needed=4;
    if(*length>=4)memcpy(&v,value,4);
    if(level==SOL_SOCKET && name==SO_LINGER) {
        if(*length<sizeof(struct ciuki_socket_linger))return -EINVAL;
        struct ciuki_socket_linger l; memcpy(&l,value,sizeof(l));
        if(set) { if(l.seconds<0)return -EINVAL; if(l.on && l.seconds)return -EOPNOTSUPP; e->abortive=l.on && !l.seconds; }
        else { l=(struct ciuki_socket_linger){e->abortive,0}; memcpy(value,&l,sizeof(l)); *length=sizeof(l); }
        return 0;
    }
    if(level==SOL_SOCKET) {
        if(name==SO_ERROR) { if(set)return -ENOPROTOOPT; v=e->error; e->error=0; }
        else if(name==SO_TYPE) { if(set)return -ENOPROTOOPT; v=e->type; }
        else if(name==SO_BROADCAST) { if(set)e->broadcast=!!v; v=e->broadcast; }
        else if(name==SO_RX_BYTES) {
            if(set)return -ENOPROTOOPT;
            v=e->count ? ipx_be16(net_packet_data(e->rx[e->head])+16)-(e->type==SOCK_DGRAM?30:42)-e->offset : 0;
        } else return -ENOPROTOOPT;
    } else if(level==NSPROTO_IPX || level==NSPROTO_SPX) {
        switch(name) {
        case IPX_PTYPE: case IPX_FILTERPTYPE: case IPX_DSTYPE:
            if((name!=IPX_DSTYPE && e->type!=SOCK_DGRAM) || (name==IPX_DSTYPE && e->type==SOCK_DGRAM))return -ENOPROTOOPT;
            if(set && (v>255 || (name==IPX_DSTYPE && v>=0xfe)))return -EINVAL;
            if(name==IPX_PTYPE) { if(set)e->packet_type=v; v=e->packet_type; }
            if(name==IPX_FILTERPTYPE) { if(set) { e->filter=v; e->filtering=true; } v=e->filter; }
            if(name==IPX_DSTYPE) { if(set)e->datastream=v; v=e->datastream; }
            break;
        case IPX_STOPFILTERPTYPE: if(!set)return -ENOPROTOOPT; e->filtering=false; break;
        case IPX_RECEIVE_BROADCAST: if(!set)return -ENOPROTOOPT; e->receive_broadcast=!!v; break;
        case IPX_IMMEDIATESPXACK: if(!set || e->type==SOCK_DGRAM)return -ENOPROTOOPT; break;
        case IPX_MAXSIZE: if(set)return -ENOPROTOOPT; v=e->type==SOCK_DGRAM?1462:SPX_DATA_MAX; break;
        case IPX_MAX_ADAPTER_NUM: if(set)return -ENOPROTOOPT; v=1; break;
        case IPX_ADDRESS: {
            needed=sizeof(struct ciuki_ipx_address_data); if(set)return -ENOPROTOOPT;
            if(*length<needed)return -EINVAL;
            if(v)return -EINVAL;
            struct ciuki_ipx_address_data a={.maxpkt=1462,.linkspeed=s->linkspeed};
            memcpy(a.netnum,s->network,4); memcpy(a.nodenum,s->node,6); memcpy(value,&a,needed); *length=needed; return 0;
        }
        case IPX_GETNETINFO: case IPX_GETNETINFO_NORIP: {
            needed=sizeof(struct ciuki_ipx_netnum_data); if(set)return -ENOPROTOOPT;
            if(*length<needed)return -EINVAL;
            struct ciuki_ipx_netnum_data a; memcpy(&a,value,needed);
            if(!zero(a.netnum,4) && memcmp(a.netnum,s->network,4))return -ENETUNREACH;
            memset(&a,0,sizeof(a)); memcpy(a.netnum,s->network,4); memcpy(value,&a,needed); *length=needed; return 0;
        }
        default:return -ENOPROTOOPT;
        }
    } else return -ENOPROTOOPT;
    if(*length<needed)return -EINVAL;
    if(!set) { memcpy(value,&v,4); *length=4; }
    return 0;
}
