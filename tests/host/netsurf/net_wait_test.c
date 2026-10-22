/* SPDX-License-Identifier: MIT
 * Production connection deadlines: pending DNS, active requests, keepalive,
 * buffered plaintext, and partial ciphertext waiting for socket input. */
#include <time.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static uint64_t model_ms;
static int model_clock(clockid_t id,struct timespec *t)
{ (void)id;t->tv_sec=model_ms/1000;t->tv_nsec=(model_ms%1000)*1000000;return 0; }
#define clock_gettime model_clock
#include "sdk/netsurf/port/net.c"
#undef clock_gettime
static unsigned tls_state;
unsigned ciuki_tls_state(struct ciuki_tls *t) { assert(t);return tls_state; }
#include "apps/libdeskui/completion.h"
static struct ciuki_net *network;
static struct du_completion completion;
static unsigned wait_calls;
static int completion_event, socket_event;
static int model_poll(struct pollfd *fds,nfds_t n,int timeout)
{
    assert(fds[0].fd==55 && fds[0].events==POLLIN);
    assert(fds[1].fd==completion.fd[0] && fds[1].events==POLLIN);
    assert(n==(socket_event ? 3u : 2u));
    assert(timeout==(socket_event ? 30000 : -1));
    if(socket_event)assert(fds[2].fd==46 && fds[2].events==POLLOUT);
    wait_calls++;model_ms+=10000;
    if(completion_event){du_completion_notify(&completion);fds[1].revents=POLLIN;}
    else fds[0].revents=POLLIN;
    return 1;
}
#define poll model_poll
#include "fetch_event_wait.inc"
#undef poll
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    struct ciuki_net n={0};model_ms=1000;
    assert(ciuki_net_timeout(&n,-1)==-1);
    struct ciuki_job job={.started_ms=model_ms};
    n.connections[0]=(struct ciuki_connection){.state=RESOLVING,.fd=-1,.job=&job};
    assert(ciuki_net_timeout(&n,-1)==30000 && ciuki_net_timeout(&n,10000)==10000);
    model_ms+=10000;assert(ciuki_net_timeout(&n,-1)==20000);
    model_ms+=20000;assert(ciuki_net_timeout(&n,-1)==0);
    n.connections[0]=(struct ciuki_connection){.state=IDLE,.idle_ms=model_ms};
    assert(ciuki_net_timeout(&n,-1)==15000);
    n.connections[0]=(struct ciuki_connection){.state=TRANSFER,.job=&job,.tls=(void *)&job};
    job.started_ms=model_ms;tls_state=BR_SSL_RECVREC;
    assert(ciuki_net_timeout(&n,-1)==30000); /* partial record cannot spin */
    tls_state=BR_SSL_RECVAPP;assert(ciuki_net_timeout(&n,-1)==0);
    /* Execute the fetcher's production wait, including real completion-token
     * consumption. It must only collect readiness after schedule_run(). */
    n=(struct ciuki_net){0};network=&n;
    for(unsigned i=0;i<8;i++)n.connections[i].fd=-1;
    assert(!du_completion_init(&completion));
    assert(event_wait(55,-1)==1 && wait_calls==1);
    completion_event=1;assert(event_wait(55,-1)==1 && wait_calls==2);
    completion_event=0;socket_event=1;job.started_ms=model_ms;
    n.connections[0]=(struct ciuki_connection){.state=CONNECTING,.fd=46,.job=&job};
    assert(event_wait(55,-1)==1 && wait_calls==3);
    du_completion_destroy(&completion);
    puts("PASS NetSurf event deadlines: infinite idle, DNS/request timeout, keepalive expiry, buffered plaintext, no partial-record spin");
    puts("PASS NetSurf production wait: 10 s no idle timeout, input/completion/socket readiness, no fetch callbacks after scheduler timeout");
    return 0;
}
