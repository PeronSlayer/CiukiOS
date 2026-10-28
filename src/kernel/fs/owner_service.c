/* One bounded OWNER exchange per PID 1 maintenance turn.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_channel.h"
#include "owner_internal.h"
static struct owner_store *service_store;
static bool polling;
/* sec-own: the system-volume dispatcher (owner_system.c) serves the query
 * operations 8..14 and the gated first RESERVE before any store is open. */
static owner_dispatch_fn service_dispatch;
void owner_service_dispatcher(owner_dispatch_fn fn) { service_dispatch=fn; }
/* end sec-own */
int owner_service_bind(struct owner_store *s)
{
    if(!s || !s->ready) return -EIO;
    if(service_store && service_store!=s) return -EBUSY;
    int e=owner_security_bind(s); if(!e) service_store=s; return e;
}
void owner_service_unbind(struct owner_store *s)
{ if(service_store==s) service_store=0; }
void owner_service_poll(void)
{
    channel_owner_reap();
    /* sec-own: a registered dispatcher also answers while no store is open. */
    bool serving=service_dispatch || (service_store && service_store->ready);
    if(!serving || (service_store && service_store->busy) || polling || !channel_owner_pending()) return;
    polling=true;
    size_t allocation=CIUKI_SECURITY_SERVICE_BODY_MAX+OWNER_REPLY_MAX;
    uint8_t *buffers=oalloc(allocation); if(!buffers) { polling=false; return; }
    struct owner_exchange exchange; size_t request_bytes=0,reply_bytes=0;
    int received=channel_owner_next(&exchange,buffers,CIUKI_SECURITY_SERVICE_BODY_MAX,&request_bytes);
    if(received==1) {
        int e=request_bytes<12 || fs_rd32(buffers+8)!=exchange.header[4]?-EINVAL:
            service_dispatch?service_dispatch(exchange.subject,buffers,request_bytes,buffers+CIUKI_SECURITY_SERVICE_BODY_MAX,OWNER_REPLY_MAX,&reply_bytes):
            owner_call(service_store,exchange.subject,buffers,request_bytes,buffers+CIUKI_SECURITY_SERVICE_BODY_MAX,OWNER_REPLY_MAX,&reply_bytes);
        /* After admission, a disappeared link suppresses the reply only. The
         * durable OWNER STATUS resolves the result after reconnect. */
        channel_owner_reply(&exchange,e,buffers+CIUKI_SECURITY_SERVICE_BODY_MAX,e?0:reply_bytes);
        channel_owner_done(&exchange);
    }
    ofree(buffers,allocation);
    polling=false;
}
