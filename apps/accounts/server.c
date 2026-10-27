/* SPDX-License-Identifier: MIT
 * Serving loop of the sealed ACCOUNT process; see server.h. Every request
 * reaches the service through acct_frame; a request the service cannot admit
 * gets a status-only CSC1 reply so its client never waits for the kernel's
 * exchange timeout. Received frames are wiped: request bodies carry secrets.
 */
#include "server.h"
#include "kernel_sys.h"
#include "wire.h"
#include <ciuki/abi.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void note(const char *what, int code)
{
    char line[128];
    if (code) snprintf(line,sizeof(line),"[accounts] %s: %d",what,code);
    else snprintf(line,sizeof(line),"[accounts] %s",what);
    acct_sys_log(line);
}
/* Status-only reply to a complete request: same header (service, operation,
 * id and the pair's endpoint generation), reply flag, no body. */
static void refuse(int fd, const uint8_t *request, int status)
{
    struct ciuki_message m; memset(&m,0,sizeof(m));
    memcpy(m.data,request,64);
    w32(m.data+20,2); w32(m.data+36,0); w32(m.data+48,(uint32_t)status); w32(m.data+52,0); w32(m.data+56,0);
    m.length=64;
    (void)acct_sys_channel_send(fd,&m);
}
static int request_complete(const struct ciuki_message *m)
{
    const uint8_t *h=m->data;
    return m->length>=64 && a32(h+20)==1 && a32(h+56)+a32(h+36)==a32(h+52);
}
static void drop_exchanges(struct acct_server *s)
{
    for (unsigned i=0;i<8;i++)
        if (s->transport.exchanges[i].occupied)
            acct_transport_drop(&s->transport,&s->service,s->transport.exchanges[i].endpoint);
    memset(&s->transport,0,sizeof(s->transport));
}
/* Faulted storage or teardown: no exchange may outlive the service state. */
static void server_reset(struct acct_server *s)
{
    if (!s->opened) return;
    drop_exchanges(s);
    acct_service_close(&s->service);
    s->opened=0;
}
static void link_closed(struct acct_server *s, int fd)
{
    if (s->opened) acct_transport_drop(&s->transport,&s->service,fd);
    (void)acct_kernel_adapter_retire(&s->kernel,fd); /* deferred while referenced */
}

int acct_server_init(struct acct_server *s, const struct acct_store *tables, int host_test)
{
    if (!s || !tables) return -EINVAL;
    memset(s,0,sizeof(*s));
    s->tables=*tables;
    s->host_test=host_test;
    return acct_kernel_adapter_init(&s->kernel,&s->tables.kernel);
}
int acct_server_open(struct acct_server *s, uint64_t now)
{
    if (s->opened) return 1;
    if (now<s->next_open) return 0;
    int e=acct_service_open(&s->service,&s->tables,s->host_test);
    if (!e) {
        s->opened=1; s->last_open_error=0;
        note(s->service.available ? "open; account database available" : "open; no published database",0);
        return 1;
    }
    if (e!=s->last_open_error) note("open refused; every request is answered with EIO",e);
    s->last_open_error=e;
    s->next_open=now+ACCT_SERVER_OPEN_RETRY_MS;
    return 0;
}
void acct_server_accept(struct acct_server *s)
{
    static const uint32_t services[]={ ACCT_ENDPOINT_AUTH, ACCT_ENDPOINT_ACCOUNTS };
    for (unsigned i=0;i<2;i++)
        for (unsigned n=0;n<ACCT_KERNEL_LINKS;n++) {
            int fd=-1, e=acct_kernel_adapter_accept(&s->kernel,services[i],&fd);
            if (!e) continue;
            /* ENOSPC: every slot is in use; the pending links time out in
             * the kernel and their clients retry. */
            if (e!=-EAGAIN && e!=-ENOSPC) note("accept failed",e);
            break;
        }
}
void acct_server_serve(struct acct_server *s, int fd)
{
    struct ciuki_message m;
    for (unsigned burst=0;burst<64;burst++) {
        int r=acct_sys_channel_recv(fd,&m,0);
        if (r==-EAGAIN) return;
        if (r<=0) { link_closed(s,fd); return; }
        if (!s->opened) { if (request_complete(&m)) refuse(fd,m.data,-EIO); }
        else {
            int e=acct_frame(&s->transport,&s->service,fd,m.data,m.length,&s->response);
            if (e<0 && request_complete(&m)) refuse(fd,m.data,e);
            acct_wipe(&s->response,sizeof(s->response));
        }
        acct_wipe(&m,sizeof(m));
    }
}
void acct_server_sweep(struct acct_server *s)
{
    int fds[ACCT_KERNEL_LINKS];
    unsigned n=acct_kernel_adapter_fds(&s->kernel,fds,ACCT_KERNEL_LINKS);
    for (unsigned i=0;i<n;i++)
        if (acct_kernel_adapter_link_live(&s->kernel,fds[i])==0) link_closed(s,fds[i]);
}
unsigned acct_server_work(struct acct_server *s, unsigned idle_ms)
{
    if (!s->opened) return idle_ms;
    unsigned wait=idle_ms; int w;
    while ((w=acct_work(&s->service,&s->response))==1) acct_wipe(&s->response,sizeof(s->response));
    if (w==-EAGAIN && s->response.retry_after_ms && s->response.retry_after_ms<wait) wait=s->response.retry_after_ms;
    acct_wipe(&s->response,sizeof(s->response));
    /* A storage fault closes authentication until close/open recovery. */
    if (s->service.faulted && !s->service.active.occupied) {
        note("storage fault; closing for recovery",-EIO);
        server_reset(s);
        s->next_open=0;
    }
    return wait;
}
void acct_server_step(struct acct_server *s, unsigned idle_ms)
{
    acct_server_open(s,acct_sys_monotonic_ms());
    acct_server_accept(s);
    unsigned wait=acct_server_work(s,idle_ms);
    int fds[ACCT_KERNEL_LINKS];
    unsigned n=acct_kernel_adapter_fds(&s->kernel,fds,ACCT_KERNEL_LINKS);
    if (acct_sys_wait_readable(fds,n,wait)>0)
        for (unsigned i=0;i<n;i++) acct_server_serve(s,fds[i]);
    if (++s->sweep>=8) { s->sweep=0; acct_server_sweep(s); }
}
void acct_server_close(struct acct_server *s)
{
    server_reset(s);
    int fds[ACCT_KERNEL_LINKS];
    unsigned n=acct_kernel_adapter_fds(&s->kernel,fds,ACCT_KERNEL_LINKS);
    for (unsigned i=0;i<n;i++) (void)acct_kernel_adapter_retire(&s->kernel,fds[i]);
    acct_kernel_adapter_shutdown(&s->kernel);
}
