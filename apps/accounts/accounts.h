/* SPDX-License-Identifier: MIT
 * Private account service API. No runtime endpoint is installed until real
 * sealed kernel/owner adapters are bound. Not a public ABI or logon UI.
 */
#ifndef CIUKI_ACCOUNTS_H
#define CIUKI_ACCOUNTS_H
#include "adapter.h"
#include <stdatomic.h>

#define ACCT_MAX 18u
#define ACCT_FILE_MAX (96u + ACCT_MAX * 256u)
#define ACCT_ITERATIONS 10000u
#define ACCT_ENABLED 1u
#define ACCT_BUILTIN 2u
#define ACCT_PASSWORD_SET 4u
/* AUTH LIST hint only; authentication remains authoritative. */
#define ACCT_PUBLIC_BLANK_LOGON 4u
#define ACCT_READY_BLANK 8u
#define ACCT_CHANGE_REQUIRED 16u
#define ACCT_ADMIN 3u
#define ACCT_STANDARD 2u
#define ACCT_GUEST 4u
#define ACCT_MIGRATED 1u
#define ACCT_EXHAUSTED 2u
#define ACCT_AUTO_RETIRED 4u
#define ACCT_READY 1u
#define ACCT_SETUP 2u
#define ACCT_UPGRADE 3u
#define ACCT_LAST_UID UINT32_C(0xfffffffe)

struct acct_record {
    uint32_t uid, flags, groups, kdf, iterations, legacy_id;
    char name[32];
    uint8_t salt[16], verifier[32];
    char picture[96];
};
struct acct_db {
    uint64_t generation;
    uint32_t next_uid, count, flags, sid[3], origin, auto_logon_uid;
    struct acct_record records[ACCT_MAX];
};
void acct_wipe(void *, size_t);
/* Zeroed heap frame for records too large for a 4 KiB target stack frame;
 * free wipes before returning the memory. */
void *acct_frame_alloc(const struct acct_kernel *, size_t);
void acct_frame_free(const struct acct_kernel *, void *, size_t);
int acct_equal_secret(const void *, const void *, size_t);
int acct_password_valid(const char *, int allow_short);
int acct_name_valid(const char *);
int acct_name_equal(const char *, const char *);
int acct_reserved_name(const char *);
int acct_picture_syntax(const char *);
int acct_profile_root_valid(const char[64]);
int acct_derive(const char *, const uint8_t[16], uint32_t, uint8_t[32]);
int acct_validate(const struct acct_db *, int seed);
int acct_encode(const struct acct_db *, int seed, void *, size_t, size_t *);
int acct_decode(struct acct_db *, int seed, const void *, size_t);

#define ACCT_OWNER_BODY_MAX (160u + 128u * 32u)
#define ACCT_OWNER_REPLY_MAX (96u + 32u * 32u)
struct acct_store;
struct acct_service;
struct acct_job;
struct acct_request;
struct acct_response;
int acct_owner_encode(const struct acct_owner_request *, unsigned, void *, size_t, size_t *);
int acct_owner_decode(struct acct_owner_request *, unsigned *, const void *, size_t);
int acct_owner_reply_encode(const struct acct_owner_reply *, void *, size_t, size_t *);
int acct_owner_reply_decode(struct acct_owner_reply *, const struct acct_owner_request *, const void *, size_t);
int acct_owner_call(struct acct_store *, unsigned, const struct acct_owner_request *, struct acct_owner_reply *);

enum acct_boot_state { ACCT_UNPROVISIONED = 1, ACCT_SID_READY, ACCT_OWNERS_READY,
                       ACCT_ACCOUNTS_READY, ACCT_CONSUMED };
struct acct_bootstrap {
    uint32_t state, origin, sid[3], next_uid, flags;
    uint64_t volume, transaction, accounts_generation, owner_generation, generation;
    uint8_t seed_digest[32];
};
int acct_boot_encode(const struct acct_bootstrap *, void *, size_t);
int acct_boot_decode(struct acct_bootstrap *, const void *, size_t);
int acct_boot_read(struct acct_store *, struct acct_bootstrap *);
int acct_boot_write(struct acct_store *, struct acct_bootstrap *);

