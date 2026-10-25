/* Eight native socket connections, bounded DNS and nonblocking HTTP/TLS.
 * SPDX-License-Identifier: MIT */
#include "net.h"
#include "dns.h"
#include "proxy.h"
#include <limits.h>
#include "logline.h"
#include <ciuki/net.h>
#ifdef CIUKI_HOST
int ciuki_devctl(int,uint32_t,void *,uint32_t) __attribute__((weak));
#endif
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
/* fx-nsperf: WAITING holds an admitted job whose socket the system refused
 * for lack of resources; it reconnects to the saved address later. */
enum { EMPTY,RESOLVING,CONNECTING,TRANSFER,IDLE,TUNNEL,WAITING };
struct ciuki_connection {
    int fd;unsigned state,port;char host[254];bool secure;
    uint64_t idle_ms;
    uint32_t address;uint64_t retry_ms;unsigned refusals;
    struct ciuki_job *job;
    char proxy_host[254],proxy_auth[264];
    char tunnel[4096];size_t tunnel_bytes,tunnel_sent,tunnel_request;
    struct ciuki_tls *tls;
};
/* fx-nsperf (T23 trial 2ca3b0b6): images of one HTTPS page failed at
 * connect with errno 105 (ENOBUFS) about 5 s after NetSurf set them up,
 * five in the same second: each refusal freed its slot, the next queued
 * image took it and was refused too. The CiukiOS socket layer answers
 * ENOBUFS when its TCP objects (32 for the system, closing ones included),
 * its request slots (8) or lwIP's PCBs, netconns, segments or heap run out
 * (src/kernel/net/lwip/sockets.c, lwipopts.h); the browser alone keeps at
 * most CIUKI_NET_SLOTS sockets. A refusal is now a temporary shortage: the
 * job keeps its slot without a socket, the number of sockets the browser
 * opens at once drops to what the system just accepted (at least one), and
 * the connect is retried after 250 ms, doubling to 4 s, within the job's
 * 30 s limit. Each established connection raises the limit by one again. */
#define CIUKI_NET_SLOTS 8
#define CIUKI_NET_IDLE 4          /* keep-alive connections kept, at most 2 per origin */
#define CIUKI_NET_RETRY_MS 250
#define CIUKI_NET_RETRY_MAX_MS 4000
struct ciuki_net { struct ciuki_dns *dns;struct ciuki_connection connections[CIUKI_NET_SLOTS];const br_x509_trust_anchor *anchors;size_t anchor_count;bool polling;unsigned limit;unsigned char input[CIUKI_HTTP_SLICE]; };
static unsigned connect_retries;
unsigned ciuki_net_connect_retries(void) { return connect_retries; }
static uint64_t now_ms(void)
{ struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000; }
static unsigned record_id;
static const char *const stages[]={"dns","connect","tls","request","response","redirect","done","error"};
/* fx-11: each failed fetch's error record also goes to C:\SYSTEM\LOGS\
 * NETSURF.LOG (chrome.c, bounded per session), and the exit line carries a
 * fetch summary: a trial's serial capture is not always available, and
 * 423b803a's NETSURF.LOG could not name the failing stage. */
