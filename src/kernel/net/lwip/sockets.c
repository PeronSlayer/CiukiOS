/* Ciuki native sockets over the pinned sequential API. Public fds are never
 * lwIP descriptors. One worker owns normal calls; control owns teardown.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include <ciuki/net_stack.h>
#include <ciuki/files.h>
#include <ciuki/desktop.h>
#include <ciuki/signal.h>
#include <ciuki/kernel.h>
#include <ciuki/security_device.h>
#include <ciuki/ipx.h>

#define REQUESTS 8u
struct socket_object {
    struct proc_object object;
    struct netconn *conn;
    struct pbuf *tcp_rx;
    struct netbuf *udp_rx;
    unsigned references, active, holds, watchers, generation, flags, type, rx_offset;
    unsigned receive_ms, send_ms, shut, control_shut;
    unsigned family, protocol, ipx;
    int receive_events, send_events, error, connect_result;
    uint32_t route_address;
    struct ciuki_sockaddr_in local, peer;
    struct process *origin;
    uint32_t origin_pid;
    uint32_t security_uid, security_session;
    bool pending, used, closing, control, connecting, connected, listening, eof, failed, network_down;
};
static bool socket_subject(const struct process *p,const struct socket_object *s)
{
    const struct ciuki_credential *c=security_credential(p);
    return security_process_live(p) && c && c->uid==s->security_uid && c->session_id==s->security_session;
}
static struct socket_object sockets[CIUKI_SOCKET_MAX];
static unsigned object_generation, tcp_count, udp_count, listeners;
static sys_mbox_t commands;
static bool workers_ready;
/* Protected counters, drained by the already allocated network watch message.
 * At most one summary per category per ten-second window, no new timers. */
struct tcp_record {
    unsigned count,attempts,established,failed,pid;
    uint32_t address; unsigned port; int error;
    uint64_t due;
};
static struct tcp_record tcp_records[3];
static void tcp_record(struct socket_object *s,unsigned kind,int error,unsigned state)
{
    if(s->type!=SOCK_STREAM)return;
    sys_prot_t f=sys_arch_protect();struct tcp_record *r=&tcp_records[kind];
    if(r->count!=UINT32_MAX)r->count++;
    if(!state && r->attempts!=UINT32_MAX)r->attempts++;
    if(state==1 && r->established!=UINT32_MAX)r->established++;
    if(state==2 && r->failed!=UINT32_MAX)r->failed++;
    r->pid=s->origin_pid;r->address=s->peer.address;r->port=lwip_ntohs(s->peer.port);r->error=error;
    sys_arch_unprotect(f);
}
void ciuki_tcp_records(void)
{
    static const char *const names[]={"connect","reset","timeout"};
    uint64_t now=cu_now();
    for(unsigned i=0;i<3;i++){
        sys_prot_t f=sys_arch_protect();struct tcp_record r=tcp_records[i];
        bool emit=r.count && now>=r.due;
        if(emit){tcp_records[i]=(struct tcp_record){.due=now+10000};}
        sys_arch_unprotect(f);
        if(!emit)continue;
        const uint8_t *ip=(const uint8_t *)&r.address;
        klog("[net] tcp %s count=%u attempts=%u established=%u failed=%u last_ip=%u.%u.%u.%u port=%u errno=%d pid=%u window_ms=10000",
             names[i],r.count,r.attempts,r.established,r.failed,ip[0],ip[1],ip[2],ip[3],r.port,r.error,r.pid);
    }
}
enum operation { OP_NEW, OP_BIND, OP_CONNECT, OP_LISTEN, OP_ACCEPT, OP_SEND, OP_RECV, OP_NAME, OP_OPTION };
struct request {
    enum operation operation;
    struct socket_object *socket;
    struct netconn *accepted;
    struct socket_object *created;
    struct process *reserved_process;
    int reserved_fd;
    struct ciuki_sockaddr_in address;
    unsigned bytes, flags, output_flags, option, level, length, which;
    int result;
    bool used, submitted, done, abandoned, has_address;
    void (*outer_cleanup)(void *);
    void *outer_operation;
    struct proc_thread *thread;
    struct process *process;
    int fd;
    unsigned fd_generation;
    uint64_t deadline;
    int cancelled;
    bool committed;
    uint8_t buffer[CIUKI_SOCKET_IO_MAX];
};
static struct request requests[REQUESTS];
#ifdef LWIP_HOST
void cu_socket_test_hook(unsigned) __attribute__((weak));
#define TEST_HOOK(n) do { if (cu_socket_test_hook) cu_socket_test_hook(n); } while (0)
#else
#define TEST_HOOK(n) ((void)0)
#endif

