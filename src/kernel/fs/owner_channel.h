/* Kernel OWNER transport only, no user ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_OWNER_CHANNEL_H
#define CIUKI_OWNER_CHANNEL_H
#include <ciuki/security.h>
struct owner_store;
struct owner_exchange {
    struct security_peer peer;
    struct security_subject *subject;
    uint32_t header[16];
};
int channel_owner_next(struct owner_exchange *,void *,size_t,size_t *);
bool channel_owner_pending(void);
void channel_owner_reap(void);
int channel_owner_reply(struct owner_exchange *,int,const void *,size_t);
void channel_owner_done(struct owner_exchange *);
int owner_service_bind(struct owner_store *);
void owner_service_unbind(struct owner_store *);
void owner_service_poll(void);
/* sec-own: optional dispatcher replacing owner_call (owner_system.c). */
typedef int (*owner_dispatch_fn)(const struct security_subject *,const void *,size_t,void *,size_t,size_t *);
void owner_service_dispatcher(owner_dispatch_fn);
#endif