void ciuki_netsurf_log_record(const char *line) __attribute__((weak));
static unsigned fetches_done,fetches_ok,fetch_errors;
static char last_error[48]="none";
size_t ciuki_net_fetch_summary(char *out,size_t bytes)
{
    int n=bytes?snprintf(out,bytes,"fetches=%u fetch_ok=%u fetch_errors=%u last_error=%s",fetches_done,fetches_ok,fetch_errors,last_error):-1;
    if(n<0){if(bytes)out[0]=0;return 0;}
    return (size_t)n<bytes?(size_t)n:bytes-1;
}
__attribute__((format(printf,3,4)))
static int record_line(char *line,size_t size,const char *format,...)
{ va_list ap;va_start(ap,format);int n=ciuki_line_vformat(line,size,format,ap);va_end(ap);return n; }
void ciuki_net_record(struct ciuki_job *j,enum ciuki_fetch_stage stage)
{
    int saved=errno;
    if(stage<CIUKI_FETCH_DONE)j->stage=stage;
    if(j->record_mask&(1u<<stage))return;
    j->record_mask|=1u<<stage;
    uint64_t now=now_ms();struct timespec utc={0};uint32_t days,seconds;
    bool rtc=ciuki_tls_clock(&days,&seconds);
    clock_gettime(CLOCK_REALTIME,&utc);
    char line[CIUKI_LOG_LINE_MAX];
    int n=record_line(line,sizeof(line),"[netsurf] fetch url=%s://%s%s stage=%s code=%u errno=%d tls_err=%d bytes=%zu ms=%llu id=%u at=%s tls_code=%u rtc=%u utc=%lld",
        j->secure?"https":"http",j->host,j->path_prefix,stages[stage],j->http?j->http->status:0,
        j->io_errno,j->tls_engine_error,j->http?j->http->body_bytes:0,
        (unsigned long long)(now>=j->record_ms?now-j->record_ms:0),j->record_id,stages[j->stage],
        (unsigned)j->tls_error,rtc,(long long)utc.tv_sec);
    if(n>0){fflush(stderr);ciuki_write_all(2,line,(size_t)n);}
    if(stage==CIUKI_FETCH_ERROR){
        if(fetch_errors!=UINT_MAX)fetch_errors++;
        snprintf(last_error,sizeof(last_error),"%s:%d:%u",stages[j->stage],j->io_errno,(unsigned)j->tls_error);
        if(n>0 && ciuki_netsurf_log_record)ciuki_netsurf_log_record(line);
    }
    if(stage==CIUKI_FETCH_DONE){
        if(fetches_done!=UINT_MAX)fetches_done++;
        if(!(j->record_mask&(1u<<CIUKI_FETCH_ERROR)) && j->http && j->http->status>=200 && j->http->status<400 && fetches_ok!=UINT_MAX)fetches_ok++;
    }
    errno=saved;
}
void ciuki_net_failure(struct ciuki_job *j,const char *message)
{
    if(j->record_mask&(1u<<CIUKI_FETCH_ERROR))return;
    /* message can already refer to error_text (network state); copy first. */
    char reason[sizeof(j->error_text)];snprintf(reason,sizeof(reason),"%s",message);
    snprintf(j->error_text,sizeof(j->error_text),"Fetch failed during %s: %.*s",
        stages[j->stage],(int)sizeof(j->error_text)-48,reason);
    j->error=j->error_text;ciuki_net_record(j,CIUKI_FETCH_ERROR);
}
static bool job_event(void *context,enum ciuki_http_event event,const void *data,size_t bytes)
{
    struct ciuki_job *j=context;
    if(event==CIUKI_HTTP_HEAD || event==CIUKI_HTTP_HEADER)ciuki_net_record(j,CIUKI_FETCH_RESPONSE);
    return j->callback(j->context,event,data,bytes);
}
static bool text(const char *s,size_t limit,bool target)
{
    if(!s)return false;
    size_t n=strnlen(s,limit+1);if(n>limit)return false;
    for(size_t i=0;i<n;i++)if((unsigned char)s[i]<=32 || (unsigned char)s[i]==127 || (target && (s[i]=='#'||s[i]=='\\')))return false;
    return true;
}
static bool append(struct ciuki_job *j,const char *s,size_t n)
{
    if(n>CIUKI_HTTP_HEADERS+CIUKI_HTTP_POST-j->request_bytes)return false;
    if(j->request)memcpy(j->request+j->request_bytes,s,n);j->request_bytes+=n;return true;
}
static bool request(struct ciuki_job *j,const struct ciuki_request *r)
{
    if(!r->host || !*r->host || strlen(r->host)>253 || !r->port || r->port>65535 || !text(r->target,CIUKI_HTTP_URL,true) || r->target[0]!='/')return false;
    for(size_t i=0;r->host[i];i++){
        unsigned c=(unsigned char)r->host[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'))return false;
    }
    for(size_t i=0;r->host[i];i++){
        unsigned c=(unsigned char)r->host[i];
        j->host[i]=(char)(c>='A'&&c<='Z'?c+32:c);
    }
    j->proxy_port=0;
    if(!r->proxy_host || !*r->proxy_host)j->proxy_host[0]=0;
    if(r->proxy_host && *r->proxy_host) {
        size_t bytes=strlen(r->proxy_host);if(bytes>253 || !r->proxy_port || r->proxy_port>65535)return false;
        for(size_t i=0;i<bytes;i++) { unsigned c=(unsigned char)r->proxy_host[i];if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'))return false; }
        memmove(j->proxy_host,r->proxy_host,bytes+1);j->proxy_port=r->proxy_port;
    }
    if(!ciuki_proxy_auth_valid(r->proxy_auth))return false;
    if(r->proxy_auth)memmove(j->proxy_auth,r->proxy_auth,strlen(r->proxy_auth)+1);else j->proxy_auth[0]=0;
    j->port=r->port;j->secure=r->tls;j->post=r->post!=NULL;
    if(r->post && (!text(r->post,CIUKI_HTTP_POST,false)))return false;
    if(r->cookie && (strlen(r->cookie)>CIUKI_HTTP_LINE || strchr(r->cookie,'\r')||strchr(r->cookie,'\n')))return false;
    char prefix[128];const char *method=r->post?"POST ":r->head?"HEAD ":"GET ";
    if(!append(j,method,strlen(method)))return false;
    if(j->proxy_port && !r->tls) {
        char authority[280];int n=snprintf(authority,sizeof(authority),"http://%s:%u",j->host,j->port);
        if(n<0 || (size_t)n>=sizeof(authority) || !append(j,authority,(size_t)n))return false;
    }
    if(!append(j,r->target,strlen(r->target)) ||
        !append(j," HTTP/1.1\r\nHost: ",17) || !append(j,j->host,strlen(j->host)))return false;
    int n;
    if(r->port!=(r->tls?443u:80u)){
        n=snprintf(prefix,sizeof(prefix),":%u",r->port);if(n<0||!append(j,prefix,(size_t)n))return false;
    }
    const char *fixed="\r\nConnection: keep-alive\r\nAccept-Encoding: gzip, deflate\r\nUser-Agent: NetSurf/3.11 (CiukiOS)\r\n";
    if(!append(j,fixed,strlen(fixed)))return false;
    if(j->proxy_port && !r->tls && j->proxy_auth[0]) {
        if(!append(j,"Proxy-Authorization: ",21) || !append(j,j->proxy_auth,strlen(j->proxy_auth)) || !append(j,"\r\n",2))return false;
    }
    unsigned fields=4;
    if(r->cookie && *r->cookie){if(!append(j,"Cookie: ",8)||!append(j,r->cookie,strlen(r->cookie))||!append(j,"\r\n",2))return false;fields++;}
    if(r->headers)for(unsigned i=0;r->headers[i];i++){
        size_t len=strnlen(r->headers[i],CIUKI_HTTP_LINE+1);
        if(++fields>CIUKI_HTTP_FIELDS || len>CIUKI_HTTP_LINE || !strncasecmp(r->headers[i],"Proxy-Authorization:",20) || !ciuki_http_field(r->headers[i],len,true) ||
            !append(j,r->headers[i],len)||!append(j,"\r\n",2))return false;
    }
    if(r->post){
        n=snprintf(prefix,sizeof(prefix),"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n",strlen(r->post));
        if(n<0||!append(j,prefix,(size_t)n))return false;
    }
    if(!append(j,"\r\n",2)||j->request_bytes>CIUKI_HTTP_HEADERS)return false;
    return !r->post || append(j,r->post,strlen(r->post));
}
/* Pending accounting includes metadata and all caller-owned input copies.
 * NetSurf may fan out resources before any connection slot is available. */
static size_t pending_bytes;
size_t ciuki_net_pending_bytes(void) { return pending_bytes; }
static char *copy_text(char **out,const char *s)
{ if(!s)return NULL;char *p=*out;size_t n=strlen(s)+1;memcpy(p,s,n);*out+=n;return p; }
static void release_pending(struct ciuki_job *j)
{ pending_bytes-=j->pending_charge;j->pending_charge=0;free(j->snapshot);j->snapshot=NULL;memset(&j->pending,0,sizeof(j->pending)); }
struct ciuki_job *ciuki_net_job(const struct ciuki_request *r,ciuki_http_callback cb,void *ctx)
{
    if(sizeof(struct ciuki_job)>CIUKI_NET_PENDING_BUDGET-pending_bytes)return NULL;
    struct ciuki_job *j=calloc(1,sizeof(*j));if(!j)return NULL;
    j->pending_charge=sizeof(*j);pending_bytes+=j->pending_charge;
    j->callback=cb;j->context=ctx;
    j->record_id=++record_id;j->record_ms=now_ms();j->secure=r->tls;j->stage=CIUKI_FETCH_REQUEST;
    /* Invalid input must not inject console records either. */
    if(r->host)for(unsigned i=0;i<sizeof(j->host)-1 && r->host[i];i++){
        unsigned c=(unsigned char)r->host[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.')){
            strcpy(j->host,"invalid-host");break;
        }
        j->host[i]=(char)c;
    }
    const char *path=r->target?r->target:"/";
    for(unsigned i=0;i<sizeof(j->path_prefix)-1 && path[i] && path[i]!='?' && path[i]!='#';i++){
        unsigned c=(unsigned char)path[i];j->path_prefix[i]=(c>32 && c<127)?(char)c:'_';
    }
    if(!request(j,r)){j->error="Unsupported or oversized HTTP request (URL, headers, credentials, or form).";return j;}
    size_t fields=0,bytes=0;
    if(r->headers)while(r->headers[fields])bytes+=strlen(r->headers[fields++])+1;
    bytes+=(fields+1)*sizeof(char *)+strlen(r->target)+1;
    if(r->post)bytes+=strlen(r->post)+1;
    if(r->cookie)bytes+=strlen(r->cookie)+1;
    if(bytes>CIUKI_NET_PENDING_BUDGET-pending_bytes || !(j->snapshot=malloc(bytes))){ciuki_net_job_free(j);return NULL;}
    j->pending_charge+=bytes;pending_bytes+=bytes;j->pending=*r;j->pending.host=j->host;j->pending.proxy_host=j->proxy_host;j->pending.proxy_auth=j->proxy_auth;
    const char **headers=j->snapshot;char *out=(char *)(headers+fields+1);
    for(size_t i=0;i<fields;i++)headers[i]=copy_text(&out,r->headers[i]);headers[fields]=NULL;
    j->pending.headers=headers;j->pending.target=copy_text(&out,r->target);
    j->pending.post=copy_text(&out,r->post);j->pending.cookie=copy_text(&out,r->cookie);
    return j;
}
void ciuki_net_job_free(struct ciuki_job *j)
{ if(j){release_pending(j);if(j->http)ciuki_http_destroy(j->http);free(j->http);free(j->request);free(j);} }
static bool admit(struct ciuki_job *j)
{
    size_t bytes=j->request_bytes;
    j->http=malloc(sizeof(*j->http));j->request=malloc(bytes);
    if(!j->http || !j->request){free(j->http);j->http=NULL;free(j->request);j->request=NULL;j->error="Unable to allocate HTTP request buffers.";return false;}
    ciuki_http_init(j->http,j->pending.head,job_event,j);
    j->request_bytes=0;
    if(!request(j,&j->pending))j->error="Unable to prepare HTTP request.";
    release_pending(j);return !j->error;
}
struct ciuki_net *ciuki_net_create(const br_x509_trust_anchor *anchors,size_t count)
{
    struct ciuki_net *n=calloc(1,sizeof(*n));if(!n)return NULL;
    n->dns=ciuki_dns_create();if(!n->dns){free(n);return NULL;}
    n->anchors=anchors;n->anchor_count=count;
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++)n->connections[i].fd=-1;
    return n;
}
static void close_connection(struct ciuki_net *n,struct ciuki_connection *c)
{
    if(c->job){
        if(c->job->error)ciuki_net_failure(c->job,c->job->error);
        ciuki_net_record(c->job,CIUKI_FETCH_DONE);
        if(c->job->dns_token)ciuki_dns_cancel(n->dns,c->job->dns_token);
        c->job->dns_token=0;c->job->connection=NULL;c->job->finished=true;
    }
    if(c->fd>=0)close(c->fd);
    ciuki_tls_free(c->tls);memset(c,0,sizeof(*c));c->fd=-1;
}
void ciuki_net_destroy(struct ciuki_net *n)
{ if(n){for(unsigned i=0;i<CIUKI_NET_SLOTS;i++)close_connection(n,&n->connections[i]);ciuki_dns_destroy(n->dns);free(n);} }
void ciuki_net_cancel(struct ciuki_job *j)
{
    j->cancelled=true;
    if(j->connection && !j->owner->polling)close_connection(j->owner,j->connection);
    else if(!j->started)j->finished=true;
}
/* With no outstanding request, any queued bytes/EOF/error disqualify reuse.
 * Checking the TLS engine first also catches buffered/partial incoming records.
 * Rejecting pending ciphertext conservatively avoids interpreting alerts or
 * application records as a response to the next URL (RFC 9112 section 9.2). */
static bool clean_socket(struct ciuki_connection *c,bool request_queued)
{
    if(c->tls){
        if(ciuki_tls_pending_input(c->tls) || (ciuki_tls_state(c->tls)&BR_SSL_CLOSED))return false;
        /* Before the first transport send, SENDREC may be our queued request.
         * Idle engines must have no pending output either. */
        if(!request_queued && (ciuki_tls_state(c->tls)&BR_SSL_SENDREC))return false;
    }
    struct pollfd fd={c->fd,POLLIN,0};
    int ready=poll(&fd,1,0);
    if(ready<0 || fd.revents)return false;
    unsigned char byte;ssize_t count=recv(c->fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
    return count<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
}
void ciuki_net_service_idle(struct ciuki_net *n)
{
    uint64_t now=now_ms();
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];
        if(c->state==IDLE && (!now || now-c->idle_ms>=15000 || !clean_socket(c,false)))close_connection(n,c);
    }
}
static bool read_state(struct ciuki_net_state *state)
{
#ifdef CIUKI_HOST
    if(!ciuki_devctl)return false;
#endif
    int fd=open("/dev/net",O_RDONLY);if(fd<0)return false;
    *state=(struct ciuki_net_state){.version=CIUKI_NET_VERSION,.size=sizeof(*state)};
    int result=ciuki_devctl(fd,NET_GET_STATE,state,sizeof(*state));close(fd);
    return !result;
}
static bool offline(void)
{
    struct ciuki_net_state state;
    return read_state(&state) && !state.address_configured;
}
/* Outcome of this browser's last DNS lookup (numeric and loopback hosts
 * never ask the resolver). */
