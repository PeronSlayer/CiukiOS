/* SYSTEM registry broker; all peer identities come from the kernel binding.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGISTRY_BROKER_H
#define CIUKI_REGISTRY_BROKER_H
#include "../../sdk/private/registry_protocol.h"
#include "../w32srv/registry.h"
struct rb_identity {
    uint32_t uid, session, groups[8], group_count, process_generation, role;
    uint32_t machine_sid[3];
    uint64_t volume, account_generation;
    char profile[64], name[32];
};
/* Trusted private boundary, same discipline as apps/accounts/adapter.h.
 * inspect revalidates the retained endpoint/process/session generations and
 * returns committed account/profile data. A copied QUERY_PEER record is not
 * a lease. A missing production binding refuses service startup. */
struct rb_authority {
    void *context;
    int (*registry_role)(void *);
    int (*retain)(void *,uintptr_t);
    void (*release)(void *,uintptr_t);
    int (*inspect)(void *,uintptr_t,struct rb_identity *);
};
struct rb_storage {
    void *context;
    /* Fixed logical root ids only; paths never arrive from an RPC. Reads
     * return malloc-owned data, writes persist atomically with existing
     * text-registry recovery. 0 or negative errno. */
    int (*read)(void *,unsigned,char **,size_t *);
    int (*write)(void *,unsigned,const char *,size_t,uint64_t);
    int (*users)(void *,const struct rb_identity *,char **,size_t *);
    int (*profile)(void *,const struct rb_identity *);
};
struct rb_link {
    uintptr_t retained;
    uint32_t id, root, total, received;
    uint64_t expected;
    uint8_t *stage, *reply;
    uint32_t reply_bytes;
    int win32;
    struct rb_identity identity;
};
struct rb_broker {
    struct rb_authority authority;
    struct rb_storage storage;
    struct wr_server *win32;
    struct wr_io raw_io;
    int active_link;
    struct rb_link links[RB_LINK_MAX];
    uint32_t high_id;
    int busy, faulted;
};
void rb_storage_posix(struct rb_storage *);
int rb_init(struct rb_broker *,const struct rb_authority *,const struct rb_storage *,struct wr_server *);
void rb_destroy(struct rb_broker *);
int rb_connect(struct rb_broker *,uintptr_t retained,unsigned *slot);
void rb_disconnect(struct rb_broker *,unsigned slot);
int rb_exchange(struct rb_broker *,unsigned slot,unsigned op,const void *,size_t,void *,size_t,size_t *);
/* Single active interactive session, as frozen by the security contract.
 * The caller provides an already broker-loaded private hive. No path or uid
 * can be chosen by request bytes. Bind only while no client links exist. */
int rb_bind_hive(struct rb_broker *,const struct rb_identity *);
#endif
