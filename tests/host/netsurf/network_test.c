/* Host nonblocking engine against scripted HTTP and BearSSL fixtures.
 * SPDX-License-Identifier: MIT */
#include "sdk/netsurf/port/net.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fixture-anchors.h"
static const char *mode;
static unsigned headers,data_events;
static size_t received;
static unsigned char payload[64];
static struct ciuki_tls *last_tls;
static bool cancel;
static _Thread_local uint64_t monotonic_offset;
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *t)
{
    int r=__real_clock_gettime(clock,t);
    if(!r && clock==CLOCK_MONOTONIC && monotonic_offset){t->tv_sec+=(time_t)monotonic_offset;monotonic_offset=0;}
    if(!r && clock==CLOCK_REALTIME && !strcmp(mode,"dated"))t->tv_sec=1735689600;
    if(!r && clock==CLOCK_REALTIME && (!strcmp(mode,"valid")||!strcmp(mode,"expired")||!strcmp(mode,"future")||
        !strcmp(mode,"wrong")||!strcmp(mode,"tls10")||!strncmp(mode,"tls-",4)||!strcmp(mode,"intermediate")||
        !strcmp(mode,"missing-intermediate")||!strcmp(mode,"malformed-chain")||!strncmp(mode,"cert-",5)||!strcmp(mode,"fallback-clock")))t->tv_sec=1790000000;
    return r;
}
int __real_getentropy(void *,size_t);
int __wrap_getentropy(void *p,size_t n)
{ if(!strcmp(mode,"entropy")){errno=ENOSYS;return -1;}return __real_getentropy(p,n); }
static bool callback(void *ctx,enum ciuki_http_event e,const void *data,size_t n)
{
    (void)ctx;(void)data;
    if(e==CIUKI_HTTP_HEADER){headers++;if(cancel)return false;}
    if(e==CIUKI_HTTP_DATA){if(received+n<=sizeof(payload))memcpy(payload+received,data,n);received+=n;data_events++;}
    return true;
}
static void drive(struct ciuki_net *net,struct ciuki_job *job)
{
    assert(ciuki_net_start(net,job));
    struct timespec start,now;__real_clock_gettime(CLOCK_MONOTONIC,&start);
    while(!job->finished){
        size_t sent=job->sent;
        last_tls=ciuki_net_tls(job);ciuki_net_poll(net);
        /* Initial handshake records must not qualify as request transmission;
         * this poll only submits the first plaintext, for a later pump. */
        if(job->secure && !sent && job->sent)assert(!job->transmitted);
        if(!strcmp(mode,"timeout") && headers && !job->finished)job->started_ms-=31000;
        if(!strcmp(mode,"cancel") && headers)ciuki_net_cancel(job);
        struct pollfd fds[8];unsigned n=ciuki_net_fds(net,fds);poll(fds,n,1);
        __real_clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-start.tv_sec<15);
    }
}
static int readable_socket(struct ciuki_net *net)
{
    struct pollfd fds[8];assert(ciuki_net_fds(net,fds)==1 && (fds[0].events&POLLIN));
    fds[0].events=POLLIN;
    for(unsigned tries=0;tries<1000;tries++){
        if(poll(fds,1,0)>0){assert(fds[0].revents==POLLIN);return fds[0].fd;}
        usleep(1000);
    }
    assert(!"fixture did not deliver the scripted response");return -1;
}
static void request_checks(void)
{
    struct ciuki_request r={.host="fixture.example",.target="/",.port=8080};
    const char *headers_bad[]={"Host: injected",NULL};r.headers=headers_bad;
    struct ciuki_job *j=ciuki_net_job(&r,callback,NULL);assert(j && j->error);ciuki_net_job_free(j);
    r.headers=NULL;r.target="/bad\r\nInjected: yes";j=ciuki_net_job(&r,callback,NULL);assert(j && j->error);ciuki_net_job_free(j);
    r.target="/";char *post=malloc(CIUKI_HTTP_POST+2);assert(post);memset(post,'x',CIUKI_HTTP_POST+1);post[CIUKI_HTTP_POST+1]=0;
    r.post=post;j=ciuki_net_job(&r,callback,NULL);assert(j && j->error);ciuki_net_job_free(j);free(post);
    r.post=NULL;r.cookie="session=x\r\nInjected: yes";j=ciuki_net_job(&r,callback,NULL);assert(j && j->error);ciuki_net_job_free(j);
}
static void pool_checks(struct ciuki_net *net)
{
    struct ciuki_job *jobs[8];char host[32];
    struct ciuki_request request={.target="/slow",.port=8080};
    for(unsigned i=0;i<8;i++){
        snprintf(host,sizeof(host),"192.0.2.%u",1+i/2);request.host=host;
        jobs[i]=ciuki_net_job(&request,callback,NULL);assert(jobs[i] && ciuki_net_start(net,jobs[i]));
        if(i==1){struct ciuki_job *third=ciuki_net_job(&request,callback,NULL);assert(third && !ciuki_net_start(net,third));ciuki_net_job_free(third);}
    }
    request.host="192.0.2.5";struct ciuki_job *ninth=ciuki_net_job(&request,callback,NULL);assert(ninth && !ciuki_net_start(net,ninth));ciuki_net_job_free(ninth);
    for(unsigned i=0;i<8;i++)ciuki_net_cancel(jobs[i]);
    ciuki_net_poll(net);
    for(unsigned i=0;i<8;i++){assert(jobs[i]->finished);ciuki_net_job_free(jobs[i]);}
}
static void fanout(struct ciuki_net *net)
{
    struct ciuki_request r={.host="fixture.example",.target="/slow",.port=8080};
    struct ciuki_job *active[2],*queued[1024];size_t n=0;
    for(unsigned i=0;i<2;i++){active[i]=ciuki_net_job(&r,callback,NULL);assert(active[i] && ciuki_net_start(net,active[i]));}
    while(n<1024){
        struct ciuki_job *j=ciuki_net_job(&r,callback,NULL);if(!j)break;
        assert(!j->http && !j->request && !ciuki_net_start(net,j));queued[n++]=j;
        assert(ciuki_net_pending_bytes()<=CIUKI_NET_PENDING_BUDGET);
    }
    assert(n>100 && n<1024);printf("pending_fanout=%zu pending_bytes=%zu budget=%u\n",n,ciuki_net_pending_bytes(),CIUKI_NET_PENDING_BUDGET);
    for(size_t i=0;i<n;i++)ciuki_net_job_free(queued[i]);
    for(unsigned i=0;i<2;i++){assert(active[i]->request_bytes<1024);ciuki_net_cancel(active[i]);ciuki_net_job_free(active[i]);}
    assert(!ciuki_net_pending_bytes());
}
static void dns_cache(struct ciuki_net *net)
{
    extern void fixture_dns_expire(void),fixture_dns_change(void);
    extern unsigned fixture_dns_queries(void);
    struct ciuki_request r={.host="fixture.example",.target="/stale",.port=8080};unsigned queries=0;
    for(unsigned step=0;step<4;step++){
        if(step==2)fixture_dns_expire();if(step==3)fixture_dns_change();
        struct ciuki_job *j=ciuki_net_job(&r,callback,NULL);assert(j);drive(net,j);assert(!j->error && j->http->done);
        unsigned after=fixture_dns_queries();
        if(step==1)assert(after==queries);else assert(after>queries);
        queries=after;ciuki_net_job_free(j);usleep(500000);ciuki_net_service_idle(net);
    }
    puts("PASS DNS TTL expiry/cache hit/server generation change through fetcher");
}
int main(int argc,char **argv)
{
    assert(argc==6);mode=argv[1];
    setvbuf(stdout,NULL,_IOLBF,0);
    extern void fixture_begin(const char *,const char *,const char *),fixture_end(void);
    extern unsigned fixture_connections(void),fixture_requests(void);
    fixture_begin(mode,argv[4],argv[5]);
    struct ciuki_net *net=ciuki_net_create(ciuki_anchors,CIUKI_ANCHOR_COUNT);assert(net);
    request_checks();
    if(!strcmp(mode,"pool")){
        pool_checks(net);ciuki_net_destroy(net);usleep(20000);fixture_end();puts("PASS eight connections/two per origin/cancellation/request bounds");return 0;
    }
    if(!strcmp(mode,"fanout")||!strcmp(mode,"dns-cache")){
        if(!strcmp(mode,"fanout"))fanout(net);else dns_cache(net);
        ciuki_net_destroy(net);usleep(20000);fixture_end();return 0;
    }
    bool secure=!strcmp(mode,"valid")||!strcmp(mode,"expired")||!strcmp(mode,"future")||!strcmp(mode,"wrong")||!strcmp(mode,"self")||!strcmp(mode,"tls10")||!strcmp(mode,"dated")||!strcmp(mode,"entropy")||!strcmp(mode,"numeric")||!strcmp(mode,"fallback-clock")||!strncmp(mode,"tls-",4)||!strcmp(mode,"intermediate")||!strcmp(mode,"missing-intermediate")||!strcmp(mode,"malformed-chain")||!strcmp(mode,"cert-depth")||!strcmp(mode,"cert-buffer");
    struct ciuki_request request={.host=!strcmp(mode,"numeric")?"127.0.0.1":!strcmp(mode,"offline")?"192.0.2.1":"fixture.example",.target=argv[3],.port=(unsigned)atoi(argv[2]),.tls=secure};
    if(!strcmp(mode,"loopback-offline"))request.host="localhost";
    if(!strcmp(mode,"post"))request.post="dog=Ciuki&step=M2";
    cancel=!strcmp(mode,"callback-cancel");
    struct ciuki_job *job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
    if(!strcmp(mode,"valid")||!strcmp(mode,"ok")||!strcmp(mode,"reuse")||!strcmp(mode,"post")||!strcmp(mode,"stale")||!strcmp(mode,"post-stale")||!strcmp(mode,"intermediate")||!strcmp(mode,"tls-reuse")||!strcmp(mode,"unsolicited")||!strcmp(mode,"tls-unsolicited")||!strcmp(mode,"idle-expiry")||!strcmp(mode,"idle-reuse-expiry")||!strcmp(mode,"tls-buffered")||!strcmp(mode,"unsolicited-recheck")||!strcmp(mode,"tls-recheck")||!strcmp(mode,"tls-submit-recheck")||!strcmp(mode,"tls-send-recheck")||!strcmp(mode,"tls-hello-request")||!strcmp(mode,"tls-before-first-send")||!strncmp(mode,"tls-partial-",12)||!strcmp(mode,"stale-retry")||!strcmp(mode,"post-no-replay")||!strcmp(mode,"tls-stale-retry")||!strcmp(mode,"tls-post-no-replay")){
        if(job->error)fprintf(stderr,"unexpected: %s\n",job->error);
        assert(!job->error && job->http->done && received==5 && !memcmp(payload,"Ciuki",5));
        if(!strcmp(mode,"reuse")||!strcmp(mode,"tls-reuse")){
            ciuki_net_job_free(job);received=0;
            job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
            assert(job->pooled && job->transmitted && !job->error && received==5 && !memcmp(payload,"Ciuki",5) && fixture_connections()==1);
        }
        if(!strcmp(mode,"tls-hello-request")||!strcmp(mode,"tls-before-first-send")){
            extern void fixture_incoming_after_check(bool,const bool *);
            extern unsigned fixture_injected_alerts(void),fixture_injected_handshakes(void);
            extern bool fixture_injected_closed(void);
            bool hello=!strcmp(mode,"tls-hello-request");unsigned previous_headers=headers;
            ciuki_net_job_free(job);received=0;
            job=ciuki_net_job(&request,callback,NULL);assert(job && ciuki_net_start(net,job) && job->pooled && !job->sent && !job->transmitted);
            fixture_incoming_after_check(hello,&job->transmitted);ciuki_net_poll(net);
            assert(fixture_injected_closed() && !fixture_injected_handshakes());
            assert(fixture_injected_alerts()==(hello?1u:0u));
            assert(job->retried && !job->transmitted && !job->sent && !job->http->header_bytes && !received && headers==previous_headers);
            assert(fixture_connections()==1 && fixture_requests()==1);
            drive(net,job);
            assert(!job->error && job->transmitted && received==5 && !memcmp(payload,"Ciuki",5));
            assert(fixture_connections()==2 && fixture_requests()==2);
            printf("PASS after-check %s: closed, no ClientHello, alerts=%u, no response misassociation\n",hello?"HelloRequest":"unsolicited response",fixture_injected_alerts());
        }
        if(!strcmp(mode,"unsolicited-recheck")||!strcmp(mode,"tls-recheck")){
            extern void fixture_unsolicited(void);
            ciuki_net_job_free(job);received=0;usleep(500000);
            job=ciuki_net_job(&request,callback,NULL);assert(job && ciuki_net_start(net,job) && job->pooled && !job->sent);
            fixture_unsolicited();usleep(500000);drive(net,job);
            assert(!job->error && job->retried && received==5 && !memcmp(payload,"Ciuki",5) && fixture_connections()==2);
        }
        if(!strcmp(mode,"tls-submit-recheck")||!strcmp(mode,"tls-send-recheck")){
            extern void fixture_unsolicited(void),fixture_block_send(bool);
            extern unsigned fixture_blocked_sends(void);
            ciuki_net_job_free(job);received=0;
            job=ciuki_net_job(&request,callback,NULL);assert(job && ciuki_net_start(net,job) && job->pooled);
            ciuki_net_poll(net);
            assert(job->sent==job->request_bytes && !job->transmitted && !job->finished && fixture_requests()==1);
            if(!strcmp(mode,"tls-send-recheck")){
                fixture_block_send(true);ciuki_net_poll(net);fixture_block_send(false);
                assert(fixture_blocked_sends()==1 && !job->transmitted && !job->error && !job->retried && fixture_requests()==1);
            }
            fixture_unsolicited();readable_socket(net);drive(net,job);
            assert(!job->error && job->retried && job->transmitted && received==5 && !memcmp(payload,"Ciuki",5));
            assert(fixture_connections()==2 && fixture_requests()==2);
        }
        if(!strcmp(mode,"unsolicited")||!strcmp(mode,"tls-unsolicited")||!strcmp(mode,"tls-buffered")||!strcmp(mode,"idle-expiry")||!strcmp(mode,"idle-reuse-expiry")||!strncmp(mode,"tls-partial-",12)){
            ciuki_net_job_free(job);received=0;
            struct pollfd fds[8];assert(ciuki_net_fds(net,fds)==1 && (fds[0].events&POLLIN));
            if(strstr(mode,"expiry"))monotonic_offset=16;else usleep(500000);
            if(!strcmp(mode,"tls-buffered")){
                assert(last_tls);size_t bytes=0;const unsigned char *app=NULL;
                for(unsigned tries=0;tries<1000;tries++){
                    size_t budget=CIUKI_HTTP_SLICE;assert(ciuki_tls_pump(last_tls,fds[0].fd,&budget,NULL)==CIUKI_TLS_OK);
                    app=ciuki_tls_read(last_tls,&bytes);if(app && bytes)break;usleep(1000);
                }
                assert(app && bytes);unsigned char byte;assert(recv(fds[0].fd,&byte,1,MSG_PEEK|MSG_DONTWAIT)<0 && errno==EAGAIN);
            }
            if(!strncmp(mode,"tls-partial-",12)){
                extern void fixture_unsolicited_prefix(size_t);
                size_t prefix=!strcmp(mode,"tls-partial-header")?2:7;
                assert(last_tls && !ciuki_tls_pending_input(last_tls));
                fixture_unsolicited_prefix(prefix);int fd=readable_socket(net);
                size_t budget=CIUKI_HTTP_SLICE,bytes;
                assert(ciuki_tls_pump(last_tls,fd,&budget,NULL)==CIUKI_TLS_OK && CIUKI_HTTP_SLICE-budget==prefix);
                assert(!ciuki_tls_read(last_tls,&bytes) && !bytes && ciuki_tls_pending_input(last_tls));
                unsigned char byte;assert(recv(fd,&byte,1,MSG_PEEK|MSG_DONTWAIT)<0 && errno==EAGAIN);
            }
            if(strcmp(mode,"idle-reuse-expiry")){ciuki_net_service_idle(net);assert(!ciuki_net_fds(net,fds));}
            request.target="/";job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
            assert(!job->error && !job->pooled && received==5 && !memcmp(payload,"Ciuki",5) && fixture_connections()==2);
        }
        if(!strcmp(mode,"stale")||!strcmp(mode,"post-stale")){
            ciuki_net_job_free(job);received=0;usleep(500000);
            if(!strcmp(mode,"post-stale"))request.post="dog=Ciuki&step=M2";
            job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
            if(!strcmp(mode,"stale"))assert(!job->error && !job->pooled && !job->retried && received==5 && !memcmp(payload,"Ciuki",5) && fixture_connections()==2);
            else assert(!job->error && !job->pooled && !job->retried && received==5 && !memcmp(payload,"Ciuki",5) && fixture_connections()==2);
        }
        if(!strcmp(mode,"stale-retry")||!strcmp(mode,"post-no-replay")||!strcmp(mode,"tls-stale-retry")||!strcmp(mode,"tls-post-no-replay")){
            ciuki_net_job_free(job);received=0;
            bool post=strstr(mode,"no-replay")!=NULL;
            if(post)request.post="dog=Ciuki&step=M2";
            job=ciuki_net_job(&request,callback,NULL);assert(job && ciuki_net_start(net,job) && job->pooled && !job->transmitted);
            drive(net,job);assert(job->transmitted);
            if(post){
                assert(job->error && !job->retried && job->pooled && !job->http->header_bytes && !received);
                assert(fixture_connections()==1 && fixture_requests()==2);
            } else {
                assert(!job->error && job->retried && !job->pooled && received==5 && !memcmp(payload,"Ciuki",5));
                assert(fixture_connections()==2 && fixture_requests()==3);
            }
        }
    } else if(!strcmp(mode,"callback-cancel")||!strcmp(mode,"cancel"))assert(job->cancelled && !received);
    else {
        assert(job->error);printf("EXPECTED %s: %s\n",mode,job->error);
        if(!strcmp(mode,"wrong"))assert(job->tls_error==CIUKI_TLS_HOST);
        if(!strcmp(mode,"expired")||!strcmp(mode,"future"))assert(job->tls_error==CIUKI_TLS_DATE);
        if(!strcmp(mode,"self"))assert(job->tls_error==CIUKI_TLS_CERT);
        if(!strcmp(mode,"tls10"))assert(job->tls_error==CIUKI_TLS_PROTOCOL);
        if(!strcmp(mode,"dated")||!strcmp(mode,"fallback-clock"))assert(job->tls_error==CIUKI_TLS_CLOCK);
        if(!strcmp(mode,"malformed-chain")||!strcmp(mode,"missing-intermediate")||!strcmp(mode,"cert-depth")||!strcmp(mode,"cert-buffer"))assert(job->tls_error==CIUKI_TLS_CERT);
        if(!strcmp(mode,"tls-truncated"))assert(job->tls_error==CIUKI_TLS_IO || strstr(job->error,"Truncated"));
        if(!strcmp(mode,"hostname-offline"))assert(strstr(job->error,"Offline:"));
        if(!strcmp(mode,"loopback-offline"))assert(strstr(job->error,"refused") && !strstr(job->error,"Offline:"));
        /* f6-22: failure pages state the network: no address / address and DNS. */
        if(!strcmp(mode,"offline")||!strcmp(mode,"hostname-offline")||!strcmp(mode,"loopback-offline")){
            assert(strstr(job->error," Network: no address yet (e100, link up); waiting for a DHCP server."));
            assert(!strcmp(ciuki_net_state_word(),"no-address"));
        }
        if(!strcmp(mode,"dns")){
            assert(strstr(job->error,"DNS lookup failed: the server name could not be resolved. Network: address 192.0.2.100 (e100); DNS lookups failing."));
            assert(!strcmp(ciuki_net_state_word(),"dns-failed"));
        }
        if(!strcmp(mode,"refused")){
            assert(strstr(job->error,"The server refused the connection. Network: address 192.0.2.100 (e100); DNS ok."));
            assert(!strcmp(ciuki_net_state_word(),"dns-ok"));
        }
        if(!strcmp(mode,"entropy"))assert(job->tls_error==CIUKI_TLS_ENTROPY);
    }
    ciuki_net_job_free(job);ciuki_net_destroy(net);
    /* Allow the detached resolver to drop its final owned state before LSan. */
    usleep(20000);
    fixture_end();
    printf("PASS %s headers=%u slices=%u bytes=%zu\n",mode,headers,data_events,received);
    return 0;
}