enum { DNS_UNTRIED,DNS_ANSWERED,DNS_FAILED };
static int dns_outcome;
static unsigned state_kind(const struct ciuki_net_state *s)
{
    if(!s->present)return 1;
    if(s->state!=CIUKI_NET_RUNNING)return 2;
    if(s->link==CIUKI_NET_LINK_DOWN)return 3;
    if(!s->address_configured)return 4;
    return dns_outcome==DNS_ANSWERED?6:dns_outcome==DNS_FAILED?7:5;
}
const char *ciuki_net_state_word(void)
{
    static const char *const words[]={"unknown","no-adapter","stopped","no-link","no-address","address","dns-ok","dns-failed"};
    struct ciuki_net_state s;
    return words[read_state(&s)?state_kind(&s):0];
}
size_t ciuki_net_state_text(char *out,size_t bytes)
{
    struct ciuki_net_state s;int n=0;
    if(!bytes)return 0;
    out[0]=0;
    if(!read_state(&s))return 0;
    char driver[sizeof(s.driver)+1],reason[sizeof(s.reason)+1];
    memcpy(driver,s.driver,sizeof(s.driver));driver[sizeof(s.driver)]=0;
    memcpy(reason,s.reason,sizeof(s.reason));reason[sizeof(s.reason)]=0;
    for(char *c=driver;*c;c++)if((unsigned char)*c<32 || (unsigned char)*c>126)*c='?';
    for(char *c=reason;*c;c++)if((unsigned char)*c<32 || (unsigned char)*c>126)*c='?';
    switch(state_kind(&s)){
    case 1:n=snprintf(out,bytes,"Network: no network adapter is active.");break;
    case 2:n=snprintf(out,bytes,"Network: the %s adapter is %s (%s).",driver,s.state==CIUKI_NET_QUARANTINED?"disabled after a fault":"stopped",reason);break;
    case 3:n=snprintf(out,bytes,"Network: the %s adapter has no link; check the cable.",driver);break;
    case 4:n=snprintf(out,bytes,"Network: no address yet (%s, link %s); waiting for a DHCP server.",driver,s.link==CIUKI_NET_LINK_UP?"up":"unknown");break;
    default:
        n=snprintf(out,bytes,"Network: address %u.%u.%u.%u (%s); %s.",s.ipv4[0],s.ipv4[1],s.ipv4[2],s.ipv4[3],driver,
            dns_outcome==DNS_ANSWERED?"DNS ok":dns_outcome==DNS_FAILED?"DNS lookups failing":"DNS not tried yet");
    }
    if(n<0){out[0]=0;return 0;}
    return (size_t)n<bytes?(size_t)n:bytes-1;
}
/* Network failures and the readiness page carry the network state. */
static void network_message(struct ciuki_job *j,const char *message)
{
    char state[224];
    if(!ciuki_net_state_text(state,sizeof(state))){j->error=message;return;}
    snprintf(j->error_text,sizeof(j->error_text),"%s %s",message,state);
    j->error=j->error_text;
}
static void tls_failure(struct ciuki_job *j)
{
    const char *message=ciuki_tls_describe(j->tls_error,j->tls_message,sizeof(j->tls_message));
    if(j->tls_error==CIUKI_TLS_NOT_READY)network_message(j,message);else j->error=message;
}
static bool loopback_name(const char *host)
{ return !strcasecmp(host,"localhost") || !strcasecmp(host,"localhost."); }
static bool origin(const struct ciuki_connection *c,const struct ciuki_job *j)
{ return c->state && c->port==(j->proxy_port?j->proxy_port:j->port) && c->secure==j->secure && !strcmp(c->host,j->host) && !strcmp(c->proxy_host,j->proxy_host) && !strcmp(c->proxy_auth,j->proxy_auth); }
static void io_error(struct ciuki_job *j,int error)
{
    j->io_errno=error;
    network_message(j,error==ENETDOWN||error==ENETUNREACH||error==EADDRNOTAVAIL?"Offline: no usable network address or route.":
        error==ECONNREFUSED?"The server refused the connection.":error==ETIMEDOUT?"The network request timed out (30 second limit).":"The network connection failed.");
}
static unsigned open_sockets(const struct ciuki_net *n)
{ unsigned count=0;for(unsigned i=0;i<CIUKI_NET_SLOTS;i++)if(n->connections[i].fd>=0)count++;return count; }
/* n->limit is 0 until the system refuses a socket, then the number of
 * sockets the browser may hold. */