uint32_t net_socket_count(void)
{
    sys_prot_t f = sys_arch_protect(); unsigned n = 0;
    for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++) n += sockets[i].used;
    sys_arch_unprotect(f); return n;
}
void net_socket_snapshot(struct ciuki_net_socket_list *list)
{
    memset(list,0,sizeof(*list));list->size=sizeof(*list);list->version=1;
    const struct process *caller=proc_current();
    const struct ciuki_credential *credential=security_credential(caller);
    if (!security_process_live(caller) || !credential) return;
    bool system=security_device_system(caller), admin=security_device_admin(caller);
    sys_prot_t f=sys_arch_protect();
    for(unsigned i=0;i<CIUKI_SOCKET_MAX;i++) {
        struct socket_object *s=&sockets[i];if(!s->used || s->pending || s->closing || s->family==AF_IPX)continue;
        if (!system && (s->security_uid==CIUKI_UID_SYSTEM ||
            (!admin && (s->security_uid!=credential->uid || s->security_session!=credential->session_id)))) continue;
        if (!system && !admin) {
            bool owned=false;
            for (unsigned fd=0;fd<CIUKI_OPEN_MAX && !owned;fd++)
                owned=file_fd_object(caller,(int)fd)==&s->object;
            if (!owned) continue;
        }
        struct ciuki_net_socket_info *out=&list->sockets[list->count++];
        out->pid=s->origin_pid;out->type=s->type;
        out->state=s->listening?CIUKI_NET_SOCKET_LISTEN:s->connecting?CIUKI_NET_SOCKET_CONNECTING:
            s->connected?(s->eof?CIUKI_NET_SOCKET_CLOSE_WAIT:CIUKI_NET_SOCKET_CONNECTED):CIUKI_NET_SOCKET_CLOSED;
        out->local_address=s->local.address;out->local_port=s->local.port;
        out->peer_address=s->peer.address;out->peer_port=s->peer.port;
    }
    sys_arch_unprotect(f);
}
static void retain(struct proc_object *o)
{
    sys_prot_t f = sys_arch_protect(); ((struct socket_object *)o)->references++; sys_arch_unprotect(f);
}
static void release(struct proc_object *o)
{
    sys_prot_t f = sys_arch_protect(); struct socket_object *s = (void *)o;
    if (!--s->references) s->closing = true;
    sys_arch_unprotect(f); cu_changed();
}
bool net_socket_object(struct proc_object *o) { return o && o->release == release; }
uint32_t *net_socket_flags(struct proc_object *o) { return net_socket_object(o) ? &((struct socket_object *)o)->flags : NULL; }
void net_fd_changed(struct process *p, unsigned fd)
{
    sys_prot_t f = sys_arch_protect();
    if (p && fd < CIUKI_OPEN_MAX) p->fd_generation[fd]++;
    sys_arch_unprotect(f);
    cu_changed();
}
void net_input_changed(void) { cu_changed_irq(); }
static int error_number(err_t e)
{
    switch (e) {
    case ERR_OK: return 0;
    case ERR_MEM: return -ENOBUFS;
    case ERR_BUF: return -ENOBUFS;
    case ERR_TIMEOUT: return -ETIMEDOUT;
    case ERR_RTE: return -ENETUNREACH;
    case ERR_INPROGRESS: return -EINPROGRESS;
    case ERR_VAL: case ERR_ARG: return -EINVAL;
    case ERR_WOULDBLOCK: return -EAGAIN;
    case ERR_USE: return -EADDRINUSE;
    case ERR_ALREADY: return -EALREADY;
    case ERR_ISCONN: return -EISCONN;
    case ERR_CONN: return -ENOTCONN;
    case ERR_IF: return -ENETDOWN;
    case ERR_ABRT: return -ECONNABORTED;
    case ERR_RST: return -ECONNRESET;
    case ERR_CLSD: return -EPIPE;
    default: return -ENETDOWN;
    }
}
static void notify(struct netconn *conn, enum netconn_evt evt, u16_t length)
{
    (void)length;
    sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++) {
        struct socket_object *s = &sockets[i]; if (!s->used || s->conn != conn) continue;
        if (evt == NETCONN_EVT_RCVPLUS) s->receive_events++;
        if (evt == NETCONN_EVT_RCVMINUS) s->receive_events--;
        if (evt == NETCONN_EVT_SENDPLUS) { s->send_events = 1; if (s->connecting) { s->connecting = false; s->connected = true; s->connect_result = 0; tcp_record(s,0,0,1); } }
        if (evt == NETCONN_EVT_SENDMINUS) s->send_events = 0;
        if (evt == NETCONN_EVT_ERROR) {
            int e = error_number(conn->pending_err);
            /* SYN refused is RST before a connection has been established. */
            if (s->connecting && e == -ECONNRESET) e = -ECONNREFUSED;
            /* tcp_slowtmr reports exhausted SYN retries as ERR_ABRT. Our
             * adapter's local teardown is marked control/closing/down. */
            if(s->connecting && e==-ECONNABORTED && !s->control && !s->closing && !s->network_down)e=-ETIMEDOUT;
            if (e) {
                if(s->connecting)tcp_record(s,0,-e,2);
                if(conn->pending_err==ERR_RST)tcp_record(s,1,-e,2);
                if(e==-ETIMEDOUT)tcp_record(s,2,-e,2);
                s->error = -e; s->failed = true;
                if (s->connecting) s->connect_result = e;
            } else {
                /* api_msg also announces successful full shutdown as ERROR. */
                s->eof = true; s->shut |= 3;
            }
            s->connecting = false;
        }
        break;
    }
    /* Accepted PCBs may notify before netconn_accept returns. Readiness is
     * reconstructed from the child's receive mailbox at installation. */
    sys_arch_unprotect(f); cu_changed();
}
static void reclaim_request(struct request *r)
{
    if (r->created) {
        r->created->pending = false;
        if (r->created != r->socket) r->created->holds--;
        release(&r->created->object);
    }
    if (r->reserved_process) file_fd_cancel(r->reserved_process, r->reserved_fd);
    if (r->socket) r->socket->holds--;
    r->used = false;
}
static void abandon(void *arg)
{
    struct request *r = arg;
    void (*outer)(void *) = r->outer_cleanup; void *outer_arg = r->outer_operation;
    sys_prot_t f = sys_arch_protect();
    if (r->reserved_process) { file_fd_cancel(r->reserved_process, r->reserved_fd); r->reserved_process = NULL; }
    r->abandoned = true;
    if (!r->submitted || r->done) reclaim_request(r);
    sys_arch_unprotect(f);
    if (outer) outer(outer_arg);
    cu_changed();
}
static struct request *request_new(struct socket_object *s)
{
    sys_prot_t f = sys_arch_protect(); struct request *r = NULL;
    for (unsigned i = 0; i < REQUESTS; i++) if (!requests[i].used) {
        r = &requests[i]; memset(r, 0, __builtin_offsetof(struct request, buffer));
        r->used = true; r->socket = s; if (s) s->holds++;
        r->deadline = UINT64_MAX;
        r->thread = proc_thread_for(g_current);
        r->outer_cleanup = r->thread ? r->thread->cleanup : NULL;
        r->outer_operation = r->thread ? r->thread->operation : NULL;
        if (r->thread) { r->thread->cleanup = abandon; r->thread->operation = r; }
        break;
    }
    sys_arch_unprotect(f); return r;
}
static void request_free(struct request *r)
{
    sys_prot_t f = sys_arch_protect();
    if (r->thread) { r->thread->cleanup = r->outer_cleanup; r->thread->operation = r->outer_operation; }
    reclaim_request(r); sys_arch_unprotect(f); cu_changed();
}
static bool interrupted(struct process *p)
{
    struct proc_thread *t = proc_thread_for(g_current);
    return p->state == PROC_STOPPING || (t && proc_signal_caught(t));
}
static struct socket_object *lookup(struct process *p, int fd, int *error)
{
    struct proc_object *o = file_fd_object(p, fd);
    *error = !o ? -EBADF : !net_socket_object(o) ? -ENOTSOCK : 0;
    if (!*error && !socket_subject(p,(void *)o)) *error=-EACCES;
    return *error ? NULL : (void *)o;
}
static bool still_open(struct process *p, int fd, unsigned generation, struct socket_object *s)
{
    return socket_subject(p,s) && p->state != PROC_STOPPING && p->fd_generation[fd] == generation &&
           file_fd_object(p, fd) == &s->object && !s->closing;
}
int net_socket_capture(struct process *p, int fd, struct net_socket_ref *ref)
{
    sys_prot_t f = sys_arch_protect(); int e;
    struct socket_object *s = lookup(p, fd, &e);
    if (s) { s->holds++; ref->object = &s->object; ref->generation = p->fd_generation[fd]; }
    sys_arch_unprotect(f); return e;
}
void net_socket_drop(struct net_socket_ref *ref)
{
    sys_prot_t f = sys_arch_protect();
    if (ref->object) { ((struct socket_object *)ref->object)->holds--; ref->object = NULL; }
    sys_arch_unprotect(f); cu_changed();
}
static struct socket_object *lookup_captured(struct process *p, int fd, int *error,
                                            const struct net_socket_ref *ref)
{
    if (!ref) return lookup(p, fd, error);
    struct socket_object *s = (void *)ref->object;
    *error = !s || !still_open(p, fd, ref->generation, s) ? -EBADF : 0;
    return *error ? NULL : s;
}
/* Called under the port's short protection, including by the worker, using
 * the submitting thread rather than the worker's current task. Preparation
 * may include internal getaddr/core waits; it is not a consuming commit. */
static int cancellation(struct request *r)
{
    if (r->abandoned) return -EINTR;
    if (r->process && !still_open(r->process, r->fd, r->fd_generation, r->socket)) return -EBADF;
    bool interruptible = r->operation == OP_CONNECT || r->operation == OP_ACCEPT ||
                         r->operation == OP_SEND || r->operation == OP_RECV;
    if (interruptible && r->thread && proc_signal_caught(r->thread)) return -EINTR;
    if (cu_now() >= r->deadline) return r->operation == OP_CONNECT ? -ETIMEDOUT : -EAGAIN;
    return 0;
}
static int commit(struct request *r)
{
    sys_prot_t f = sys_arch_protect();
    int e = r->committed ? 0 : r->cancelled ? r->cancelled : cancellation(r);
    if (!e) r->committed = true;
    sys_arch_unprotect(f); return e;
}
static void address_out(struct ciuki_sockaddr_in *out, const ip_addr_t *ip, u16_t port)
{
    *out = (struct ciuki_sockaddr_in){ .family = AF_INET, .port = lwip_htons(port), .address = ip4_addr_get_u32(ip_2_ip4(ip)) };
}
static int address_check(const struct ciuki_sockaddr_in *a, bool unspec)
{
    for (unsigned i = 0; i < 8; i++) if (a->zero[i]) return -EINVAL;
    if (a->family == AF_UNSPEC && unspec && !a->address && !a->port) return 0;
    if (a->family != AF_INET) return -EAFNOSUPPORT;
    uint32_t host = lwip_ntohl(a->address);
    if ((host & 0xf0000000u) == 0xe0000000u) return -EAFNOSUPPORT;
    return 0;
}
static void free_rx(struct socket_object *s)
{
    if (s->tcp_rx) pbuf_free(s->tcp_rx);
    if (s->udp_rx) netbuf_delete(s->udp_rx);
    s->tcp_rx = NULL; s->udp_rx = NULL; s->rx_offset = 0;
}
/* Core PCB options must be changed on tcpip_thread, even though the sequential
 * adapter itself has a single owner. No raw PCB access by a native caller. */
