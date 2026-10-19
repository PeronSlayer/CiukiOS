/* SPDX-License-Identifier: MIT */
#include "v3.h"
#include <errno.h>
#include <string.h>
int du3_connect(struct du3_connection *c, int channel, const struct du3_transport *t)
{
    if (!c || !t || !t->send || !t->recv || !t->info || !t->close || channel<0) { errno=EINVAL; return -1; }
    memset(c,0,sizeof(*c)); c->channel=channel; c->transport=*t;
    struct ciuki_message m; desk_v3_message(&m,DESK_HELLO,0,0,0,0);
    return t->send(channel,&m,DONTWAIT);
}
uint32_t du3_request(struct du3_connection *c, unsigned op, uint32_t id,
                     uint32_t generation, const void *payload, unsigned bytes, int fd)
{
    if (!c || c->disconnected) { errno=EPIPE; return 0; }
    if (bytes>240 || (bytes && !payload)) { errno=EINVAL; return 0; }
    struct ciuki_message m; struct desk_packet p; struct ciuki_surface_info info={0};
    uint32_t token=c->request+1; if (!token) token=1;
    desk_v3_message(&m,op,bytes,token,id,generation);
    if (bytes) memcpy(m.data+16,payload,bytes);
    if (fd>=0) { m.fd_count=1; m.fds[0]=fd; if (c->transport.info(fd,&info)) return 0; }
    /* DAMAGE needs the retained window's geometry at the server. Validate its
     * wire bounds locally with the maximum supported XRGB8888 allocation. */
    if (op==DESK_DAMAGE) info=(struct ciuki_surface_info){.size=sizeof(info),.width=2048,.height=2048,
        .stride=8192,.format=CIUKI_SURFACE_XRGB8888,.allocation_bytes=16777216};
    if (!desk_parse(&m,1,&info,&p) || op==DESK_HELLO || op==DESK_PONG) { errno=EINVAL; return 0; }
    if (c->transport.send(c->channel,&m,DONTWAIT)<0) return 0;
    c->request=token; return token;
}
int du3_next(struct du3_connection *c, struct desk_packet *p)
{
    if (!c || !p || c->disconnected) { errno=EPIPE; return -1; }
    struct ciuki_message m; int r=c->transport.recv(c->channel,&m,DONTWAIT);
    if (r==0) c->disconnected=1;
    if (r!=1) return r;
    int valid=m.data[0]==DESK_V3_VERSION && desk_parse(&m,0,NULL,p);
    for (unsigned k=0;k<m.fd_count && k<4;k++) if (m.fds[k]>=0) c->transport.close(m.fds[k]);
    if (!valid) { c->disconnected=1; errno=EINVAL; return -1; }
    /* The additive lease tuple stays with its window event in desk_packet;
     * callers must replace cached FRAME tokens on every grant. */
    if (p->opcode==DESK_V3_LEASE_EVENT && p->flags!=1) memset(&p->lease,0,sizeof(p->lease));
    return 1;
}
int du3_ui_progress(struct du3_connection *c, uint32_t id, uint32_t generation, uint32_t ping)
{
    if (!c || c->disconnected) { errno=EPIPE; return -1; }
    struct ciuki_message m; desk_v3_message(&m,DESK_PONG,4,0,id,generation); desk_put32(m.data+16,ping);
    return c->transport.send(c->channel,&m,DONTWAIT);
}
uint32_t du3_window_order(struct du3_connection *c, uint32_t id, uint32_t generation,
    uint32_t flags, uint32_t after, uint32_t after_generation, uint32_t owner, uint32_t owner_generation)
{
    uint8_t b[20];
    desk_put32(b,flags); desk_put32(b+4,after); desk_put32(b+8,after_generation);
    desk_put32(b+12,owner); desk_put32(b+16,owner_generation);
    return du3_request(c,DESK_V3_WINDOW_ORDER,id,generation,b,sizeof(b),-1);
}