static bool at_limit(const struct ciuki_net *n) { return n->limit && open_sockets(n)>=n->limit; }
static bool refused_for_resources(int error)
{ return error==ENOBUFS || error==ENOMEM || error==EAGAIN || error==EMFILE || error==ENFILE; }
/* Wait for a socket: the next attempt is due at retry_ms, and only below
 * the browser's current socket limit. */
static void wait_for_socket(struct ciuki_connection *c,uint64_t delay)
{ c->state=WAITING;c->retry_ms=now_ms()+delay; }
static bool open_connection(struct ciuki_net *n,struct ciuki_connection *c,uint32_t address)
{
    struct ciuki_job *j=c->job;
    ciuki_net_record(j,CIUKI_FETCH_CONNECT);
    c->address=address;
    if(at_limit(n)){wait_for_socket(c,0);return true;}
    c->fd=socket(AF_INET,SOCK_STREAM,0);
    int r=c->fd<0?-1:0;
    if(!r){
        int flags=fcntl(c->fd,F_GETFL,0);
        if(flags<0||fcntl(c->fd,F_SETFL,flags|O_NONBLOCK)<0){io_error(j,errno);return false;}
        struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons((uint16_t)c->port);a.sin_addr.s_addr=address;
        r=connect(c->fd,(struct sockaddr *)&a,sizeof(a));
        if(r<0 && (errno==EINPROGRESS || errno==EALREADY || errno==EINTR))r=0;
    }
    if(r<0){
        int error=errno;
        if(!refused_for_resources(error) || now_ms()-j->started_ms>=30000){io_error(j,error);return false;}
        /* The system is out of sockets or buffers: give this one back, open
         * no more than are open now and try again later; the retry first
         * closes a kept-alive connection no waiting job can use. */
        if(c->fd>=0)close(c->fd);
        c->fd=-1;
        unsigned open=open_sockets(n);
        n->limit=open?open:1;
        uint64_t delay=(uint64_t)CIUKI_NET_RETRY_MS<<(c->refusals<4?c->refusals:4);
        if(delay>CIUKI_NET_RETRY_MAX_MS)delay=CIUKI_NET_RETRY_MAX_MS;
        c->refusals++;j->io_errno=error;
        if(connect_retries!=UINT_MAX)connect_retries++;
        if(c->refusals==1){
            /* Not a fetch stage record: the fetch goes on. The session's
             * first wait also reaches NETSURF.LOG. */
            static bool logged;
            char line[CIUKI_LOG_LINE_MAX];
            int count=record_line(line,sizeof(line),"[netsurf] connect-wait url=%s://%s%s errno=%d id=%u open=%u limit=%u retry_ms=%llu",
                j->secure?"https":"http",j->host,j->path_prefix,error,j->record_id,open,n->limit,(unsigned long long)delay);
            if(count>0){
                fflush(stderr);ciuki_write_all(2,line,(size_t)count);
                if(!logged && ciuki_netsurf_log_record){logged=true;ciuki_netsurf_log_record(line);}
            }
        }
        wait_for_socket(c,delay);return true;
    }
    c->tunnel_bytes=c->tunnel_sent=c->tunnel_request=0;
    c->state=CONNECTING;return true;
}
static bool resolve(struct ciuki_net *n,struct ciuki_connection *c)
{
    struct in_addr address;
    const char *host=c->job->proxy_port?c->job->proxy_host:c->job->host;
    c->state=RESOLVING;
    ciuki_net_record(c->job,CIUKI_FETCH_DNS);
    if(inet_pton(AF_INET,host,&address)==1)return open_connection(n,c,address.s_addr);
    c->job->dns_token=ciuki_dns_submit(n->dns,host);
    return true;
}
bool ciuki_net_start(struct ciuki_net *n,struct ciuki_job *j)
{
    if(j->started)return true;
    ciuki_net_service_idle(n);
    if(j->cancelled){j->started=j->finished=true;return true;}
    if(j->secure){
        uint32_t days,seconds;
        if(!ciuki_tls_clock(&days,&seconds)){j->stage=CIUKI_FETCH_TLS;j->tls_error=CIUKI_TLS_CLOCK;j->error=ciuki_tls_message(j->tls_error);}
    }
    if(j->error){ciuki_net_record(j,j->stage);ciuki_net_failure(j,j->error);ciuki_net_record(j,CIUKI_FETCH_DONE);release_pending(j);j->started=j->finished=true;return true;}
    struct ciuki_connection *slot=NULL,*idle=NULL;unsigned active=0;
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];
        if(origin(c,j)){
            if(c->job)active++;
            else if(c->state==IDLE)idle=c;
        }
        if(!c->state)slot=c;
    }
    if(active>=2)return false;
    if(idle)slot=idle;
    if(!slot)for(unsigned i=0;i<CIUKI_NET_SLOTS;i++)if(n->connections[i].state==IDLE){slot=&n->connections[i];close_connection(n,slot);break;}
    if(!slot)return false;
    if(!admit(j)){ciuki_net_failure(j,j->error);ciuki_net_record(j,CIUKI_FETCH_DONE);release_pending(j);j->started=j->finished=true;return true;}
    j->started=true;j->started_ms=now_ms();j->owner=n;j->connection=slot;slot->job=j;
    if(!idle)slot->state=RESOLVING;
    if(!j->started_ms){j->error="Monotonic system clock is unavailable.";return true;}
    if(idle){j->pooled=true;slot->state=TRANSFER;return true;}
    slot->port=j->proxy_port?j->proxy_port:j->port;memcpy(slot->proxy_host,j->proxy_host,sizeof(slot->proxy_host));memcpy(slot->proxy_auth,j->proxy_auth,sizeof(slot->proxy_auth));slot->secure=j->secure;memcpy(slot->host,j->host,sizeof(slot->host));
    /* Refuse invalid clock/entropy before DNS or any TLS network traffic. */
    if(j->secure){
        slot->tls=ciuki_tls_create(j->host,n->anchors,n->anchor_count,&j->tls_error);
        if(!slot->tls){ciuki_net_record(j,CIUKI_FETCH_TLS);tls_failure(j);return true;}
    }
    resolve(n,slot);return true;
}
/* A job waiting for a socket takes an idle keep-alive connection of its
 * origin instead (HTTP/1.1 persistent connection reuse, RFC 9112 9.3). */