static void option_core(void *arg)
{
    struct request *r = arg; struct socket_object *s = r->socket;
    int32_t value = 0; memcpy(&value, r->buffer, sizeof(value));
    struct ip_pcb *pcb = s->conn->pcb.ip;
    if (!pcb) { r->result = -ENOPROTOOPT; return; }
    if (r->level == IPPROTO_TCP) {
        /* Listening PCBs use the smaller tcp_pcb_listen allocation. */
        if (s->listening) { r->result = -ENOPROTOOPT; return; }
        if (r->which) { if (value) tcp_nagle_disable(s->conn->pcb.tcp); else tcp_nagle_enable(s->conn->pcb.tcp); }
        value = tcp_nagle_disabled(s->conn->pcb.tcp) ? 1 : 0;
    } else if (r->level == IPPROTO_IP) {
        if (r->which) pcb->ttl = (u8_t)value;
        value = pcb->ttl;
    } else {
        unsigned bit = r->option == SO_REUSEADDR ? SOF_REUSEADDR : SOF_BROADCAST;
        if (r->which) { if (value) ip_set_option(pcb, bit); else ip_reset_option(pcb, bit); }
        value = ip_get_option(pcb, bit) ? 1 : 0;
    }
    memcpy(r->buffer, &value, sizeof(value));
}
static int option(struct request *r)
{
    struct socket_object *s = r->socket; int32_t value = 0;
    if (r->level == SOL_SOCKET && r->option == SO_RX_BYTES) {
        if(r->which)return -ENOPROTOOPT;
        unsigned available=0;
        if(s->type==SOCK_STREAM) {
            sys_prot_t f=sys_arch_protect();
            available=s->conn->recv_avail>0?(unsigned)s->conn->recv_avail:0;
            sys_arch_unprotect(f);
            if(s->tcp_rx)available+=s->tcp_rx->tot_len-s->rx_offset;
        } else {
            if(!s->udp_rx) {
                err_t e=netconn_recv_udp_raw_netbuf_flags(s->conn,&s->udp_rx,NETCONN_DONTBLOCK);
                if(e && e!=ERR_WOULDBLOCK)return error_number(e);
            }
            if(s->udp_rx)available=s->udp_rx->p->tot_len;
        }
        memcpy(r->buffer,&available,4);r->length=4;return 0;
    }
    if (r->level == SOL_SOCKET && (r->option == SO_TYPE || r->option == SO_ERROR)) {
        if (r->which) return -ENOPROTOOPT;
        sys_prot_t f = sys_arch_protect();
        value = r->option == SO_TYPE ? (int)s->type : s->error;
        if (r->option == SO_ERROR) s->error = 0;
        sys_arch_unprotect(f);
        memcpy(r->buffer, &value, 4); r->length = 4; return 0;
    }
    if (r->level == SOL_SOCKET && (r->option == SO_RCVTIMEO || r->option == SO_SNDTIMEO)) {
        unsigned *ms = r->option == SO_RCVTIMEO ? &s->receive_ms : &s->send_ms;
        struct ciuki_socket_timeval t = { .seconds = *ms / 1000, .microseconds = (int)(*ms % 1000) * 1000 };
        if (r->which) {
            if (r->length != sizeof(t)) return -EINVAL;
            memcpy(&t, r->buffer, sizeof(t));
            if (t.seconds < 0 || t.seconds > INT32_MAX / 1000 || t.microseconds < 0 || t.microseconds >= 1000000 || t.zero) return -EINVAL;
            uint64_t total = (uint64_t)t.seconds * 1000 + ((unsigned)t.microseconds + 999) / 1000;
            if (total > INT32_MAX) return -EINVAL;
            *ms = (unsigned)total;
        } else memcpy(r->buffer, &t, sizeof(t));
        r->length = sizeof(t); return 0;
    }
    if (!((r->level == SOL_SOCKET && (r->option == SO_REUSEADDR || r->option == SO_BROADCAST)) ||
          (r->level == IPPROTO_TCP && r->option == TCP_NODELAY && s->type == SOCK_STREAM) ||
          (r->level == IPPROTO_IP && r->option == IP_TTL))) return -ENOPROTOOPT;
    if (r->which) {
        if (r->length != 4) return -EINVAL;
        memcpy(&value, r->buffer, 4); if (r->level == IPPROTO_IP ? value < 1 || value > 255 : value != 0 && value != 1) return -EINVAL;
    }
    r->result = 0;
    err_t e = tcpip_callback_wait(option_core, r);
    r->length = 4; return e ? error_number(e) : r->result;
}
static void local_address_core(void *arg)
{
    struct request *r = arg;
    uint32_t address = r->address.address;
    if (!address || (lwip_ntohl(address) >> 24) == 127) { r->result = 0; return; }
    r->result = -EADDRNOTAVAIL;
    for (struct netif *n = netif_list; n; n = n->next)
        if (netif_is_up(n) && netif_is_link_up(n) && ip4_addr_get_u32(netif_ip4_addr(n)) == address) r->result = 0;
}
static void route_address_core(void *arg)
{
    struct request *r = arg; struct socket_object *s = r->socket;
    ip4_addr_t remote = { r->has_address || r->operation == OP_CONNECT ? r->address.address : s->peer.address };
    struct netif *n = ip4_route(&remote);
    sys_prot_t f = sys_arch_protect();
    if (n) s->route_address = ip4_addr_get_u32(netif_ip4_addr(n));
    sys_arch_unprotect(f);
}
/* IPX keeps the existing request/fd ownership boundary. The protocol hub is
 * serialized with DOS ingress by its port gate, independently of lwIP PCBs. */