/* Private first-boot diagnostics; neither a disk record nor an SDK ABI. */
enum acct_boot_step {
    ACCT_BOOT_RECOVER, ACCT_BOOT_PROMOTE, ACCT_BOOT_IDENTITY,
    ACCT_BOOT_ENTROPY_SID, ACCT_BOOT_ENTROPY_SALT, ACCT_BOOT_GENERATION,
    ACCT_BOOT_BINDING, ACCT_BOOT_STAGE, ACCT_BOOT_TREE_DIGEST,
    ACCT_BOOT_VALIDATE_STAGE, ACCT_BOOT_ORPHANS, ACCT_BOOT_RESERVE,
    ACCT_BOOT_STATUS, ACCT_BOOT_PREPARE, ACCT_BOOT_COMMIT, ACCT_BOOT_ABORT,
    ACCT_BOOT_CHECKPOINT, ACCT_BOOT_COMPACT, ACCT_BOOT_INTENT, ACCT_BOOT_STEP_COUNT
};
struct acct_store {
    struct acct_io io;
    struct acct_owner owner;
    struct acct_kernel kernel;
    uint64_t volume;
    uint64_t high_generation;
    uint32_t high_uid;
    enum acct_boot_step firstboot_step;
    const struct acct_subject *subject; /* borrowed retained worker subject, or NULL for recovery */
};
static inline int acct_boot_result(struct acct_store *s, enum acct_boot_step step, int e)
{ if (e) s->firstboot_step=step; return e; }
/* Store operations require the live service lease and its serialized worker. */
int acct_store_recover(struct acct_store *, struct acct_db *);
int acct_store_commit(struct acct_store *, const struct acct_db *,
                      const struct acct_db *, const struct acct_owner_request *, unsigned);
int acct_store_load_watermarks(struct acct_store *, const uint32_t sid[3]);
int acct_store_reserve_generation(struct acct_store *, const uint32_t sid[3], uint64_t, uint32_t);
int acct_store_local_recover(struct acct_store *);
int acct_store_local_commit(struct acct_store *, const struct acct_db *, const struct acct_db *);
int acct_blob_read(struct acct_store *, const char *, void *, size_t, size_t *, uint64_t *);
int acct_blob_write(struct acct_store *, const char *, const void *, size_t, uint64_t *);
int acct_results_open(struct acct_service *);
int acct_results_execute(struct acct_service *, struct acct_job *, struct acct_response *);
int acct_results_cancel(struct acct_service *, const struct acct_job *);
int acct_results_admit(struct acct_service *, struct acct_job *, struct acct_response *, int *cached);
int acct_results_finish(struct acct_service *, const struct acct_request *, int);
int acct_results_replan(struct acct_service *, const struct acct_request *);
int acct_management_digest(const struct acct_request *, uint8_t[32]);
#define ACCT_MANIFEST_MAX (64u + 32u * 256u)
#define ACCT_CHECKPOINT_MAX (160u + 32u * 112u + 256u * (8u + ACCT_OWNER_BODY_MAX + ACCT_OWNER_REPLY_MAX))
int acct_manifest_validate(const struct acct_owner_request *, const void *, size_t);
int acct_tree_digest(const void *, size_t, uint8_t[32]);
int acct_owner_checkpoint(struct acct_store *, const struct acct_db *, int force);
int acct_owner_checkpoint_recover(struct acct_store *, const struct acct_db *);
int acct_checkpoint_validate(const struct acct_store *, const struct acct_db *, const void *, size_t, uint64_t);
int acct_store_bootstrap(struct acct_store *, struct acct_db *);
int acct_boot_identity(struct acct_store *, struct acct_db *, int create);
int acct_owner_requests(struct acct_store *, const struct acct_db *,
                        struct acct_owner_request *, unsigned *);

