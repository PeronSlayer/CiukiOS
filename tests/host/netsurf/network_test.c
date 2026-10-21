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
static bool cancel;
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *t)
{
    int r=__real_clock_gettime(clock,t);
    if(!r && clock==CLOCK_REALTIME && !strcmp(mode,"dated"))t->tv_sec=1735689600;
    if(!r && clock==CLOCK_REALTIME && (!strcmp(mode,"expired")||!strcmp(mode,"valid")||!strcmp(mode,"wrong")||!strcmp(mode,"tls10")||!strcmp(mode,"future")))t->tv_sec=1790000000;
    return r;
}
int __real_getentropy(void *,size_t);
int __wrap_getentropy(void *p,size_t n)
{ if(!strcmp(mode,"entropy")){errno=ENOSYS;return -1;}return __real_getentropy(p,n); }
static bool callback(void *ctx,enum ciuki_http_event e,const void *data,size_t n)
{
    (void)ctx;(void)data;
    if(e==CIUKI_HTTP_HEADER){headers++;if(cancel)return false;}
    if(e==CIUKI_HTTP_DATA){received+=n;data_events++;}
    return true;
}
static void drive(struct ciuki_net *net,struct ciuki_job *job)
{
    assert(ciuki_net_start(net,job));
    struct timespec start,now;__real_clock_gettime(CLOCK_MONOTONIC,&start);
    while(!job->finished){
        ciuki_net_poll(net);
        if(!strcmp(mode,"timeout") && headers && !job->finished)job->started_ms-=31000;
        if(!strcmp(mode,"cancel") && headers)ciuki_net_cancel(job);
        struct pollfd fds[8];unsigned n=ciuki_net_fds(net,fds);poll(fds,n,1);
        __real_clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-start.tv_sec<15);
    }
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
int main(int argc,char **argv)
{
    assert(argc==6);mode=argv[1];
    setvbuf(stdout,NULL,_IOLBF,0);
    extern void fixture_begin(const char *,const char *,const char *),fixture_end(void);
    extern unsigned fixture_connections(void);
    fixture_begin(mode,argv[4],argv[5]);
    struct ciuki_net *net=ciuki_net_create(ciuki_anchors,CIUKI_ANCHOR_COUNT);assert(net);
    request_checks();
    if(!strcmp(mode,"pool")){
        pool_checks(net);ciuki_net_destroy(net);usleep(20000);fixture_end();puts("PASS eight connections/two per origin/cancellation/request bounds");return 0;
    }
    bool secure=!strcmp(mode,"valid")||!strcmp(mode,"expired")||!strcmp(mode,"future")||!strcmp(mode,"wrong")||!strcmp(mode,"self")||!strcmp(mode,"tls10")||!strcmp(mode,"dated")||!strcmp(mode,"entropy")||!strcmp(mode,"numeric");
    struct ciuki_request request={.host=!strcmp(mode,"numeric")?"127.0.0.1":!strcmp(mode,"offline")?"192.0.2.1":"fixture.example",.target=argv[3],.port=(unsigned)atoi(argv[2]),.tls=secure};
    if(!strcmp(mode,"post"))request.post="dog=Ciuki&step=M2";
    cancel=!strcmp(mode,"callback-cancel");
    struct ciuki_job *job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
    if(!strcmp(mode,"valid")||!strcmp(mode,"ok")||!strcmp(mode,"reuse")||!strcmp(mode,"post")||!strcmp(mode,"stale")||!strcmp(mode,"post-stale")){
        if(job->error)fprintf(stderr,"unexpected: %s\n",job->error);
        assert(!job->error && job->http.done && received==5);
        if(!strcmp(mode,"reuse")){
            ciuki_net_job_free(job);received=0;
            job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
            assert(job->pooled && !job->error && received==5 && fixture_connections()==1);
        }
        if(!strcmp(mode,"stale")||!strcmp(mode,"post-stale")){
            ciuki_net_job_free(job);received=0;usleep(500000);
            if(!strcmp(mode,"post-stale"))request.post="dog=Ciuki&step=M2";
            job=ciuki_net_job(&request,callback,NULL);assert(job);drive(net,job);
            if(!strcmp(mode,"stale"))assert(!job->error && job->retried && received==5 && fixture_connections()==2);
            else assert(job->error && !job->retried && fixture_connections()==1);
        }
    } else if(!strcmp(mode,"callback-cancel")||!strcmp(mode,"cancel"))assert(job->cancelled && !received);
    else {
        assert(job->error);printf("EXPECTED %s: %s\n",mode,job->error);
        if(!strcmp(mode,"wrong"))assert(job->tls_error==CIUKI_TLS_HOST);
        if(!strcmp(mode,"expired")||!strcmp(mode,"future"))assert(job->tls_error==CIUKI_TLS_DATE);
        if(!strcmp(mode,"self"))assert(job->tls_error==CIUKI_TLS_CERT);
        if(!strcmp(mode,"tls10"))assert(job->tls_error==CIUKI_TLS_PROTOCOL);
        if(!strcmp(mode,"dated"))assert(job->tls_error==CIUKI_TLS_CLOCK);
        if(!strcmp(mode,"entropy"))assert(job->tls_error==CIUKI_TLS_ENTROPY);
    }
    ciuki_net_job_free(job);ciuki_net_destroy(net);
    /* Allow the detached resolver to drop its final owned state before LSan. */
    usleep(20000);
    fixture_end();
    printf("PASS %s headers=%u slices=%u bytes=%zu\n",mode,headers,data_events,received);
    return 0;
}