static void ipx_sync(struct socket_object *s)
{
    if(!s->ipx)return;
    struct ciuki_sockaddr_ipx local,peer; unsigned error=0;
    if(!net_ipx_names(s->ipx,&local,&peer)) { memcpy(&s->local,&local,16); memcpy(&s->peer,&peer,16); }
    int state=net_ipx_call(s->ipx,IPX_CALL_STATUS,NULL,0,0,NULL,&error);
    if(state<0)return;
    if(s->type!=SOCK_DGRAM) {
        s->connecting=state==SPX_CONNECTING; s->connected=state==SPX_ESTABLISHED || state==SPX_END;
        if(s->connected)s->connect_result=0;
        if(error) { s->error=(int)error; s->failed=true; s->connect_result=-(int)error; }
    }
}
static int attempt_ipx_once(struct request *r)
{
    struct socket_object *s=r->socket; struct ciuki_sockaddr_ipx a;
    memcpy(&a,&r->address,sizeof(a));
    int e=0;
    if(r->operation!=OP_RECV && r->operation!=OP_NAME && r->operation!=OP_OPTION) {
        e=commit(r); if(e)return e;
    }
    switch(r->operation) {
    case OP_NEW:
        e=net_ipx_open(s->type,s->protocol); if(e>0) { s->ipx=(unsigned)e; e=0; } break;
    case OP_BIND:e=net_ipx_call(s->ipx,IPX_CALL_BIND,NULL,0,0,&a,NULL);break;
    case OP_CONNECT:
        e=net_ipx_call(s->ipx,IPX_CALL_CONNECT,NULL,0,0,&a,NULL);
        if(e==-EINPROGRESS) { s->connecting=true;s->connected=false;s->connect_result=e; }
        else if(!e) { s->connecting=false;s->connected=a.family!=AF_UNSPEC;s->connect_result=0; }
        break;
    case OP_LISTEN:
        e=net_ipx_call(s->ipx,IPX_CALL_LISTEN,NULL,r->which,0,NULL,NULL);
        if(!e)s->listening=true;
        break;
    case OP_ACCEPT: {
        struct ciuki_sockaddr_ipx local,peer;
        e=net_ipx_call(s->ipx,IPX_CALL_ACCEPT,&local,sizeof(local),0,&peer,NULL);
        if(e>0) {
            struct socket_object *c=r->created; c->family=AF_IPX; c->protocol=NSPROTO_SPX;
            c->ipx=(unsigned)e; c->connected=true;
            memcpy(&c->local,&local,16);memcpy(&c->peer,&peer,16);r->address=c->peer;e=0;
        }
        break;
    }
    case OP_SEND:e=net_ipx_call(s->ipx,IPX_CALL_SEND,r->buffer,r->bytes,r->flags,r->has_address?&a:NULL,NULL);break;
    case OP_RECV:
        if((e=commit(r)))break;
        e=net_ipx_call(s->ipx,IPX_CALL_RECV,r->buffer,r->bytes,r->flags,&a,&r->output_flags);
        if(e>=0)memcpy(&r->address,&a,16);
        break;
    case OP_OPTION:
        if(r->level==SOL_SOCKET && (r->option==SO_RCVTIMEO || r->option==SO_SNDTIMEO))e=option(r);
        else e=net_ipx_option(s->ipx,r->which,(int)r->level,r->option,r->buffer,&r->length);
        break;
    case OP_NAME: {
        struct ciuki_sockaddr_ipx local,peer;unsigned error=0;
        int state=net_ipx_call(s->ipx,IPX_CALL_STATUS,NULL,0,0,NULL,&error);
        if(state<0) { e=state;break; }
        e=net_ipx_names(s->ipx,&local,&peer);if(e)break;
        if(r->which && (s->type==SOCK_DGRAM ? !s->connected : state!=SPX_ESTABLISHED && state!=SPX_END)) { e=-ENOTCONN;break; }
        memcpy(&r->address,r->which?&peer:&local,16);break;
    }
    }
    ipx_sync(s); return e;
}
static int attempt_ipx(struct request *r)
{
    for(;;) {
        int e=attempt_ipx_once(r);if(e!=IPX_PORT_BUSY)return e;
        /* Contention precedes every protocol side effect. Re-open cancellation
         * before yielding; data/window EAGAIN retains its ordinary meaning. */
        sys_prot_t f=sys_arch_protect();r->committed=false;
        e=cancellation(r);sys_arch_unprotect(f);if(e)return e;
        cu_yield();
    }
}
static int attempt(struct request *r)
{
    struct socket_object *s = r->socket; struct netconn *c = s->conn;
    ip_addr_t ip; ip_addr_set_ip4_u32(&ip, r->address.address);
    u16_t port = lwip_ntohs(r->address.port); err_t e; int cancelled;
    if (s->control || s->closing) return -EBADF;
    if (r->process && !still_open(r->process, r->fd, r->fd_generation, s)) return -EBADF;
    if (s->network_down && (r->operation == OP_SEND || r->operation == OP_RECV ||
                            r->operation == OP_CONNECT || r->operation == OP_ACCEPT)) return -ENETDOWN;
    if(s->family==AF_IPX)return attempt_ipx(r);
    switch (r->operation) {
    case OP_NEW:
        if ((cancelled = commit(r))) return cancelled;
        s->conn = netconn_new_with_proto_and_callback(s->type == SOCK_STREAM ? NETCONN_TCP :
            s->type == SOCK_RAW ? NETCONN_RAW : NETCONN_UDP, s->type == SOCK_RAW ? IPPROTO_ICMP : 0, notify);
        if (!s->conn) return -ENOBUFS;
        netconn_set_nonblocking(s->conn, 1); s->send_events = s->type != SOCK_STREAM;
        return 0;
    case OP_BIND:
        r->result = 0;
        e = tcpip_callback_wait(local_address_core, r);
        if (e) return error_number(e);
        if (r->result) return r->result;
        if ((cancelled = commit(r))) return cancelled;
        return error_number(netconn_bind(c, &ip, port));
    case OP_CONNECT:
        if (s->type == SOCK_DGRAM && r->address.family == AF_UNSPEC) {
            if ((cancelled = commit(r))) return cancelled;
            s->connected = false; return error_number(netconn_disconnect(c));
        }
        if (s->connecting) return -EALREADY;
        if (s->connected && s->type == SOCK_STREAM) return -EISCONN;
        e = tcpip_callback_wait(route_address_core, r); if (e) return error_number(e);
        if ((cancelled = commit(r))) return cancelled;
        s->peer = r->address;
        tcp_record(s,0,0,0);
        s->connecting = s->type == SOCK_STREAM; s->failed = false; s->connect_result = -EINPROGRESS;
        e = netconn_connect(c, &ip, port);
        if (e == ERR_OK) { s->connected = true; s->connecting = false; s->connect_result = 0; tcp_record(s,0,0,1); }
        else if (e != ERR_INPROGRESS) { s->connecting = false; s->connect_result = error_number(e); tcp_record(s,0,-s->connect_result,2); }
        return error_number(e);
    case OP_LISTEN:
        if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
        if (!s->listening && listeners == 8) return -ENOBUFS;
        /* SO_REUSEADDR must never permit duplicate live listeners. The
         * normal worker owns all sequential queries in this comparison. */
        if (!s->listening) {
            ip_addr_t local; u16_t local_port;
            if (netconn_getaddr(c, &local, &local_port, 1) == ERR_OK && local_port) {
                for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++) {
                    struct socket_object *other = &sockets[i];
                    if (!other->used || !other->listening || other->closing) continue;
                    uint32_t other_ip = other->local.address;
                    if (local_port == lwip_ntohs(other->local.port) && (ip_addr_isany(&local) ||
                        !other_ip || ip4_addr_get_u32(ip_2_ip4(&local)) == other_ip)) return -EADDRINUSE;
                }
            }
        }
        if ((cancelled = commit(r))) return cancelled;
        e = netconn_listen_with_backlog(c, (u8_t)r->which);
        if (!e && !s->listening) {
            s->listening = true; listeners++;
            if (netconn_getaddr(c, &ip, &port, 1) == ERR_OK) address_out(&s->local, &ip, port);
        }
        return error_number(e);
    case OP_ACCEPT:
        if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
        if (!s->listening) return -EINVAL;
        if ((cancelled = commit(r))) return cancelled;
        e = netconn_accept(c, &r->accepted);
        if (!e) {
            struct socket_object *child = r->created;
            sys_prot_t f = sys_arch_protect(); child->conn = r->accepted;
            child->connected = true; child->send_events = 1; sys_arch_unprotect(f);
            netconn_set_nonblocking(child->conn, 1);
            ip_addr_t peer; u16_t peer_port;
            if (netconn_getaddr(child->conn, &peer, &peer_port, 0) == ERR_OK)
                address_out(&r->address, &peer, peer_port);
            child->peer = r->address;
            if (netconn_getaddr(child->conn, &peer, &peer_port, 1) == ERR_OK)
                address_out(&child->local, &peer, peer_port);
            child->route_address = child->local.address;
            /* ERROR/SEND notifications can precede installation of conn. */
            f = sys_arch_protect();
            if (child->conn->pending_err) {
                child->error = -error_number(child->conn->pending_err); child->failed = true;
            }
            sys_arch_unprotect(f);
        }
        return error_number(e);
    case OP_SEND:
        if (s->shut & 2) return -EPIPE;
        e = tcpip_callback_wait(route_address_core, r); if (e) return error_number(e);
        if ((cancelled = commit(r))) return cancelled;
        if (s->type == SOCK_STREAM) {
            if (!s->connected) return -ENOTCONN;
            if (r->has_address) return -EISCONN;
            if (!r->bytes) return 0;
            size_t done = 0;
            e = netconn_write_partly(c, r->buffer, r->bytes, NETCONN_COPY | NETCONN_DONTBLOCK, &done);
            return done ? (int)done : error_number(e);
        } else {
            if (!r->has_address && !s->connected) return -EDESTADDRREQ;
            struct netbuf *b = netbuf_new(); if (!b) return -ENOBUFS;
            void *data = netbuf_alloc(b, (u16_t)r->bytes);
            if (!data && r->bytes) { netbuf_delete(b); return -ENOBUFS; }
            if (r->bytes) memcpy(data, r->buffer, r->bytes);
            e = r->has_address ? netconn_sendto(c, b, &ip, port) : netconn_send(c, b);
            /* Upstream UDP uses ERR_VAL for the SO_BROADCAST permission
             * check; records and addresses have already been validated. */
            netbuf_delete(b); return e == ERR_VAL ? -EACCES : e ? error_number(e) : (int)r->bytes;
        }
    case OP_RECV:
        if (s->type == SOCK_STREAM) {
            if (!s->connected) return -ENOTCONN;
            if (!r->bytes || s->shut & 1) return 0;
            ip_addr_t peer; u16_t peer_port;
            if (netconn_getaddr(c, &peer, &peer_port, 0) == ERR_OK)
                address_out(&r->address, &peer, peer_port);
            TEST_HOOK(5);
            if ((cancelled = commit(r))) return cancelled;
            if (!s->tcp_rx && !s->eof) {
                e = netconn_recv_tcp_pbuf_flags(c, &s->tcp_rx, NETCONN_DONTBLOCK | NETCONN_NOAUTORCVD | NETCONN_NOFIN);
                if (e == ERR_WOULDBLOCK && netconn_is_flag_set(c, NETCONN_FIN_RX_PENDING)) {
                    s->eof = true; s->shut |= 1; cu_changed();
                } else if (e == ERR_CLSD) s->eof = true;
                else if (e) return error_number(e);
                if (!s->tcp_rx) s->eof = true;
                s->rx_offset = 0;
            }
            if (s->eof && !s->tcp_rx) return 0;
            unsigned left = s->tcp_rx->tot_len - s->rx_offset;
            unsigned n = r->bytes < left ? r->bytes : left;
            pbuf_copy_partial(s->tcp_rx, r->buffer, (u16_t)n, (u16_t)s->rx_offset);
            if (!(r->flags & MSG_PEEK)) {
                s->rx_offset += n; (void)netconn_tcp_recvd(c, n);
                if (s->rx_offset == s->tcp_rx->tot_len) { pbuf_free(s->tcp_rx); s->tcp_rx = NULL; s->rx_offset = 0; }
            }
            return (int)n;
        } else {
            if ((cancelled = commit(r))) return cancelled;
            if (!s->udp_rx) {
                e = netconn_recv_udp_raw_netbuf_flags(c, &s->udp_rx, NETCONN_DONTBLOCK);
                if (e) return error_number(e);
            }
            r->output_flags=s->udp_rx->p->tot_len>r->bytes?MSG_TRUNC:0;
            unsigned n = s->udp_rx->p->tot_len < r->bytes ? s->udp_rx->p->tot_len : r->bytes;
            pbuf_copy_partial(s->udp_rx->p, r->buffer, (u16_t)n, 0);
            address_out(&r->address, netbuf_fromaddr(s->udp_rx), netbuf_fromport(s->udp_rx));
            if (!(r->flags & MSG_PEEK)) { netbuf_delete(s->udp_rx); s->udp_rx = NULL; }
            return (int)n;
        }
    case OP_NAME:
        if ((cancelled = commit(r))) return cancelled;
        e = netconn_getaddr(c, &ip, &port, r->which ? 0 : 1);
        if (!e) address_out(&r->address, &ip, port);
        return error_number(e);
    case OP_OPTION:
        if ((cancelled = commit(r))) return cancelled;
        return option(r);
    }
    return -EINVAL;
}
static void socket_worker(void *unused)
{
    (void)unused; unsigned batch = 0;
    for (;;) {
        struct request *r; sys_arch_mbox_fetch(&commands, (void **)&r, 0);
        TEST_HOOK(1); /* deterministic pre-commit queue race in the host fixture */
        sys_prot_t f = sys_arch_protect();
        int cancelled = r->cancelled ? r->cancelled : cancellation(r);
        sys_arch_unprotect(f);
        r->result = cancelled ? cancelled : attempt(r);
        if (!r->abandoned && r->socket && r->socket->conn) {
            struct socket_object *s = r->socket; ip_addr_t ip; u16_t port;
            struct ciuki_sockaddr_in local = s->local, peer = s->peer;
            if (netconn_getaddr(s->conn, &ip, &port, 1) == ERR_OK) address_out(&local, &ip, port);
            if (netconn_getaddr(s->conn, &ip, &port, 0) == ERR_OK) address_out(&peer, &ip, port);
            sys_prot_t f = sys_arch_protect(); s->local = local; s->peer = peer; sys_arch_unprotect(f);
        }
        f = sys_arch_protect();
        if (r->socket) r->socket->active--;
        r->done = true;
        if (r->abandoned) reclaim_request(r);
        sys_arch_unprotect(f); cu_changed();
        if (++batch == 16) { batch = 0; cu_yield(); }
    }
}
static void control_worker(void *unused)
{
    (void)unused;
    for (;;) {
        bool worked = false;
        for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++) {
            struct socket_object *s = &sockets[i]; sys_prot_t f = sys_arch_protect();
            bool close = s->used && s->closing && !s->active && !s->holds && !s->watchers;
            bool half = s->used && !s->closing && !s->active && s->shut != s->control_shut;
            if ((!close && !half) || s->control) { sys_arch_unprotect(f); continue; }
            s->control = true; unsigned directions = s->shut;
            sys_arch_unprotect(f);
            TEST_HOOK(4); /* host fixture pauses after the exclusive handoff */
            if (close) {
                free_rx(s);
                err_t e = ERR_OK;
                int ipx_error=s->ipx ? net_ipx_close(s->ipx) : 0;
                if(ipx_error)e=ERR_WOULDBLOCK;
                else if(s->conn)e=netconn_delete(s->conn);
                f = sys_arch_protect();
                if(ipx_error==IPX_PORT_BUSY) { s->control=false;sys_arch_unprotect(f);cu_yield();continue; }
                if (!e) {
                    if (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET) tcp_count--; else udp_count--;
                    if (s->listening && s->family!=AF_IPX) listeners--;
                    s->used = false; s->conn = NULL;
                } else { s->failed = true; s->error = -error_number(e); }
            } else {
                int ipx_error=s->ipx ? net_ipx_call(s->ipx,IPX_CALL_SHUTDOWN,NULL,
                    directions==3?SHUT_RDWR:directions&2?SHUT_WR:SHUT_RD,0,NULL,NULL) : 0;
                err_t e=s->ipx ? ipx_error ? ERR_WOULDBLOCK : ERR_OK :
                    netconn_shutdown(s->conn,(directions&1)!=0,(directions&2)!=0);
                f = sys_arch_protect();
                if(s->ipx && ipx_error==IPX_PORT_BUSY) { s->control=false;sys_arch_unprotect(f);cu_yield();continue; }
                s->control_shut = directions;
                if (e) { s->failed = true; s->error = -error_number(e); }
            }
            s->control = false; sys_arch_unprotect(f); cu_changed();
            worked = true; cu_yield();
        }
        unsigned sequence = cu_change_snapshot();
        if (!worked) cu_wait_change(sequence, cu_now() + 10);
    }
}
int cu_sockets_init(void)
{
    if (workers_ready) return 0;
    if (sys_mbox_new(&commands, REQUESTS) != ERR_OK) return -ENOMEM;
    if (!sys_thread_new("net-sockets", socket_worker, NULL, 32768, 0) ||
        !sys_thread_new("net-control", control_worker, NULL, 32768, 0)) return -ENOMEM;
    workers_ready = true; return 0;
}
static int submit(struct request *r)
{
    r->cancelled = 0; r->committed = false;
    for (;;) {
        unsigned seq = cu_change_snapshot(); sys_prot_t f = sys_arch_protect();
        int e = cancellation(r);
        if (e) { sys_arch_unprotect(f); return e; }
        if (!r->socket || !r->socket->control) {
            if (r->socket) r->socket->active++;
            r->done = false; r->submitted = true;
            sys_arch_unprotect(f); break;
        }
        sys_arch_unprotect(f); TEST_HOOK(2); cu_wait_change(seq, cu_now() + 1);
    }
    if (sys_mbox_trypost(&commands, r) != ERR_OK) {
        sys_prot_t f = sys_arch_protect(); r->submitted = false;
        if (r->socket) r->socket->active--;
        sys_arch_unprotect(f); return -EAGAIN;
    }
    for (;;) {
        unsigned seq = cu_change_snapshot(); sys_prot_t f = sys_arch_protect(); bool done = r->done;
        if (!done && !r->committed && !r->cancelled) r->cancelled = cancellation(r);
        sys_arch_unprotect(f);
        if (done) return r->result;
        cu_wait_change(seq, cu_now() + 10);
    }
}
static unsigned process_sockets(struct process *p)
{
    uint64_t seen = 0; unsigned n = 0;
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
        struct proc_object *o = file_fd_object(p, (int)i); if (!net_socket_object(o)) continue;
        uint64_t bit = 1ULL << ((struct socket_object *)o - sockets);
        if (!(seen & bit)) { seen |= bit; n++; }
    }
    for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++)
        if (sockets[i].used && sockets[i].pending && sockets[i].origin == p && sockets[i].origin_pid == p->pid) n++;
    return n;
}
static struct socket_object *reserve_socket(struct process *p, unsigned type, int *error)
{
    sys_prot_t f = sys_arch_protect();
    const struct ciuki_credential *credential=security_credential(p);
    *error = security_process_live(p) && credential ? 0 : -EACCES;
    if (*error) { sys_arch_unprotect(f); return NULL; }
    if (process_sockets(p) >= CIUKI_SOCKET_PROCESS_MAX) *error = -EMFILE;
    else if (net_socket_count() + file_description_count() + desktop_objects.descriptions >= CIUKI_OPEN_DESCRIPTION_MAX) *error = -ENFILE;
    if (*error) { sys_arch_unprotect(f); return NULL; }
    if (((type == SOCK_STREAM || type == SOCK_SEQPACKET) ? tcp_count >= 32 : udp_count >= 16)) *error = -ENOBUFS;
    struct socket_object *s = NULL;
    for (unsigned i = 0; !*error && i < CIUKI_SOCKET_MAX; i++) if (!sockets[i].used) {
        s = &sockets[i]; memset(s, 0, sizeof(*s)); s->used = true; s->references = 1;
        s->object = (struct proc_object){ retain, release, true }; s->type = type; s->flags = O_RDWR;
        s->local.family = AF_INET; s->peer.family = AF_INET;
        s->generation = ++object_generation; s->origin = p; s->origin_pid = p->pid; s->pending = true;
        s->security_uid=credential->uid; s->security_session=credential->session_id;
        if (type == SOCK_STREAM || type == SOCK_SEQPACKET) tcp_count++; else udp_count++;
        break;
    }
    if (!s && !*error) *error = -ENOBUFS;
    sys_arch_unprotect(f); return s;
}
int net_socket_create(struct process *p, int family, int type, int protocol)
{
    if (family != AF_INET && family != AF_IPX) return -EAFNOSUPPORT;
    if (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC | 0xf)) return -EINVAL;
    unsigned base = type & 0xf;
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_RAW && !(family==AF_IPX && base==SOCK_SEQPACKET)) return -ESOCKTNOSUPPORT;
    if (family==AF_INET && protocol && protocol != (base == SOCK_STREAM ? IPPROTO_TCP : base == SOCK_RAW ? IPPROTO_ICMP : IPPROTO_UDP)) return -EPROTONOSUPPORT;
    if (base == SOCK_RAW && (family==AF_IPX || protocol != IPPROTO_ICMP)) return -EPROTONOSUPPORT;
    if(family==AF_IPX && protocol && (base==SOCK_DGRAM ? protocol<NSPROTO_IPX || protocol>NSPROTO_IPX+255 : protocol!=NSPROTO_SPX))return -EPROTONOSUPPORT;
    if (!workers_ready) return -ENOBUFS;
    int fd = file_fd_reserve(p); if (fd < 0) return fd;
    int e; struct socket_object *s = reserve_socket(p, base, &e); if (!s) { file_fd_cancel(p, fd); return e; }
    s->family=(unsigned)family; s->protocol=(unsigned)protocol;
    s->flags |= type & SOCK_NONBLOCK ? O_NONBLOCK : 0;
    struct request *r = request_new(s);
    if (!r) { s->pending = false; release(&s->object); file_fd_cancel(p, fd); return -ENOBUFS; }
    r->created = s; r->reserved_process = p; r->reserved_fd = fd;
    r->operation = OP_NEW; e = submit(r);
    if (!e) {
        e = file_fd_publish(p, fd, &s->object, type & SOCK_CLOEXEC ? FD_CLOEXEC : 0);
        if (e >= 0) { s->pending = false; r->created = NULL; r->reserved_process = NULL; e = 0; }
    }
    request_free(r);
    if (e) return e;
    return fd;
}
static struct request *for_fd(struct process *p, int fd, int *error, const struct net_socket_ref *ref)
{
    sys_prot_t f = sys_arch_protect();
    struct socket_object *s = lookup_captured(p, fd, error, ref);
    struct request *r = s ? request_new(s) : NULL;
    if (r) { r->process = p; r->fd = fd; r->fd_generation = p->fd_generation[fd]; }
    else if (s) *error = -ENOBUFS;
    sys_arch_unprotect(f);
    return r;
}
int net_socket_bind(struct process *p, int fd, const struct ciuki_sockaddr_in *a, const struct net_socket_ref *ref)
{
    int e; struct request *r = for_fd(p, fd, &e, ref); if (!r) return e;
    e = r->socket->family==AF_IPX ? 0 : address_check(a, false);
    if (!e) { r->operation = OP_BIND; r->address = *a; e = submit(r); }
    request_free(r); return e;
}
int net_socket_connect(struct process *p, int fd, const struct ciuki_sockaddr_in *a, const struct net_socket_ref *ref)
{
    int e; struct request *r = for_fd(p, fd, &e, ref); if (!r) return e;
    struct socket_object *s = r->socket; unsigned generation = r->fd_generation;
    e = s->family==AF_IPX ? 0 : address_check(a, s->type == SOCK_DGRAM);
    if (!e) {
        r->operation = OP_CONNECT; r->address = *a;
        uint64_t end = s->send_ms ? cu_now() + s->send_ms : UINT64_MAX;
        r->deadline = end; e = submit(r);
        while (e == -EINPROGRESS && !(s->flags & O_NONBLOCK)) {
            TEST_HOOK(3);
            unsigned seq = cu_change_snapshot();
            if (!still_open(p, fd, generation, s)) { e = -EBADF; break; }
            if (interrupted(p)) { e = -EINTR; break; }
            if(s->family==AF_IPX)ipx_sync(s);
            if (!s->connecting) { e = s->connect_result ? s->connect_result : s->connected ? 0 : -ENOTCONN; break; }
            if (cu_now() >= end) { e = -ETIMEDOUT; break; }
            cu_wait_change(seq, cu_now() + 1);
        }
    }
    request_free(r); return e;
}
int net_socket_listen(struct process *p, int fd, int backlog, const struct net_socket_ref *ref)
{
    if (backlog < 1) return -EINVAL;
    int e; struct request *r = for_fd(p, fd, &e, ref); if (!r) return e;
    r->operation = OP_LISTEN; r->which = backlog > 4 ? 4 : (unsigned)backlog;
    e = submit(r); request_free(r); return e;
}
static int retry(struct process *p, int fd, struct request *r, unsigned timeout)
{
    unsigned gen = r->fd_generation; uint64_t end = timeout ? cu_now() + timeout : UINT64_MAX;
    r->deadline = end;
    for (;;) {
        unsigned seq = cu_change_snapshot();
        if (!still_open(p, fd, gen, r->socket)) return -EBADF;
        if (interrupted(p)) return -EINTR;
        int e = submit(r);
        if (e != -EAGAIN || (r->socket->flags & O_NONBLOCK) || (r->flags & MSG_DONTWAIT)) return e;
        if (cu_now() >= end) return -EAGAIN;
        cu_wait_change(seq, cu_now() + 1);
    }
}
int net_socket_accept(struct process *p, int fd, struct ciuki_sockaddr_in *a, const struct net_socket_ref *ref)
{
    int e; struct request *r = for_fd(p, fd, &e, ref); if (!r) return e;
    if (r->socket->type != SOCK_STREAM && r->socket->type != SOCK_SEQPACKET) { request_free(r); return -EOPNOTSUPP; }
    int slot = file_fd_reserve(p); if (slot < 0) { request_free(r); return slot; }
    struct socket_object *child = reserve_socket(p, r->socket->type, &e);
    if (!child) { file_fd_cancel(p, slot); request_free(r); return e; }
    child->holds++;
    r->created = child; r->reserved_process = p; r->reserved_fd = slot;
    r->operation = OP_ACCEPT;
    e = retry(p, fd, r, r->socket->receive_ms);
    if (!e) {
        e = file_fd_publish(p, slot, &child->object, 0);
        if (e >= 0) {
            if (a) *a = r->address;
            child->pending = false; child->holds--; r->created = NULL; r->reserved_process = NULL; e = 0;
        }
    }
    request_free(r);
    if (e) return e;
    return slot;
}
static int socket_io(struct process *p, int fd, void *buffer, unsigned bytes, unsigned flags,
                  struct ciuki_sockaddr_in *a, bool send, const struct net_socket_ref *ref, unsigned *output_flags)
{
    if (bytes > CIUKI_SOCKET_IO_MAX || flags & ~(MSG_DONTWAIT | (send ? MSG_NOSIGNAL : MSG_PEEK))) return -EINVAL;
    int e; struct request *r = for_fd(p, fd, &e, ref); if (!r) return e;
    struct socket_object *s = r->socket;
    if (send && s->type != SOCK_STREAM && bytes > CIUKI_UDP_MAX) { request_free(r); return -EMSGSIZE; }
    if (send && s->type == SOCK_RAW && a && a->port) { request_free(r); return -EINVAL; }
    r->operation = send ? OP_SEND : OP_RECV; r->bytes = bytes; r->flags = flags;
    r->has_address = send && a;
    if (send && a) { e = s->family==AF_IPX ? 0 : address_check(a, false); if (e) { request_free(r); return e; } r->address = *a; }
    if (send && bytes) memcpy(r->buffer, buffer, bytes);
    e = retry(p, fd, r, send ? s->send_ms : s->receive_ms);
    if (!send && e >= 0) {
        if (e) memcpy(buffer, r->buffer, (unsigned)e);
        if (a) *a = r->address;
        if (output_flags) *output_flags=r->output_flags;
    }
    if (e == -EPIPE && send && !(flags & MSG_NOSIGNAL)) {
        struct proc_thread *t = proc_thread_for(g_current); if (t) proc_signal_pipe(t);
    }
    request_free(r); return e;
}
int net_socket_io(struct process *p,int fd,void *buffer,unsigned bytes,unsigned flags,
                  struct ciuki_sockaddr_in *a,bool send,const struct net_socket_ref *ref)
{ return socket_io(p,fd,buffer,bytes,flags,a,send,ref,NULL); }
int net_socket_receive(struct process *p,int fd,void *buffer,unsigned bytes,unsigned flags,
                       struct ciuki_sockaddr_in *a,const struct net_socket_ref *ref,unsigned *out)
{ *out=0;return socket_io(p,fd,buffer,bytes,flags,a,false,ref,out); }
int net_socket_shutdown(struct process *p, int fd, unsigned how, const struct net_socket_ref *ref)
{
    if (how > SHUT_RDWR) return -EINVAL;
    sys_prot_t f = sys_arch_protect(); int e;
    struct socket_object *s = lookup_captured(p, fd, &e, ref);
    if (!s) { sys_arch_unprotect(f); return e; }
    if (s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET) e = -EOPNOTSUPP;
    else if (!s->connected) e = -ENOTCONN;
    else s->shut |= how == SHUT_RD ? 1 : how == SHUT_WR ? 2 : 3;
    sys_arch_unprotect(f); if (e) return e;
    cu_changed(); return 0;
}
int net_socket_name(struct process *p, int fd, unsigned which, struct ciuki_sockaddr_in *out, const struct net_socket_ref *ref)
{
    if (which > 1) return -EINVAL;
    sys_prot_t f = sys_arch_protect(); int e;
    struct socket_object *s = lookup_captured(p, fd, &e, ref);
    if(s && s->family==AF_IPX) {
        sys_arch_unprotect(f);
        struct request *r=for_fd(p,fd,&e,ref);if(!r)return e;
        r->operation=OP_NAME;r->which=which;e=submit(r);
        if(!e)*out=r->address;request_free(r);return e;
    }
    if (s && which && !s->connected) e = -ENOTCONN;
    if (s && !e) *out = which ? s->peer : s->local;
    sys_arch_unprotect(f); return e;
}
int net_socket_option(struct process *p, int fd, unsigned set, int level, int name, void *value, unsigned *length, const struct net_socket_ref *ref)
{
    if (set > 1 || *length > 65535) return -EINVAL;
    int e; struct request *r = for_fd(p, fd, &e, ref); if (!r) return e == -ENOBUFS ? -ENOMEM : e;
    r->operation = OP_OPTION; r->which = set; r->level = (unsigned)level; r->option = (unsigned)name; r->length = *length;
    unsigned input=set?*length:0;
    if(!set && r->socket->family==AF_IPX) {
        if(name==IPX_ADDRESS)input=sizeof(struct ciuki_ipx_address_data);
        if(name==IPX_GETNETINFO || name==IPX_GETNETINFO_NORIP)input=sizeof(struct ciuki_ipx_netnum_data);
    }
    if(input>32 || input>*length) { request_free(r);return -EINVAL; }
    if(input)memcpy(r->buffer,value,input);
    e = submit(r);
    if (!e && !set) { unsigned n = *length < r->length ? *length : r->length; memcpy(value, r->buffer, n); *length = r->length; }
    request_free(r); return e;
}

