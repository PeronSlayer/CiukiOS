/* Owned schema-2 restart queues. Private SYSTEM broker interface.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGISTRY_RESTART_H
#define CIUKI_REGISTRY_RESTART_H
#include "broker.h"
#define RJ_JOBS 64u
#define RJ_RECORD 1024u
#define RJ_HEADER 128u
#define RJ_BYTES (RJ_HEADER+RJ_JOBS*RJ_RECORD)
enum { RJ_PERSONAL=1,RJ_MACHINE=2 };
enum { RJ_FILE=1,RJ_PROGRAM=2 };
enum { RJ_PREPARED=1,RJ_APPLY_REQUIRED,RJ_APPLIED,RJ_HANDED,RJ_COMPLETE,RJ_CANCELLED };
struct rj_spec {
    unsigned kind,mask,phase;
    uint64_t registry_generation;
    uint8_t registry_identity[32];
    const char *source,*target,*executable,*arguments,*cwd;
};
struct rj_backend {
    void *context;
    /* Read/write fixed broker-owned scope/uid journals and intents/backups.
     * write acknowledges only after recoverable replacement and metadata flush.
     * Callbacks return 0 or -errno; read returns -ENOENT only if truly absent. */
    int (*read)(void *,unsigned,uint32_t,uint8_t **,size_t *);
    int (*write)(void *,unsigned,uint32_t,const void *,size_t);
    int (*account)(void *,uint32_t,struct rb_identity *,int *enabled);
    /* resolve/admit/revalidate checks paths, mount/object proofs, executable
     * hash and the exact registry entry/generation. No devices, sealed files,
     * security stores or foreign profiles. Writes only stable proof/hash bytes
     * (wire offsets 112..191); phase/state/strings cannot be rewritten. */
    int (*authorize)(void *,uintptr_t,const struct rb_identity *,unsigned,uint8_t *,int replay);
    /* File mutation is idempotent against stored identities and operation
     * receipt. It never resolves replacement pathname objects. */
    int (*apply)(void *,uintptr_t,const uint8_t *);
    /* Current user's credential only, never the broker's SYSTEM credential.
     * HANDED is durable before this callback. Failure is uncertain and held. */
    int (*launch)(void *,uintptr_t,const uint8_t *);
    /* Idempotently preserve original v1/WININIT/.DEFAULT bytes in protected
     * evidence and durably mark consumed. No next-user assignment or replay. */
    int (*quarantine)(void *);
};
struct rj_queue {
    struct rb_authority authority;
    struct rj_backend backend;
    uint8_t *wire;
    int faulted,busy;
};
int rj_open(struct rj_queue *,const struct rb_authority *,const struct rj_backend *,
            unsigned scope,uint32_t uid,const uint32_t sid[3],uint64_t volume);
void rj_close(struct rj_queue *);
int rj_admit(struct rj_queue *,uintptr_t,const struct rj_spec *,uint64_t *id);
int rj_replay(struct rj_queue *,uintptr_t,uint64_t id);
int rj_reconcile(struct rj_queue *,uintptr_t,uint64_t id,int completed);
int rj_cancel_deleted(struct rj_queue *); /* broker maintenance, retains retired evidence */
int rj_reapprove(struct rj_queue *,uintptr_t,uint64_t old_id,uint64_t *new_id);
int rj_quarantine(struct rj_queue *);
#endif