static bool reuse_idle(struct ciuki_net *n,struct ciuki_connection *waiting)
{
    struct ciuki_job *j=waiting->job;
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];
        if(c==waiting || c->state!=IDLE || !origin(c,j))continue;
        if(waiting->fd>=0)close(waiting->fd);
        ciuki_tls_free(waiting->tls);memset(waiting,0,sizeof(*waiting));waiting->fd=-1;
        j->pooled=true;j->connection=c;c->job=j;c->state=TRANSFER;
        return true;
    }
    return false;
}
/* A kept-alive connection that no waiting job can use, to close when the
 * browser is at its socket limit. */
static struct ciuki_connection *unwanted_idle(struct ciuki_net *n)
{
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];bool wanted=false;
        if(c->state!=IDLE)continue;
        for(unsigned k=0;k<CIUKI_NET_SLOTS;k++)
            if(n->connections[k].state==WAITING && origin(c,n->connections[k].job))wanted=true;
        if(!wanted)return c;
    }
    return NULL;
}
static void finish(struct ciuki_net *n,struct ciuki_connection *c)
{
    struct ciuki_job *j=c->job;
    if(j->error)ciuki_net_failure(j,j->error);
    ciuki_net_record(j,CIUKI_FETCH_DONE);
    bool keep=!j->cancelled && !j->error && j->sent==j->request_bytes && ciuki_http_reusable(j->http);
    unsigned idle=0,same=0;
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++)if(n->connections[i].state==IDLE){idle++;if(origin(&n->connections[i],j))same++;}
    if(keep && idle<CIUKI_NET_IDLE && same<2){
        j->finished=true;j->connection=NULL;c->job=NULL;c->state=IDLE;c->idle_ms=now_ms();
        for(unsigned i=0;i<CIUKI_NET_SLOTS;i++)
            if(n->connections[i].state==WAITING && origin(c,n->connections[i].job) && reuse_idle(n,&n->connections[i]))break;
    }
    else close_connection(n,c);
}
static void tunnel(struct ciuki_connection *c)
{
    struct ciuki_job *j=c->job;
    if(!c->tunnel_request) {
        int n=ciuki_proxy_connect_request(c->tunnel,sizeof(c->tunnel),j->host,j->port,j->proxy_auth);
        if(n<0) { j->error="Invalid proxy CONNECT request.";return; }c->tunnel_request=(size_t)n;
    }
    if(c->tunnel_sent<c->tunnel_request) {
        ssize_t n=send(c->fd,c->tunnel+c->tunnel_sent,c->tunnel_request-c->tunnel_sent,MSG_NOSIGNAL);
        if(n<0) { if(errno!=EAGAIN && errno!=EINTR)io_error(j,errno);return; }
        if(!n) { io_error(j,ECONNRESET);return; }c->tunnel_sent+=(size_t)n;return;
    }
    ssize_t n=recv(c->fd,c->tunnel+c->tunnel_bytes,sizeof(c->tunnel)-c->tunnel_bytes,MSG_DONTWAIT);
    if(n<0) { if(errno!=EAGAIN && errno!=EINTR)io_error(j,errno);return; }
    if(!n) { io_error(j,ECONNRESET);return; }c->tunnel_bytes+=(size_t)n;
    int result=ciuki_proxy_connect_response(c->tunnel,c->tunnel_bytes);
    if(result<0)j->error=result==-407?"Proxy authentication failed or credentials are required.":"The proxy refused or malformed the CONNECT tunnel.";
    else if(result>0) { memset(c->tunnel,0,sizeof(c->tunnel));c->state=TRANSFER;ciuki_net_record(j,CIUKI_FETCH_TLS); }
}
static void transfer(struct ciuki_connection *c,unsigned char *buffer)
{
    struct ciuki_job *j=c->job;size_t budget=CIUKI_HTTP_SLICE;
    if(j->pooled && !j->transmitted && !clean_socket(c,j->sent!=0)){io_error(j,ECONNRESET);return;}
    if(ciuki_http_pending(j->http)){
        ciuki_http_drain(j->http);
        if(j->http->error)j->error=j->http->error;
        if(j->http->cancelled)j->cancelled=true;
        return;
    }
    if(c->tls){
        j->tls_error=ciuki_tls_pump(c->tls,c->fd,&budget,&j->transmitted);
        j->tls_engine_error=ciuki_tls_engine_error(c->tls);
        if(j->tls_error==CIUKI_TLS_IO)j->io_errno=errno;
        if(j->tls_error==CIUKI_TLS_EOF){ciuki_http_eof(j->http);j->tls_error=CIUKI_TLS_OK;if(j->http->error)j->error=j->http->error;return;}
        if(j->tls_error){j->error=ciuki_tls_message(j->tls_error);return;}
        /* The pump may have decrypted application data since the first idle
         * check. Plaintext submission alone does not make it our response. */
        if(j->pooled && !j->transmitted && !clean_socket(c,j->sent!=0)){io_error(j,ECONNRESET);return;}
        size_t bytes;unsigned char *out=ciuki_tls_write(c->tls,&bytes);
        if(out && j->sent<j->request_bytes){
            ciuki_net_record(j,CIUKI_FETCH_REQUEST);
            size_t count=j->request_bytes-j->sent;if(count>bytes)count=bytes;if(count>CIUKI_HTTP_SLICE)count=CIUKI_HTTP_SLICE;
            memcpy(out,j->request+j->sent,count);j->sent+=count;ciuki_tls_write_ack(c->tls,count);ciuki_tls_flush(c->tls);
        }
        const unsigned char *in=ciuki_tls_read(c->tls,&bytes);
        if(in && bytes){
            if(!j->transmitted){io_error(j,ECONNRESET);return;}
            if(bytes>CIUKI_HTTP_SLICE)bytes=CIUKI_HTTP_SLICE;
            j->stage=CIUKI_FETCH_RESPONSE;
            ciuki_http_feed(j->http,in,bytes);ciuki_tls_read_ack(c->tls,bytes);
        }
    } else {
        if(j->sent<j->request_bytes){
            ciuki_net_record(j,CIUKI_FETCH_REQUEST);
            size_t count=j->request_bytes-j->sent;if(count>budget)count=budget;
            ssize_t r=send(c->fd,j->request+j->sent,count,MSG_NOSIGNAL);
            if(r>0){j->transmitted=true;j->sent+=(size_t)r;budget-=(size_t)r;}
            else if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){io_error(j,errno);return;}
        }
        if(budget){
            ssize_t r=recv(c->fd,buffer,budget,0);
            if(r>0){if(!j->transmitted){io_error(j,ECONNRESET);return;}j->stage=CIUKI_FETCH_RESPONSE;ciuki_http_feed(j->http,buffer,(size_t)r);}
            else if(!r)ciuki_http_eof(j->http);
            else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){io_error(j,errno);return;}
        }
    }
    if(j->http->error)j->error=j->http->error;
    if(j->http->cancelled)j->cancelled=true;
}
void ciuki_net_poll(struct ciuki_net *n)
{
    if(n->polling)return;n->polling=true;
    ciuki_net_service_idle(n);
    uint64_t now=now_ms();
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];struct ciuki_job *j=c->job;
        if(c->state==IDLE)continue;
        if(!j)continue;
        if(!j->error && now-j->started_ms>=30000)io_error(j,ETIMEDOUT);
        if(!j->error && !j->cancelled && c->state==RESOLVING){
            uint32_t address;int error;
            if(!j->dns_token)j->dns_token=ciuki_dns_submit(n->dns,j->proxy_port?j->proxy_host:j->host);
            if(j->dns_token && ciuki_dns_result(n->dns,j->dns_token,&address,&error)){
                j->dns_token=0;
                j->io_errno=error;
                if(!loopback_name(j->host))dns_outcome=error?DNS_FAILED:DNS_ANSWERED;
                if(error){
                    if(!loopback_name(j->host) && offline())io_error(j,ENETDOWN);
                    else if(error==EHOSTUNREACH)network_message(j,"DNS lookup failed: the server name could not be resolved.");
                    else io_error(j,error);
                }
                else open_connection(n,c,address);
            }
        }
        if(!j->error && !j->cancelled && c->state==CONNECTING){
            struct pollfd fd={c->fd,POLLOUT,0};int r=poll(&fd,1,0);
            if(r<0 && errno!=EINTR)io_error(j,errno);
            else if(r>0){int error=0;socklen_t bytes=sizeof(error);
                if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&error,&bytes))io_error(j,errno);
                else if(error)io_error(j,error);
                else {
                    j->io_errno=0;
                    c->state=j->secure && j->proxy_port?TUNNEL:TRANSFER;if(c->tls && c->state==TRANSFER)ciuki_net_record(j,CIUKI_FETCH_TLS);
                    if(n->limit && ++n->limit>=CIUKI_NET_SLOTS)n->limit=0;
                }
            }
        }
        if(!j->error && !j->cancelled && c->state==WAITING && !reuse_idle(n,c) && now>=c->retry_ms){
            struct ciuki_connection *idle=at_limit(n)?unwanted_idle(n):NULL;
            if(idle)close_connection(n,idle);
            if(!at_limit(n))open_connection(n,c,c->address);
        }
        if(c->state==EMPTY)continue; /* the job moved to a kept-alive connection */
        if(!j->error && !j->cancelled && c->state==TUNNEL)tunnel(c);
        if(!j->error && !j->cancelled && c->state==TRANSFER)transfer(c,n->input);
        /* A stale idle socket can be retried once, only for idempotent methods
         * before receiving any response byte. Never replay a POST. */
        if(j->error && j->pooled && !j->post && !j->retried && !j->http->header_bytes && !j->cancelled){
            j->retried=true;j->pooled=false;j->sent=0;j->transmitted=false;j->error=NULL;j->tls_error=CIUKI_TLS_OK;
            j->io_errno=0;j->tls_engine_error=0;
            bool head=j->http->head;
            ciuki_http_destroy(j->http);ciuki_http_init(j->http,head,job_event,j);
            if(c->fd>=0)close(c->fd);c->fd=-1;ciuki_tls_free(c->tls);c->tls=NULL;
            if(j->secure){c->tls=ciuki_tls_create(j->host,n->anchors,n->anchor_count,&j->tls_error);if(!c->tls)tls_failure(j);}
            if(!j->error)resolve(n,c);
        }
        if(j->error||j->cancelled||j->http->done)finish(n,c);
    }
    n->polling=false;
}
void ciuki_net_notify(struct ciuki_net *n,void (*notify)(void *),void *ctx)
{ if(n)ciuki_dns_notify(n->dns,notify,ctx); }
/* Real request/keepalive expiry, including DNS with no socket yet. */
int ciuki_net_timeout(struct ciuki_net *n,int timeout)
{
    if(!n)return timeout;
    uint64_t now=now_ms();
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];uint64_t end;
        if(c->state==IDLE)end=c->idle_ms+15000;
        else if(c->job)end=c->job->started_ms+30000;
        else continue;
        /* A wait that is due runs as soon as a socket is free; one blocked by
         * the limit wakes with the connection that frees it. */
        if(c->state==WAITING && c->retry_ms>now && c->retry_ms<end)end=c->retry_ms;
        if(c->state==WAITING && c->retry_ms<=now && (!at_limit(n) || unwanted_idle(n)))end=now;
        uint64_t left=end>now?end-now:0;
        if(c->job && c->tls && (ciuki_tls_state(c->tls)&BR_SSL_RECVAPP))left=0;
        if(timeout<0 || left<(unsigned)timeout)timeout=left>INT_MAX?INT_MAX:(int)left;
    }
    return timeout;
}
unsigned ciuki_net_fds(struct ciuki_net *n,struct pollfd fds[8])
{
    unsigned count=0;
    for(unsigned i=0;i<CIUKI_NET_SLOTS;i++){
        struct ciuki_connection *c=&n->connections[i];if(c->fd<0)continue;
        short events=POLLIN;
        if(c->state==CONNECTING)events=POLLOUT;
        else if(c->state==TUNNEL) { if(!c->tunnel_request || c->tunnel_sent<c->tunnel_request)events|=POLLOUT; }
        else if(c->tls){if(ciuki_tls_state(c->tls)&BR_SSL_SENDREC)events|=POLLOUT;}
        else if(c->job && c->job->sent<c->job->request_bytes)events|=POLLOUT;
        fds[count++]=(struct pollfd){c->fd,events,0};
    }
    return count;
}
struct ciuki_tls *ciuki_net_tls(struct ciuki_job *j) { return j->connection?j->connection->tls:NULL; }