extern int channel_poll_events(struct desktop_description *) __attribute__((weak));
/* fx-26: state-change readiness of /dev/mixer (audio_dev.c) and /dev/media
 * (media_dev.c): POLLIN while the description has not read the current state. */
extern int audio_mixer_poll_events(struct file_description *) __attribute__((weak));
extern int media_poll_events(struct file_description *) __attribute__((weak));
static unsigned mailbox_count(sys_mbox_t box)
{
    extern unsigned cu_mbox_count(sys_mbox_t); return box ? cu_mbox_count(box) : 0;
}
static int readiness(struct process *p, int fd, struct proc_object *o)
{
    if (net_socket_object(o)) {
        struct socket_object *s = (void *)o;
        if (!socket_subject(p,s)) return POLLNVAL;
        if(s->family==AF_IPX) { ipx_sync(s); return net_ipx_events(s->ipx); }
        int events = 0;
        if (s->tcp_rx || s->udp_rx || s->eof || s->receive_events > 0 || (s->shut & 1) ||
            (!s->control && s->conn && netconn_is_flag_set(s->conn, NETCONN_FIN_RX_PENDING)) ||
            (!s->control && s->conn && (mailbox_count(s->listening ? s->conn->acceptmbox : s->conn->recvmbox)))) events |= POLLIN;
        if (!s->connecting && !s->listening && (s->send_events || s->failed || (s->shut & 2))) events |= POLLOUT;
        if (s->error) events |= POLLERR;
        if (s->closing || s->failed || ((s->shut & 3) == 3)) events |= POLLHUP;
        return events;
    }
    struct file_description *file = file_fd(p, fd);
    if (file) {
        if (file->node->kind == PX_MIXER) return audio_mixer_poll_events ? audio_mixer_poll_events(file) : POLLNVAL;
        if (file->node->kind == PX_MEDIA) return media_poll_events ? media_poll_events(file) : POLLNVAL;
        if (file->node->kind != PX_FILE && file->node->kind != PX_DIRECTORY &&
            file->node->kind != PX_MNT && file->node->kind != PX_DEV) return POLLNVAL;
        unsigned mode = file->flags & O_ACCMODE;
        return (mode != O_WRONLY ? POLLIN : 0) | (mode != O_RDONLY ? POLLOUT : 0);
    }
    struct desktop_description *d = desktop_fd(p, fd);
    if (d && d->kind == DESKTOP_CHANNEL) return channel_poll_events ? channel_poll_events(d) : POLLNVAL;
    if (d && d->kind == DESKTOP_INPUT) {
        struct input_stats state; input_snapshot(&state); return state.pending ? POLLIN : 0;
    }
    return POLLNVAL;
}
struct poll_operation {
    struct proc_object *objects[CIUKI_OPEN_MAX];
    unsigned generations[CIUKI_OPEN_MAX], count;
    bool used;
    struct proc_thread *thread;
    void (*outer_cleanup)(void *);
    void *outer_operation;
};
static struct poll_operation polls[16];
static void poll_free(void *arg)
{
    struct poll_operation *op = arg;
    for (unsigned i = 0; i < op->count; i++) if (op->objects[i]) {
        if (net_socket_object(op->objects[i])) {
            sys_prot_t f = sys_arch_protect(); ((struct socket_object *)op->objects[i])->watchers--; sys_arch_unprotect(f);
        } else op->objects[i]->release(op->objects[i]);
    }
    op->used = false; cu_changed();
}
void net_poll_drop(void *arg) { poll_free(arg); }
static void poll_abandon(void *arg)
{
    struct poll_operation *op = arg; void (*outer)(void *) = op->outer_cleanup; void *value = op->outer_operation;
    poll_free(op); if (outer) outer(value);
}
int net_poll_capture(struct process *p, const struct ciuki_pollfd *fds, unsigned count, void **capture)
{
    if (count > CIUKI_OPEN_MAX) return -EINVAL;
    for (unsigned i = 0; i < count; i++)
        if ((unsigned)(uint16_t)fds[i].events & ~(POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)) return -EINVAL;
    struct poll_operation *op = NULL; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < 16; i++) if (!polls[i].used) { op = &polls[i]; memset(op, 0, sizeof(*op)); op->used = true; break; }
    sys_arch_unprotect(f); if (!op) return -ENOMEM;
    op->count = count;
    f = sys_arch_protect();
    for (unsigned i = 0; i < count; i++) {
        struct proc_object *o = file_fd_object(p, fds[i].fd); op->objects[i] = o;
        if (o) {
            op->generations[i] = p->fd_generation[fds[i].fd];
            if (net_socket_object(o)) ((struct socket_object *)o)->watchers++;
            else o->retain(o);
        }
    }
    sys_arch_unprotect(f);
    *capture = op; return 0;
}
int net_poll(struct process *p, struct ciuki_pollfd *fds, unsigned count, int timeout, void *capture)
{
    if (count > CIUKI_OPEN_MAX || timeout < -1) return -EINVAL;
    bool owned = !capture;
    if (owned) { int e = net_poll_capture(p, fds, count, &capture); if (e) return e; }
    struct poll_operation *op = capture;
    if (owned) {
        op->thread = proc_thread_for(g_current);
        if (op->thread) {
            op->outer_cleanup = op->thread->cleanup; op->outer_operation = op->thread->operation;
            op->thread->cleanup = poll_abandon; op->thread->operation = op;
        }
    }
    sys_prot_t f;
    uint64_t deadline = timeout >= 0 ? cu_now() + (unsigned)timeout : UINT64_MAX;
    int result;
    for (;;) {
        unsigned seq = cu_change_snapshot(); result = 0;
        for (unsigned i = 0; i < count; i++) {
            fds[i].revents = 0; if (fds[i].fd < 0) continue;
            f = sys_arch_protect();
            struct proc_object *o = op->objects[i];
            if (!o || file_fd_object(p, fds[i].fd) != o || p->fd_generation[fds[i].fd] != op->generations[i]) fds[i].revents = POLLNVAL;
            else fds[i].revents = (int16_t)(readiness(p, fds[i].fd, o) & (fds[i].events | POLLERR | POLLHUP | POLLNVAL));
            sys_arch_unprotect(f);
            result += fds[i].revents != 0;
        }
        if (result || !timeout) break;
        if (interrupted(p)) { result = -EINTR; break; }
        if (cu_now() >= deadline) break;
        cu_wait_change(seq, deadline);
    }
    if (owned) {
        if (op->thread) { op->thread->cleanup = op->outer_cleanup; op->thread->operation = op->outer_operation; }
        poll_free(op);
    }
    return result;
}
struct dns_request {
    char name[254];
    uint32_t address;
    int status;
    unsigned generation;
    uint32_t owner, token;
    bool used, done, abandoned;
    uint64_t started;
};
static struct dns_request lookups[CIUKI_DNS_MAX];
static unsigned resolver_generation = 1;
/* f6-22: the first answer and the first failure after each DNS server change
 * are protected records; the totals go into the `[net] stack` line. The
 * looked-up name is not recorded. Core thread only. */
