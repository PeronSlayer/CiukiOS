/* NetSurf 3.11 fetcher binding. SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libwapcaplet/libwapcaplet.h>
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/inet.h"
#include "netsurf/ssl_certs.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"
#include "framebuffer/ciuki/net.h"
#include "framebuffer/ciuki/testmode.h"
struct ciuki_fetch {
    struct ciuki_fetch *next;
    struct fetch *parent;
    nsurl *url;
    struct ciuki_job *job;
    bool active,aborted,only_2xx;
    const char *error;
    fetch_msg_type terminal;
    size_t delivered; /* decoded body bytes handed to the core (f6-18 observer) */
};
/* Host harnesses link the fetcher without chrome.c: silent defaults. */
__attribute__((weak)) void ciuki_test_fetch_observe(unsigned status,size_t bytes,int tls_error,const char *error)
{ (void)status;(void)bytes;(void)tls_error;(void)error; }
__attribute__((weak)) const br_x509_trust_anchor *ciuki_test_anchors(size_t *count) { *count=0;return NULL; }
static void observe(struct ciuki_fetch *f)
{
    unsigned status=f->job && f->job->http ? f->job->http->status : 0;
    int tls=f->job && f->job->secure ? (int)f->job->tls_error : -1;
    ciuki_test_fetch_observe(status,f->delivered,tls,f->error?f->error:f->job?f->job->error:NULL);
}
static struct ciuki_net *network;
static struct ciuki_fetch *jobs;
static unsigned references;
static bool in_poll;
static unsigned cookie_count;
static const struct cookie_data *oldest_cookie;
static char cookie_keys[3][CIUKI_HTTP_LINE+1];
static bool count_cookie(const struct cookie_data *cookie)
{
    cookie_count++;
    if(!oldest_cookie || cookie->last_used<oldest_cookie->last_used)oldest_cookie=cookie;
    return cookie_count<=200;
}
static bool trim_cookies(void)
{
    /* Retention is enforced in NetSurf's database, including session cookies.
     * Delete outside the iterator and retain no cookie pointers afterwards. */
    for(;;){
        cookie_count=0;oldest_cookie=NULL;urldb_iterate_cookies(count_cookie);
        if(cookie_count<=200)break;
        const char *keys[]={oldest_cookie->domain,oldest_cookie->path,oldest_cookie->name};
        for(unsigned i=0;i<3;i++){
            size_t n=strnlen(keys[i],CIUKI_HTTP_LINE+1);if(n>CIUKI_HTTP_LINE){oldest_cookie=NULL;return false;}
            memcpy(cookie_keys[i],keys[i],n+1);
        }
        /* delete_cookie continues traversing other hosts after releasing the
         * cookie; its arguments must outlive that traversal. */
        urldb_delete_cookie(cookie_keys[0],cookie_keys[1],cookie_keys[2]);
    }
    oldest_cookie=NULL;
    return true;
}
static bool notify(struct ciuki_fetch *f,const fetch_msg *msg)
{ if(f->aborted)return false;fetch_send_callback(msg,f->parent);return !f->aborted; }
static bool http_event(void *context,enum ciuki_http_event e,const void *bytes,size_t count)
{
    struct ciuki_fetch *f=context;
    if(f->aborted)return false;
    struct ciuki_http *p=f->job->http;
    fetch_set_http_code(f->parent,p->status);
    if(e==CIUKI_HTTP_HEAD){
        struct ciuki_tls *tls=ciuki_net_tls(f->job);
        if(tls){
            const unsigned char *der;const struct ciuki_tls_cert *certs;
            struct cert_chain chain={0};chain.depth=ciuki_tls_certificates(tls,&der,&certs);
            for(size_t i=0;i<chain.depth;i++){
                chain.certs[i].der=(uint8_t *)der+certs[i].offset;
                chain.certs[i].der_length=certs[i].bytes;chain.certs[i].err=SSL_CERT_ERR_OK;
            }
            fetch_msg message={.type=FETCH_CERTS,.data.chain=&chain};
            if(!notify(f,&message))return false;
        }
        if(p->status==304 && !f->job->post){f->terminal=FETCH_NOTMODIFIED;return false;}
        if((p->status==301||p->status==302||p->status==303||p->status==307||p->status==308) && p->location[0]){
            nsurl *to=NULL;
            if(nsurl_join(f->url,p->location,&to)!=NSERROR_OK)f->error="Invalid HTTP redirect URL.";
            else{
                enum nsurl_scheme_type scheme=nsurl_get_scheme_type(to);
                if(scheme!=NSURL_SCHEME_HTTP && scheme!=NSURL_SCHEME_HTTPS)f->error="Unsupported HTTP redirect scheme.";
                else if(f->job->secure && scheme!=NSURL_SCHEME_HTTPS)f->error="HTTPS redirect to an insecure connection was refused.";
                else f->terminal=FETCH_REDIRECT;
                nsurl_unref(to);
            }
            return false;
        }
        if(p->status==401 || p->status==407){f->error="HTTP authentication and proxies are unsupported.";return false;}
        if(f->only_2xx && (p->status<200 || p->status>299)){f->error="The server returned an unsuccessful HTTP status.";return false;}
        return true;
    }
    if(e==CIUKI_HTTP_HEADER && count>11 && !strncasecmp(bytes,"Set-Cookie:",11)){
        /* NetSurf owns domain/path/Secure/HttpOnly/PSL/context policy. */
        const char *s=bytes;size_t n=count-13;char *cookie=malloc(n+1);
        if(!cookie){f->error="Unable to allocate HTTP cookie.";return false;}
        memcpy(cookie,s+11,n);cookie[n]=0;fetch_set_cookie(f->parent,cookie);free(cookie);
        if(!trim_cookies()){f->error="Stored cookie exceeds the browser limit.";return false;}
        if(f->aborted)return false;
    }
    fetch_msg msg={.type=e==CIUKI_HTTP_DATA?FETCH_DATA:FETCH_HEADER};
    msg.data.header_or_data.buf=bytes;msg.data.header_or_data.len=count;
    if(e==CIUKI_HTTP_DATA)f->delivered+=count;
    return notify(f,&msg);
}
static bool initialise(lwc_string *scheme)
{
    (void)scheme;
    if(!references){
        size_t count=0;const br_x509_trust_anchor *anchors=ciuki_test_anchors(&count);
        network=ciuki_net_create(anchors,count);if(!network)return false;
    }
    references++;return true;
}
static bool acceptable(const nsurl *url)
{
    enum nsurl_scheme_type scheme=nsurl_get_scheme_type(url);
    return (scheme==NSURL_SCHEME_HTTP||scheme==NSURL_SCHEME_HTTPS) && nsurl_has_component(url,NSURL_HOST);
}
static void *setup(struct fetch *parent,nsurl *url,bool only_2xx,bool downgrade,const char *post,
    const struct fetch_multipart_data *multipart,const char **headers)
{
    (void)downgrade; /* Never authorises a weaker protocol or certificate check. */
    struct ciuki_fetch *f=calloc(1,sizeof(*f));if(!f)return NULL;
    f->parent=parent;f->url=nsurl_ref(url);f->only_2xx=only_2xx;f->terminal=FETCH_FINISHED;
    lwc_string *host=nsurl_get_component(url,NSURL_HOST),*port=nsurl_get_component(url,NSURL_PORT);
    char *target=NULL,*cookie=NULL;size_t length=0;
    struct ciuki_request r={.tls=nsurl_get_scheme_type(url)==NSURL_SCHEME_HTTPS,.post=post,.headers=headers};
    r.port=r.tls?443:80;r.host=host?lwc_string_data(host):"";
    if(port){
        r.port=0;for(const char *s=lwc_string_data(port);*s;s++){
            if(*s<'0'||*s>'9'||r.port>6553){r.port=0;break;}r.port=r.port*10+(unsigned)(*s-'0');
        }
    }
    if(multipart)f->error="Multipart form uploads are unsupported.";
    else if(nsurl_has_component(url,NSURL_CREDENTIALS))f->error="HTTP URL credentials are unsupported.";
    else if(strlen(nsurl_access(url))>CIUKI_HTTP_URL)f->error="HTTP URL exceeds 4 KiB.";
    else if(nsurl_get(url,NSURL_PATH|NSURL_QUERY,&target,&length)!=NSERROR_OK)f->error="Unable to allocate HTTP request URL.";
    if(!f->error){
        cookie=urldb_get_cookie(url,true);r.cookie=cookie;r.target=target;
        f->job=ciuki_net_job(&r,http_event,f);
        if(!f->job){free(target);free(cookie);if(host)lwc_string_unref(host);if(port)lwc_string_unref(port);nsurl_unref(f->url);free(f);return NULL;}
    }
    free(target);free(cookie);if(host)lwc_string_unref(host);if(port)lwc_string_unref(port);
    f->next=jobs;jobs=f;return f;
}
static bool start(void *context)
{
    struct ciuki_fetch *f=context;
    if(f->error){f->active=true;return true;}
    if(!ciuki_net_start(network,f->job))return false;
    f->active=true;return true;
}
static void abort_fetch(void *context)
{
    struct ciuki_fetch *f=context;f->aborted=true;
    if(f->job)ciuki_net_cancel(f->job);
    if(!in_poll){fetch_remove_from_queues(f->parent);fetch_free(f->parent);}
}
static void free_fetch(void *context)
{
    struct ciuki_fetch *f=context,**link=&jobs;
    while(*link && *link!=f)link=&(*link)->next;
    if(*link)*link=f->next;
    ciuki_net_job_free(f->job);nsurl_unref(f->url);free(f);
}
static void poll_fetch(lwc_string *scheme)
{
    /* The shared pool is pumped exactly once when core visits both schemes. */
    if(lwc_string_length(scheme)!=4 || in_poll)return;
    in_poll=true;ciuki_net_poll(network);
    /* Callbacks may enqueue/abort other jobs: restart after each terminal callback
     * so no cached next pointer can refer to a reentrantly released fetch. */
    for(;;){
        struct ciuki_fetch *f;
        for(f=jobs;f;f=f->next)if((f->active||f->aborted) && (f->error||!f->job||f->job->finished))break;
        if(!f)break;
        fetch_msg msg={.type=f->terminal};
        if(f->error || (f->job && f->job->error)){
            msg.type=FETCH_ERROR;msg.data.error=f->error?f->error:f->job->error;
        } else if(msg.type==FETCH_REDIRECT)msg.data.redirect=f->job->http->location;
        fetch_remove_from_queues(f->parent);
        if(!f->aborted){observe(f);notify(f,&msg);}
        fetch_free(f->parent);
    }
    in_poll=false;
}
static int fdset(lwc_string *scheme,fd_set *read,fd_set *write,fd_set *error)
{
    if(lwc_string_length(scheme)!=4)return -1;
    struct pollfd fds[8];unsigned n=ciuki_net_fds(network,fds);int max=-1;
    for(unsigned i=0;i<n;i++)if(fds[i].fd>=0 && fds[i].fd<FD_SETSIZE){
        if(fds[i].events&POLLIN)FD_SET(fds[i].fd,read);
        if(fds[i].events&POLLOUT)FD_SET(fds[i].fd,write);
        FD_SET(fds[i].fd,error);if(fds[i].fd>max)max=fds[i].fd;
    }
    return max;
}
static void finalise(lwc_string *scheme)
{ (void)scheme;if(references && !--references){ciuki_net_destroy(network);network=NULL;} }
nserror fetch_ciuki_register(void)
{
    static const struct fetcher_operation_table operations={initialise,acceptable,setup,start,abort_fetch,free_fetch,poll_fetch,fdset,finalise};
    const char *names[]={"http","https"};
    for(unsigned i=0;i<2;i++){
        lwc_string *scheme;if(lwc_intern_string(names[i],strlen(names[i]),&scheme)!=lwc_error_ok)return NSERROR_NOMEM;
        nserror e=fetcher_add(scheme,&operations);if(e!=NSERROR_OK){lwc_string_unref(scheme);return e;}
    }
    return NSERROR_OK;
}
void ciuki_fetch_pump(void)
{
    /* fetch_fdset progresses all fetchers. poll is a readiness bridge only;
     * libnsfb owns the bounded (<=10 ms) UI wait, including desktop events. */
    if(network)ciuki_net_service_idle(network);
    fd_set read,write,error;int max=-1;
    fetch_fdset(&read,&write,&error,&max);
    struct pollfd fds[FD_SETSIZE];unsigned count=0;
    for(int fd=0;fd<=max && fd<FD_SETSIZE;fd++){
        short events=(FD_ISSET(fd,&read)?POLLIN:0)|(FD_ISSET(fd,&write)?POLLOUT:0);
        if(events)fds[count++]=(struct pollfd){fd,events,0};
    }
    if(count)poll(fds,count,0);
}