enum acct_operation {
    ACCT_LIST = 1, ACCT_AUTH, ACCT_UNLOCK, ACCT_CREATE, ACCT_DELETE,
    ACCT_RENAME, ACCT_ENABLE, ACCT_ROLE, ACCT_PASSWORD, ACCT_PICTURE,
    ACCT_ENROLL_VERIFY, ACCT_UPDATE, ACCT_LOOKUP, ACCT_CHALLENGE,
    ACCT_AUTOLOGON, ACCT_ENROLL_PLAN, ACCT_STATUS, ACCT_RESERVE, ACCT_PROFILE,
    ACCT_ENROLL_SOURCE /* sec-user-04: AUTH op 8, LOGON only (contract amendment) */
};
struct acct_challenge {
    uint32_t action, uid, session, console, authentication_epoch;
    uint32_t ui_generation, logon_generation, account_generation, flags;
    uint64_t attempt, accounts_generation;
};
struct acct_begin {
    uint32_t action, session, console, selector, uid;
    char name[32];
};
struct acct_request {
    uint32_t operation, uid, flags, groups;
    uint64_t generation;
    char name[32], picture[96];
    char password[64], confirm[64], old_password[64];
    struct acct_challenge challenge;
    struct acct_begin begin;
    uint64_t client_operation_id;
    uint8_t request_digest[32];
    uint64_t attempt;
    uint64_t request_id;
    uint32_t plan_count, first_old_id;
    struct { uint32_t old_id; char name[32]; } plan[16];
    uint32_t session, console, selectors, service, self_password, wire_operation;
};
struct acct_public {
    uint32_t uid, flags, groups;
    char name[32], picture[96];
};
struct acct_response {
    uint64_t ticket, generation;
    int error;
    uint32_t retry_after_ms, authenticated_uid, change_required, count;
    struct acct_public records[ACCT_MAX];
    size_t body_bytes;
    uint8_t body[272]; /* challenge, STATUS, PROFILE, RESERVE or progress */
};
struct acct_job {
    uint64_t ticket;
    struct acct_subject subject;
    struct acct_request request;
    int occupied;
};
#define ACCT_RECEIPTS 1024u
#define ACCT_RECEIPT_RESERVED 1u
#define ACCT_RECEIPT_PENDING 2u
#define ACCT_RECEIPT_COMMITTED 3u
#define ACCT_RECEIPT_ABORTED 4u
#define ACCT_RECEIPT_EXPIRED 5u
struct acct_receipt {
    uint64_t id, generation, planned_generation;
    uint32_t state, operation, requester, target;
    int32_t status;
    uint8_t digest[32];
};
struct acct_results {
    uint64_t high, floor, sequence;
    unsigned count;
    struct acct_receipt receipts[ACCT_RECEIPTS];
};
struct acct_attempt {
    struct acct_challenge challenge;
    struct acct_begin begin;
    struct acct_subject logon;
    uint64_t started;
    int occupied;
};
struct acct_enrollment {
    uint64_t generation;
    uint8_t plan_digest[32];
    uint32_t staged, map_count;
    struct acct_db draft;
    struct { uint32_t old_id, uid; char root[64]; } map[16];
};
struct acct_service {
    struct acct_store store;
    struct acct_db db;
    atomic_flag queue_lock, worker_lock;
    struct acct_job active, pending;
    struct acct_response completion;
    uint64_t next_ticket, retry_at, high_attempt;
    struct acct_results *results;
    struct acct_attempt attempts[8];
    struct acct_enrollment enrollment;
    int autologon_used, faulted;
    int leased, completed;
    atomic_int available;
};
/* host_test=0 demands real kernel, owner AND protected-storage bindings.
 * Caller must not close while a worker is running. No pthread/UI dependencies.
 */
int acct_service_open(struct acct_service *, const struct acct_store *, int host_test);
/* Retry unpublished startup under the same lease, before admitting clients.
 * Durable intents/orphans use exactly the open/reboot recovery rules. */
int acct_service_resume(struct acct_service *);
void acct_service_close(struct acct_service *);
/* Called only by sealed provisioning/enrollment worker before admitting clients. */
int acct_service_bootstrap(struct acct_service *, const void *seed, size_t, uint32_t expected_origin);
/* submit copies a fixed private IPC payload and clears its secret fields even
 * on failure. endpoint is the receiver's retained kernel link, not a sender PID.
 * One active worker, at most one pending trusted-gate authentication request.
 */
int acct_submit(struct acct_service *, int endpoint, struct acct_request *, size_t,
                struct acct_response *);
int acct_work(struct acct_service *, struct acct_response *);

/* Kernel role/service values from the frozen call-101 contract. These are
 * private adapter constants, never an alternate SDK or registration route. */
#define ACCT_ROLE_LOGON 1u
#define ACCT_ROLE_UI 2u
#define ACCT_ROLE_ACCOUNT 3u
#define ACCT_ROLE_OWNER 4u
#define ACCT_ROLE_REGISTRY 5u
#define ACCT_ROLE_DESKTOP 6u
#define ACCT_ROLE_W32SRV 7u
#define ACCT_ENDPOINT_AUTH 1u
#define ACCT_ENDPOINT_ACCOUNTS 2u
#define ACCT_ENDPOINT_LOGON 5u
int acct_body_decode(unsigned service, unsigned operation, const void *, size_t,
                     uint64_t frame_generation, struct acct_request *);
int acct_public_encode(const struct acct_response *, void *, size_t, size_t *);
int acct_reply_body(unsigned service, unsigned operation, const struct acct_response *, void *, size_t, size_t *);
int acct_reply_frame(const struct acct_subject *, unsigned operation, uint64_t request_id,
                      uint64_t generation, int status, const void *, size_t total, size_t offset,
                      void *frame, size_t *frame_bytes);