static unsigned resolver_recorded_generation, resolver_answers, resolver_failures;
static bool resolver_answer_recorded, resolver_failure_recorded;
void ciuki_resolver_counts(unsigned *answers, unsigned *failures)
{ *answers = resolver_answers; *failures = resolver_failures; }
static void dotted_text(uint32_t address, char out[16])
{
    const uint8_t *b = (const uint8_t *)&address; char *p = out;
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = b[i];
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
        *p++ = (char)('0' + v % 10);
        *p++ = i < 3 ? '.' : 0;
    }
}
static void resolver_record(const struct dns_request *r, bool answered, uint32_t address)
{
    ciuki_lwip_assert_core();
    if (resolver_recorded_generation != r->generation) {
        resolver_recorded_generation = r->generation;
        resolver_answer_recorded = resolver_failure_recorded = false;
    }
    if (answered) resolver_answers++; else resolver_failures++;
    if (answered ? resolver_answer_recorded : resolver_failure_recorded) return;
    if (answered) resolver_answer_recorded = true; else resolver_failure_recorded = true;
    char ip[16], first[16], second[16];
    dotted_text(address, ip);
    const ip_addr_t *a = dns_getserver(0), *b = dns_getserver(1);
    dotted_text(a ? ip4_addr_get_u32(ip_2_ip4(a)) : 0, first);
    dotted_text(b ? ip4_addr_get_u32(ip_2_ip4(b)) : 0, second);
    klog("[net] resolver %s ip=%s servers=%s,%s elapsed_ms=%llu generation=%u",
         answered ? "first_answer" : "first_failure", ip, first, second,
         (unsigned long long)(cu_now() - r->started), r->generation);
}
/* The netif status callback runs AFTER upstream may abort/free TCP PCBs.
 * Retained endpoint/route state also identifies those sockets and listeners. */
