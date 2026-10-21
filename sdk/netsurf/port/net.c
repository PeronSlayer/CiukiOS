/* Eight native socket connections, bounded DNS and nonblocking HTTP/TLS.
 * SPDX-License-Identifier: MIT */
#include "net.h"
#include "dns.h"
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
enum { EMPTY,RESOLVING,CONNECTING,TRANSFER,IDLE };
struct ciuki_connection {
    int fd;unsigned state,port;char host[254];bool secure;
    uint64_t idle_ms;
    struct ciuki_job *job;
    struct ciuki_tls *tls;
};
struct ciuki_net { struct ciuki_dns *dns;struct ciuki_connection connections[8];const br_x509_trust_anchor *anchors;size_t anchor_count;bool polling;unsigned char input[CIUKI_HTTP_SLICE]; };
static uint64_t now_ms(void)
{ struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000; }
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
    memcpy(j->request+j->request_bytes,s,n);j->request_bytes+=n;return true;
}
static bool request(struct ciuki_job *j,const struct ciuki_request *r)
{
    if(!r->host || !*r->host || strlen(r->host)>253 || !r->port || r->port>65535 || !text(r->target,CIUKI_HTTP_URL,true) || r->target[0]!='/')return false;
    for(size_t i=0;r->host[i];i++){
        unsigned c=(unsigned char)r->host[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'))return false;
        j->host[i]=(char)(c>='A'&&c<='Z'?c+32:c);
    }
    j->port=r->port;j->secure=r->tls;j->post=r->post!=NULL;
    if(r->post && (!text(r->post,CIUKI_HTTP_POST,false)))return false;
    if(r->cookie && (strlen(r->cookie)>CIUKI_HTTP_LINE || strchr(r->cookie,'\r')||strchr(r->cookie,'\n')))return false;
    char prefix[128];const char *method=r->post?"POST ":r->head?"HEAD ":"GET ";
    if(!append(j,method,strlen(method)) || !append(j,r->target,strlen(r->target)) ||
        !append(j," HTTP/1.1\r\nHost: ",17) || !append(j,j->host,strlen(j->host)))return false;
    int n;
    if(r->port!=(r->tls?443u:80u)){
        n=snprintf(prefix,sizeof(prefix),":%u",r->port);if(n<0||!append(j,prefix,(size_t)n))return false;
    }
    const char *fixed="\r\nConnection: keep-alive\r\nAccept-Encoding: gzip, deflate\r\nUser-Agent: NetSurf/3.11 (CiukiOS)\r\n";
    if(!append(j,fixed,strlen(fixed)))return false;
    unsigned fields=4;
    if(r->cookie && *r->cookie){if(!append(j,"Cookie: ",8)||!append(j,r->cookie,strlen(r->cookie))||!append(j,"\r\n",2))return false;fields++;}
    if(r->headers)for(unsigned i=0;r->headers[i];i++){
        size_t len=strnlen(r->headers[i],CIUKI_HTTP_LINE+1);
        if(++fields>CIUKI_HTTP_FIELDS || len>CIUKI_HTTP_LINE || !ciuki_http_field(r->headers[i],len,true) ||
            !append(j,r->headers[i],len)||!append(j,"\r\n",2))return false;
    }
    if(r->post){
        n=snprintf(prefix,sizeof(prefix),"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n",strlen(r->post));
        if(n<0||!append(j,prefix,(size_t)n))return false;
    }
    if(!append(j,"\r\n",2)||j->request_bytes>CIUKI_HTTP_HEADERS)return false;
    return !r->post || append(j,r->post,strlen(r->post));
}
struct ciuki_job *ciuki_net_job(const struct ciuki_request *r,ciuki_http_callback cb,void *ctx)
{
    struct ciuki_job *j=calloc(1,sizeof(*j));if(!j)return NULL;
    ciuki_http_init(&j->http,r->head,cb,ctx);
    j->request=malloc(CIUKI_HTTP_HEADERS+CIUKI_HTTP_POST);
    if(!j->request){free(j);return NULL;}
    if(!request(j,r))j->error="Unsupported or oversized HTTP request (URL, headers, credentials, or form).";
    return j;
}
void ciuki_net_job_free(struct ciuki_job *j)
{ if(j){ciuki_http_destroy(&j->http);free(j->request);free(j);} }
struct ciuki_net *ciuki_net_create(const br_x509_trust_anchor *anchors,size_t count)
{
    struct ciuki_net *n=calloc(1,sizeof(*n));if(!n)return NULL;
    n->dns=ciuki_dns_create();if(!n->dns){free(n);return NULL;}
    n->anchors=anchors;n->anchor_count=count;
    for(unsigned i=0;i<8;i++)n->connections[i].fd=-1;
    return n;
}
static void close_connection(struct ciuki_net *n,struct ciuki_connection *c)
{
    if(c->job){
        if(c->job->dns_token)ciuki_dns_cancel(n->dns,c->job->dns_token);
        c->job->dns_token=0;c->job->connection=NULL;c->job->finished=true;
    }
    if(c->fd>=0)close(c->fd);
    ciuki_tls_free(c->tls);memset(c,0,sizeof(*c));c->fd=-1;
}
void ciuki_net_destroy(struct ciuki_net *n)
{ if(n){for(unsigned i=0;i<8;i++)close_connection(n,&n->connections[i]);ciuki_dns_destroy(n->dns);free(n);} }
void ciuki_net_cancel(struct ciuki_job *j)
{
    j->cancelled=true;
    if(j->connection && !j->owner->polling)close_connection(j->owner,j->connection);
    else if(!j->started)j->finished=true;
}
static bool origin(const struct ciuki_connection *c,const struct ciuki_job *j)
{ return c->state && c->port==j->port && c->secure==j->secure && !strcmp(c->host,j->host); }
static void io_error(struct ciuki_job *j,int error)
{
    j->error=error==ENETDOWN||error==ENETUNREACH||error==EADDRNOTAVAIL?"Offline: no usable network address or route.":
        error==ECONNREFUSED?"The server refused the connection.":error==ETIMEDOUT?"The network request timed out (30 second limit).":"The network connection failed.";
}
static bool open_connection(struct ciuki_connection *c,uint32_t address)
{
    struct ciuki_job *j=c->job;
    c->fd=socket(AF_INET,SOCK_STREAM,0);
    if(c->fd<0){io_error(j,errno);return false;}
    int flags=fcntl(c->fd,F_GETFL,0);
    if(flags<0||fcntl(c->fd,F_SETFL,flags|O_NONBLOCK)<0){io_error(j,errno);return false;}
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons((uint16_t)j->port);a.sin_addr.s_addr=address;
    int r=connect(c->fd,(struct sockaddr *)&a,sizeof(a));
    if(r<0 && errno!=EINPROGRESS && errno!=EALREADY && errno!=EINTR){io_error(j,errno);return false;}
    c->state=CONNECTING;return true;
}
static bool resolve(struct ciuki_net *n,struct ciuki_connection *c)
{
    struct in_addr address;
    c->state=RESOLVING;
    if(inet_pton(AF_INET,c->job->host,&address)==1)return open_connection(c,address.s_addr);
    c->job->dns_token=ciuki_dns_submit(n->dns,c->job->host);
    return true;
}
bool ciuki_net_start(struct ciuki_net *n,struct ciuki_job *j)
{
    if(j->started)return true;
    if(j->cancelled){j->started=j->finished=true;return true;}
    if(j->secure){
        uint32_t days,seconds;
        if(!ciuki_tls_clock(&days,&seconds)){j->tls_error=CIUKI_TLS_CLOCK;j->error=ciuki_tls_message(j->tls_error);}
    }
    if(j->error){j->started=j->finished=true;return true;}
    struct ciuki_connection *slot=NULL,*idle=NULL;unsigned active=0;
    for(unsigned i=0;i<8;i++){
        struct ciuki_connection *c=&n->connections[i];
        if(origin(c,j)){
            if(c->job)active++;
            else if(c->state==IDLE)idle=c;
        }
        if(!c->state)slot=c;
    }
    if(active>=2)return false;
    if(idle)slot=idle;
    if(!slot)for(unsigned i=0;i<8;i++)if(n->connections[i].state==IDLE){slot=&n->connections[i];close_connection(n,slot);break;}
    if(!slot)return false;
    j->started=true;j->started_ms=now_ms();j->owner=n;j->connection=slot;slot->job=j;
    if(!idle)slot->state=RESOLVING;
    if(!j->started_ms){j->error="Monotonic system clock is unavailable.";return true;}
    if(idle){j->pooled=true;slot->state=TRANSFER;return true;}
    slot->port=j->port;slot->secure=j->secure;memcpy(slot->host,j->host,sizeof(slot->host));
    /* Refuse invalid clock/entropy before DNS or any TLS network traffic. */
    if(j->secure){
        slot->tls=ciuki_tls_create(j->host,n->anchors,n->anchor_count,&j->tls_error);
        if(!slot->tls){j->error=ciuki_tls_message(j->tls_error);return true;}
    }
    resolve(n,slot);return true;
}
static void finish(struct ciuki_net *n,struct ciuki_connection *c)
{
    struct ciuki_job *j=c->job;
    bool keep=!j->cancelled && !j->error && j->sent==j->request_bytes && ciuki_http_reusable(&j->http);
    unsigned idle=0;for(unsigned i=0;i<8;i++)if(n->connections[i].state==IDLE)idle++;
    if(keep && idle<2){j->finished=true;j->connection=NULL;c->job=NULL;c->state=IDLE;c->idle_ms=now_ms();}
    else close_connection(n,c);
}
static void transfer(struct ciuki_connection *c,unsigned char *buffer)
{
    struct ciuki_job *j=c->job;size_t budget=CIUKI_HTTP_SLICE;
    if(ciuki_http_pending(&j->http)){
        ciuki_http_drain(&j->http);
        if(j->http.error)j->error=j->http.error;
        if(j->http.cancelled)j->cancelled=true;
        return;
    }
    if(c->tls){
        j->tls_error=ciuki_tls_pump(c->tls,c->fd,&budget);
        if(j->tls_error==CIUKI_TLS_EOF){ciuki_http_eof(&j->http);j->tls_error=CIUKI_TLS_OK;if(j->http.error)j->error=j->http.error;return;}
        if(j->tls_error){j->error=ciuki_tls_message(j->tls_error);return;}
        size_t bytes;unsigned char *out=ciuki_tls_write(c->tls,&bytes);
        if(out && j->sent<j->request_bytes){
            size_t count=j->request_bytes-j->sent;if(count>bytes)count=bytes;if(count>CIUKI_HTTP_SLICE)count=CIUKI_HTTP_SLICE;
            memcpy(out,j->request+j->sent,count);j->sent+=count;ciuki_tls_write_ack(c->tls,count);ciuki_tls_flush(c->tls);
        }
        const unsigned char *in=ciuki_tls_read(c->tls,&bytes);
        if(in && bytes){
            if(bytes>CIUKI_HTTP_SLICE)bytes=CIUKI_HTTP_SLICE;
            ciuki_http_feed(&j->http,in,bytes);ciuki_tls_read_ack(c->tls,bytes);
        }
    } else {
        if(j->sent<j->request_bytes){
            size_t count=j->request_bytes-j->sent;if(count>budget)count=budget;
            ssize_t r=send(c->fd,j->request+j->sent,count,MSG_NOSIGNAL);
            if(r>0){j->sent+=(size_t)r;budget-=(size_t)r;}
            else if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){io_error(j,errno);return;}
        }
        if(budget){
            ssize_t r=recv(c->fd,buffer,budget,0);
            if(r>0)ciuki_http_feed(&j->http,buffer,(size_t)r);
            else if(!r)ciuki_http_eof(&j->http);
            else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){io_error(j,errno);return;}
        }
    }
    if(j->http.error)j->error=j->http.error;
    if(j->http.cancelled)j->cancelled=true;
}
void ciuki_net_poll(struct ciuki_net *n)
{
    if(n->polling)return;n->polling=true;
    uint64_t now=now_ms();
    for(unsigned i=0;i<8;i++){
        struct ciuki_connection *c=&n->connections[i];struct ciuki_job *j=c->job;
        if(c->state==IDLE){if(now-c->idle_ms>=15000)close_connection(n,c);continue;}
        if(!j)continue;
        if(!j->error && now-j->started_ms>=30000)io_error(j,ETIMEDOUT);
        if(!j->error && !j->cancelled && c->state==RESOLVING){
            uint32_t address;int error;
            if(!j->dns_token)j->dns_token=ciuki_dns_submit(n->dns,j->host);
            if(j->dns_token && ciuki_dns_result(n->dns,j->dns_token,&address,&error)){
                j->dns_token=0;
                if(error){io_error(j,error);if(error==EHOSTUNREACH)j->error="DNS lookup failed: the server name could not be resolved.";}
                else open_connection(c,address);
            }
        }
        if(!j->error && !j->cancelled && c->state==CONNECTING){
            struct pollfd fd={c->fd,POLLOUT,0};int r=poll(&fd,1,0);
            if(r<0 && errno!=EINTR)io_error(j,errno);
            else if(r>0){int error=0;socklen_t bytes=sizeof(error);
                if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&error,&bytes))io_error(j,errno);
                else if(error)io_error(j,error);
                else c->state=TRANSFER;
            }
        }
        if(!j->error && !j->cancelled && c->state==TRANSFER)transfer(c,n->input);
        /* A stale idle socket can be retried once, only for idempotent methods
         * before receiving any response byte. Never replay a POST. */
        if(j->error && j->pooled && !j->post && !j->retried && !j->http.header_bytes && !j->cancelled){
            j->retried=true;j->pooled=false;j->sent=0;j->error=NULL;j->tls_error=CIUKI_TLS_OK;
            ciuki_http_callback callback=j->http.callback;void *context=j->http.context;bool head=j->http.head;
            ciuki_http_destroy(&j->http);ciuki_http_init(&j->http,head,callback,context);
            if(c->fd>=0)close(c->fd);c->fd=-1;ciuki_tls_free(c->tls);c->tls=NULL;
            if(j->secure){c->tls=ciuki_tls_create(j->host,n->anchors,n->anchor_count,&j->tls_error);if(!c->tls)j->error=ciuki_tls_message(j->tls_error);}
            if(!j->error)resolve(n,c);
        }
        if(j->error||j->cancelled||j->http.done)finish(n,c);
    }
    n->polling=false;
}
unsigned ciuki_net_fds(struct ciuki_net *n,struct pollfd fds[8])
{
    unsigned count=0;
    for(unsigned i=0;i<8;i++){
        struct ciuki_connection *c=&n->connections[i];if(!c->job || c->fd<0)continue;
        short events=POLLIN;
        if(c->state==CONNECTING)events=POLLOUT;
        else if(c->tls){if(ciuki_tls_state(c->tls)&BR_SSL_SENDREC)events|=POLLOUT;}
        else if(c->job->sent<c->job->request_bytes)events|=POLLOUT;
        fds[count++]=(struct pollfd){c->fd,events,0};
    }
    return count;
}
struct ciuki_tls *ciuki_net_tls(struct ciuki_job *j) { return j->connection?j->connection->tls:NULL; }