struct acct_ui_event { uint32_t action, session, console; uint64_t attempt; };
int acct_event_encode(unsigned operation, int challenge_reply, const struct acct_ui_event *, void *, size_t);
int acct_event_decode(unsigned operation, int challenge_reply, struct acct_ui_event *, const void *, size_t);
struct acct_auth_outcome {
    struct acct_challenge challenge;
    int32_t status;
    uint32_t disposition, retry_after_ms;
    uint64_t resulting_generation;
    uint8_t credential[64], profile[112];
    char name[32];
};
int acct_begin_encode(const struct acct_begin *, int internal, void *, size_t);
int acct_begin_decode(struct acct_begin *, int internal, const void *, size_t);
int acct_challenge_encode(const struct acct_challenge *, int proposal, void *, size_t);
int acct_challenge_decode(struct acct_challenge *, int proposal, const void *, size_t);
int acct_auth_execute(struct acct_service *, struct acct_job *, struct acct_response *);
void acct_attempts_clear(struct acct_service *);
int acct_enrollment_recover(struct acct_service *);
int acct_enrollment_source(struct acct_service *, const void *, size_t);
int acct_enrollment_execute(struct acct_service *, struct acct_job *, struct acct_response *);
/* sec-user-04: AUTH ENROLL_SOURCE reply (32-byte header + 48-byte rows, the
 * layout of apps/logon/logon_proto.h lgn_source) from the response that
 * acct_enrollment_execute filled: count rows in records[] (uid = old id,
 * flags = row flags, groups = staged uid, name), generation = plan
 * generation, change_required = header flags, authenticated_uid = staged
 * mask. Names and ids only. */
int acct_source_encode(const struct acct_response *, void *, size_t, size_t *);
int acct_auth_result(struct acct_service *, struct acct_job *, struct acct_response *, int,
                     unsigned disposition, const struct acct_record *);
int acct_verify(const struct acct_record *, const char *);
int acct_set_password(struct acct_service *, struct acct_record *, const char *);
struct acct_logon_attempt {
    struct acct_subject ui;
    struct acct_begin begin;
    struct acct_challenge challenge;
    uint64_t started;
    int occupied, resolved, submitted;
};
struct acct_logon {
    struct acct_kernel kernel;
    struct acct_logon_attempt attempts[8];
    uint32_t next_attempt, attempt_generation;
    int autologon_used;
};
int acct_logon_begin(struct acct_logon *, const struct acct_subject *, const void *, size_t, void *relay128);
int acct_logon_autologon(struct acct_logon *, int safe, int logged_off_or_locked, void *relay128);
int acct_logon_challenged(struct acct_logon *, const struct acct_subject *account, const void *, size_t);
int acct_logon_submit(struct acct_logon *, const struct acct_subject *ui, const void *, size_t, void *relay272);
int acct_logon_outcome(struct acct_logon *, const struct acct_subject *account, uint64_t current_accounts_generation,
                       const void *, size_t, struct acct_auth_outcome *, void *progress16);
void acct_logon_clear(struct acct_logon *);
int acct_logon_session_lock(const struct acct_kernel *, const struct acct_subject *, const void *, size_t, void *progress);
int acct_outcome_decode(struct acct_auth_outcome *, int frame_status, const void *, size_t);
int acct_submit_body(struct acct_service *, int endpoint, unsigned service, unsigned operation,
                     void *, size_t, uint64_t frame_generation, uint64_t request_id, struct acct_response *);
struct acct_exchange {
    struct acct_subject subject;
    uint8_t *body;
    uint64_t request_id, generation, started;
    uint32_t service, operation, total, used;
    int endpoint, occupied, transferred;
};
struct acct_transport { struct acct_exchange exchanges[8]; };
int acct_frame(struct acct_transport *, struct acct_service *, int endpoint,
               const void *, size_t, struct acct_response *);
int acct_transport_cancel(struct acct_transport *, struct acct_service *, int endpoint);
void acct_transport_drop(struct acct_transport *, struct acct_service *, int endpoint);
int acct_autologon_eligible(const struct acct_db *, const struct acct_bootstrap *,
                           int safe_mode, int logged_off, int locked);

/* Read-only G32 conversion input; enrollment/relocation and evidence moves belong
 * to sec-user-02. Never calls users_execute or changes desktop logon behavior.
 */
struct acct_import_options {
    uint32_t first_legacy_id;
    char replacement_names[16][32]; /* by sorted old-id order; empty = old name */
};
struct acct_import_map {
    uint32_t legacy_id, uid;
    char root[64];
};
int acct_import_g32(const void *, size_t, const struct acct_import_options *,
                    struct acct_db *, struct acct_import_map[16], unsigned *);
#endif