void ciuki_sockets_address_lost(uint32_t address)
{
    ciuki_lwip_assert_core();
    sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++) {
        struct socket_object *s = &sockets[i];
        if (!s->used || s->closing || !s->conn) continue;
        uint32_t local = s->local.address;
        if (!local && s->conn->pcb.ip) local = ip4_addr_get_u32(ip_2_ip4(&s->conn->pcb.ip->local_ip));
        if (local == address || s->route_address == address) {
            s->network_down = true; s->error = ENETDOWN; s->failed = true;
            if (s->connecting || s->connect_result == -EINPROGRESS) s->connect_result = -ENETDOWN;
            s->connecting = false;
        }
    }
    sys_arch_unprotect(f); cu_changed();
}
void ciuki_dns_servers_changed(void)
{
    ciuki_lwip_assert_core();
    sys_prot_t f = sys_arch_protect(); resolver_generation++;
    sys_arch_unprotect(f); cu_changed();
}
static void dns_done(const char *name, const ip_addr_t *address, void *arg)
{
    (void)name; struct dns_request *r = arg; sys_prot_t f = sys_arch_protect();
    /* Upstream's callback cannot distinguish NXDOMAIN from retry exhaustion. */
    r->status = address && r->generation == resolver_generation ? 0 : 3;
    r->address = address ? ip4_addr_get_u32(ip_2_ip4(address)) : 0;
    struct dns_request copy = *r;
    r->done = true; if (r->abandoned) r->used = false;
    sys_arch_unprotect(f);
    resolver_record(&copy, !copy.status, copy.address);
    cu_changed();
}
static void dns_start(void *arg)
{
    struct dns_request *r = arg; ip_addr_t address;
    err_t e = dns_gethostbyname(r->name, &address, dns_done, r);
    if (e == ERR_OK) dns_done(r->name, &address, r);
    else if (e != ERR_INPROGRESS) dns_done(r->name, NULL, r);
}
static bool dns_name(const char *name)
{
    unsigned label = 0, length = 0;
    for (const unsigned char *p = (const void *)name; *p; p++) {
        if (++length > 253) return false;
        if (*p == '.') { if (!label || p[-1] == '-') return false; label = 0; continue; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
        if ((!label && *p == '-') || ++label > 63) return false;
    }
    return length && name[length - 1] != '-' && (label || name[length - 1] == '.');
}
struct dns_waiter { struct dns_request *request; void (*outer)(void *); void *value; };
static void dns_abandon(void *arg)
{
    struct dns_waiter *w = arg; sys_prot_t f = sys_arch_protect();
    w->request->abandoned = true; if (w->request->done) w->request->used = false;
    sys_arch_unprotect(f); if (w->outer) w->outer(w->value);
}
static struct { char name[64]; uint32_t address; } resolve_overrides[NET_RESOLVE_OVERRIDES];
int net_resolve_override(const char *name, uint32_t address)
{
    if (!name || strlen(name) >= sizeof(resolve_overrides[0].name) || !dns_name(name)) return -EINVAL;
    sys_prot_t f = sys_arch_protect(); int slot = -1;
    for (unsigned i = 0; i < NET_RESOLVE_OVERRIDES; i++) {
        if (!strncmp(resolve_overrides[i].name, name, sizeof(resolve_overrides[i].name))) { slot = (int)i; break; }
        if (slot < 0 && !resolve_overrides[i].address) slot = (int)i;
    }
    if (slot < 0) { sys_arch_unprotect(f); return -ENOSPC; }
    if (address) { memcpy(resolve_overrides[slot].name, name, strlen(name) + 1); resolve_overrides[slot].address = address; }
    else memset(&resolve_overrides[slot], 0, sizeof(resolve_overrides[slot]));
    sys_arch_unprotect(f);
    return 0;
}
static bool resolve_overridden(const char *name, uint32_t *address)
{
    bool found = false; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < NET_RESOLVE_OVERRIDES && !found; i++)
        if (resolve_overrides[i].address && !strncmp(resolve_overrides[i].name, name, sizeof(resolve_overrides[i].name))) { *address = resolve_overrides[i].address; found = true; }
    sys_arch_unprotect(f);
    return found;
}
int net_resolve(struct process *p, const char *name, uint32_t *address, int *status)
{
    if (!dns_name(name)) return -EINVAL;
    char qualified[254], domain[128]; bool dot = false;
    for (const char *s = name; *s; s++) if (*s == '.') dot = true;
    if (!dot) {
        net_stack_search_domain(domain);
        if (domain[0]) {
            size_t n = strlen(name), d = strlen(domain);
            if (n+1+d >= sizeof(qualified)) return -EINVAL;
            memcpy(qualified, name, n); qualified[n++] = '.'; memcpy(qualified+n, domain, d+1); name = qualified;
        }
    }
    if (resolve_overridden(name, address)) { *status = 0; return 0; }
    struct dns_request *r = NULL; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < CIUKI_DNS_MAX; i++) if (!lookups[i].used) { r = &lookups[i]; memset(r, 0, sizeof(*r)); r->used = true; break; }
    sys_arch_unprotect(f);
    if (!r) { *status = 2; *address = 0; return 0; }
    memcpy(r->name, name, strlen(name) + 1); r->generation = resolver_generation; r->started = cu_now();
    struct proc_thread *t = proc_thread_for(g_current);
    struct dns_waiter w = { r, t ? t->cleanup : NULL, t ? t->operation : NULL };
    if (t) { t->cleanup = dns_abandon; t->operation = &w; }
    int result = 0;
    if (tcpip_try_callback(dns_start, r) != ERR_OK) { r->used = false; result = -ENOMEM; }
    else {
        uint64_t end = cu_now() + 10000;
        for (;;) {
            unsigned seq = cu_change_snapshot(); f = sys_arch_protect();
            if (r->done) {
                *status = r->generation == resolver_generation ? r->status : 3;
                *address = *status ? 0 : r->address;
                r->used = false; sys_arch_unprotect(f); break;
            }
            if (interrupted(p) || cu_now() >= end) {
                result = interrupted(p) ? -EINTR : 0; *status = 2; *address = 0;
                r->abandoned = true; sys_arch_unprotect(f); break;
            }
            sys_arch_unprotect(f);
            cu_wait_change(seq, cu_now() + 1);
        }
    }
    if (t) { t->cleanup = w.outer; t->operation = w.value; }
    return result;
}
/* Async v2 shares the four DNS callback slots with synchronous v1. A token
 * never aliases a late callback: abandoned storage stays live until dns_done. */
