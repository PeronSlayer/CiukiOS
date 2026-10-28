/* OWNER private service/storage interface. No SDK ABI or user setters.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_OWNER_H
#define CIUKI_FS_OWNER_H
#include "fs_port.h"
struct security_subject;
struct ciuki_security_profile;
struct fat_volume;
struct fat_entry;
/* Backing consistency evidence, used with a nonreused protected owner key.
 * Namespace replacement/rename still requires the owner transaction. */
void owner_object_proof(const struct fat_volume *, const struct fat_entry *, uint8_t proof[32]);
#define OWNER_PROFILES 32u
#define OWNER_ROOTS 256u
#define OWNER_RECEIPTS 1024u
#define OWNER_COMPACT_AT 768u
#define OWNER_RETAIN 256u
#define OWNER_REQUEST_MAX (160u+128u*32u)
#define OWNER_REPLY_MAX (96u+32u*32u)
#define OWNER_MANIFEST_MAX (64u+256u*32u)
enum owner_op { OWNER_PREPARE=1, OWNER_COMMIT, OWNER_ABORT, OWNER_STATUS,
                OWNER_CHECKPOINT, OWNER_COMPACT, OWNER_RESERVE };
enum owner_state { OWNER_UNKNOWN, OWNER_PREPARED, OWNER_COMMITTED, OWNER_ABORTED,
                   OWNER_CHECKPOINTED, OWNER_RESERVED };
enum owner_action { OWNER_CREATE=1, OWNER_RETIRE, OWNER_KEEP };
struct owner_binding {
    uint32_t uid, action;
    uint64_t key, generation, volume, locator;
    char path[64];
};
struct owner_request {
    uint32_t sid[3], count;
    uint64_t volume, transaction, accounts_generation, owner_generation;
    uint8_t accounts_digest[32], manifest_digest[32], digest[32];
    struct owner_binding bindings[OWNER_PROFILES];
};
struct owner_record {
    struct owner_binding binding;
    uint8_t proof[32];
    bool retired, pending;
};
struct owner_receipt;
struct owner_store;
/* Storage is sealed to the destination volume. No callback may trust an
 * input uid/locator instead of resolving the pinned destination itself.
 * All methods are task-context and run WITHOUT a policy/namespace mutex.
 * A service operation holds the single writer admission (EBUSY on contention).
 * I/O errors may mean durable success: the core closes until reopen/recovery.
 * File ids: 0/1 alternating snapshots, 2/3 their commit markers, 4 candidate.
 * flush(file) + barrier() includes directories and the destination volume. */
struct owner_backend {
    void *ctx;
    int (*read)(void *, unsigned file, void *, size_t cap, size_t *bytes);
    int (*write)(void *, unsigned file, const void *, size_t);
    int (*flush)(void *, unsigned file);
    int (*barrier)(void *);
    int (*manifest)(void *, uint64_t g, bool checkpoint, void *, size_t, size_t *);
    int (*tree)(void *, uint64_t g, uint32_t uid, uint8_t digest[32]);
    /* Resolve and seal the manifest/tree/root/ancestors. Recheck=true proves
     * the saved mapping on restart/COMMIT, not just the pathname's existence.
     * The core assigns locator; backend must never derive it from st_ino. */
    int (*reserve)(void *, const struct owner_request *, struct owner_record *, bool recheck);
    /* EAGAIN is a read-only validation refusal. Earlier successful renames
     * remain fenced and repeat idempotently; uncertain writes use another errno. */
    int (*publish)(void *, const struct owner_request *, struct owner_record *);
    int (*release)(void *, const struct owner_request *, bool committed);
    /* Protected account journal: 0 absent/precommit, 1 COMMIT_REQUIRED or
     * later, -errno contradictory. Compare full key/digest before returning. */
    int (*account_intent)(void *, const struct owner_request *);
    int (*account_current)(void *, uint64_t *generation, uint8_t digest[32]);
    /* Verifies exact durable ACCOUNT checkpoint marker + allocation highs. */
    int (*checkpoint_marker)(void *, const struct owner_request *, uint64_t high_g, uint32_t high_uid);
    /* Protected enrollment mapping; ordinary CREATE cannot import a root. */
    int (*import_map)(void *, uint64_t g, uint32_t old_id, uint32_t uid,
                       const char *prior, const char *destination);
    int (*occupied)(void *,const char *canonical_root); /* physical/imported roots too */
    int (*verify)(void *,const struct owner_record *); /* resolved map at mount */
};
struct owner_store {
    struct owner_backend io;
    uint32_t sid[3];
    uint64_t volume, sequence, generation, generation_high, key_high, locator_high;
    uint64_t transaction_high, floor, account_high;
    uint64_t committed_accounts_high;
    uint8_t committed_accounts_digest[32];
    uint32_t uid_high;
    struct owner_record profiles[OWNER_PROFILES], roots[OWNER_ROOTS];
    struct owner_record pending[OWNER_PROFILES];
    unsigned profile_count, root_count, receipt_count;
    struct owner_receipt *receipts[OWNER_RECEIPTS], *active;
    uint8_t checkpoint[160];
    uint64_t root_key;
    uint32_t root_operation;
    char root_old[64], root_new[64];
    bool candidate, ready, busy;
};
/* Explicit fresh provisioning is allowed only after the lead's authenticated
 * destination/seed gate. A missing store at ordinary open is EIO. */
int owner_open(struct owner_store *, const struct owner_backend *, const uint32_t sid[3], uint64_t volume, bool fresh);
/* Resume a loaded store after reversible open validation EAGAIN. The caller
 * withholds service/policy binding until this succeeds; never reload it. */
int owner_open_resume(struct owner_store *);
void owner_close(struct owner_store *);
int owner_request_decode(struct owner_request *, unsigned *op, const void *, size_t);
int owner_request_encode(const struct owner_request *, unsigned op, void *, size_t, size_t *);
int owner_manifest_validate(struct owner_store *, const struct owner_request *);
int owner_call(struct owner_store *, const struct security_subject *, const void *, size_t, void *, size_t, size_t *);
int owner_recover(struct owner_store *);
int owner_root_occupied(const struct owner_store *, uint64_t volume, const char *);
int owner_profile(const struct owner_store *, uint32_t uid, struct owner_record *);
int owner_label(const struct owner_store *, uint64_t locator, struct owner_record *);
int owner_security_bind(struct owner_store *); /* installed boot only */
/* Canonical snapshot codec: CIUKIOCP v1, or v2 with creator-root records.
 * ACCOUNT's reader needs the matching extension before v2 integration. */
int owner_checkpoint_image(struct owner_store *, uint64_t accounts_generation,
    const uint8_t accounts_digest[32], uint64_t high_g, uint32_t high_uid,
    void *, size_t, size_t *, uint64_t *floor);
/* Root-side transactions: reserve before namespace publication; publish under
 * the VFS mutation guard, then finish. An uncertain result fences the mount.
 * Retained root records remain retired; neither key nor locator is reused. */
int owner_root_reserve(struct owner_store *, const struct security_subject *, const char *name,
                        const uint8_t proof[32], struct owner_record *);
int owner_root_finish(struct owner_store *, uint64_t key, const uint8_t proof[32], bool abort);
/* change prepares rename (new_name) or delete (NULL); finish after namespace
 * durability, abort before publication only. Both names stay deny-labelled. */
int owner_root_change(struct owner_store *, const struct security_subject *, uint64_t key,
                       const char *new_name, const uint8_t proof[32]);
bool owner_profile_path(const char path[64]);
#endif