static uint32_t dns_token;
void net_resolve_process_exit(struct process *p)
{
    sys_prot_t f=sys_arch_protect();
    for(unsigned i=0;i<CIUKI_DNS_MAX;i++)if(lookups[i].used && lookups[i].owner==p->pid) {
        lookups[i].abandoned=true;lookups[i].token=0;
        if(lookups[i].done)lookups[i].used=false;
    }
    sys_arch_unprotect(f);
}
int net_resolve_query(struct process *p,struct ciuki_resolve_query *q,const char *name)
{
    if(q->action<CIUKI_RESOLVE_START || q->action>CIUKI_RESOLVE_CANCEL)return -EINVAL;
    if(q->action==CIUKI_RESOLVE_START) {
        if(!workers_ready)return -ENETDOWN;
        if(!name || !dns_name(name))return -EINVAL;
        char qualified[254],domain[128];bool dot=false;
        for(const char *s=name;*s;s++)if(*s=='.')dot=true;
        if(!dot) {
            net_stack_search_domain(domain);
            if(domain[0]) {
                size_t n=strlen(name),d=strlen(domain);
                if(n+1+d>=sizeof(qualified))return -EINVAL;
                memcpy(qualified,name,n);qualified[n++]='.';memcpy(qualified+n,domain,d+1);name=qualified;
            }
        }
        struct dns_request *r=NULL;sys_prot_t f=sys_arch_protect();
        if(dns_token!=UINT32_MAX)for(unsigned i=0;i<CIUKI_DNS_MAX;i++)if(!lookups[i].used) {
            r=&lookups[i];memset(r,0,sizeof(*r));r->used=true;
            r->owner=p->pid;r->token=++dns_token;r->generation=resolver_generation;r->started=cu_now();
            memcpy(r->name,name,strlen(name)+1);break;
        }
        sys_arch_unprotect(f);if(!r)return -EAGAIN;
        q->token=r->token;q->address=0;q->status=4;
        if(resolve_overridden(name,&r->address)) { r->status=0;r->done=true; }
        else if(tcpip_try_callback(dns_start,r)!=ERR_OK) { r->used=false;return -ENOMEM; }
        return 0;
    }
    sys_prot_t f=sys_arch_protect();struct dns_request *r=NULL;
    for(unsigned i=0;i<CIUKI_DNS_MAX;i++)if(lookups[i].used && q->token &&
        lookups[i].token==q->token && lookups[i].owner==p->pid) { r=&lookups[i];break; }
    if(!r) { sys_arch_unprotect(f);return -EINVAL; }
    q->address=0;q->status=4;
    if(q->action==CIUKI_RESOLVE_CANCEL || (!r->done && cu_now()-r->started>=10000)) {
        q->status=2;r->token=0;r->abandoned=true;if(r->done)r->used=false;
    } else if(r->done) {
        q->status=r->generation==resolver_generation?r->status:3;
        q->address=q->status?0:r->address;r->used=false;
    }
    sys_arch_unprotect(f);return 0;
}
unsigned cu_socket_bytes(void) { return sizeof(sockets) + sizeof(polls) + sizeof(lookups); }
unsigned cu_socket_object_bytes(void) { return sizeof(struct socket_object); }
unsigned cu_request_bytes(void) { return sizeof(struct request); }
#ifdef LWIP_HOST
#include <stdio.h>
struct netconn *cu_socket_test_conn(struct process *p, int fd)
{
    int e; struct socket_object *s = lookup(p, fd, &e); return s ? s->conn : NULL;
}
bool cu_socket_test_control_pending(struct process *p, int fd)
{
    sys_prot_t f = sys_arch_protect(); int e; struct socket_object *s = lookup(p, fd, &e);
    bool pending = s && (s->control || s->shut != s->control_shut);
    sys_arch_unprotect(f); return pending;
}
void cu_socket_dump(void)
{
    sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < CIUKI_SOCKET_MAX; i++) if (sockets[i].used) {
        struct socket_object *s = &sockets[i];
        fprintf(stderr, "socket %u refs=%u holds=%u active=%u watchers=%u close=%u control=%u error=%d conn=%p\n",
            i, s->references, s->holds, s->active, s->watchers, s->closing, s->control, s->error, (void *)s->conn);
    }
    sys_arch_unprotect(f);
}
#endif
