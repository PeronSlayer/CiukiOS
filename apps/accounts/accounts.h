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

struct acct_store {
    struct acct_io io;
    struct acct_owner owner;
    struct acct_kernel kernel;
    uint64_t volume;
    uint64_t high_generation;
    uint32_t high_uid;
    const struct acct_subject *subject; /* borrowed retained worker subject, or NULL for recovery */
};
/* Store operations require the live service lease and its serialized worker. */
int acct_store_recover(struct acct_store *, struct acct_db *);
int acct_store_commit(struct acct_store *, const struct acct_db *,
                      const struct acct_db *, const struct acct_owner_request *, unsigned);
int acct_store_bootstrap(struct acct_store *, struct acct_db *);
int acct_boot_identity(struct acct_store *, struct acct_db *, int create);
int acct_owner_requests(struct acct_store *, const struct acct_db *,
                        struct acct_owner_request *, unsigned *);

enum acct_operation {
    ACCT_LIST = 1, ACCT_AUTH, ACCT_UNLOCK, ACCT_CREATE, ACCT_DELETE,
    ACCT_RENAME, ACCT_ENABLE, ACCT_ROLE, ACCT_PASSWORD, ACCT_PICTURE,
    ACCT_ENROLL_VERIFY, ACCT_REPLACE, ACCT_UPDATE, ACCT_LOOKUP
};
struct acct_request {
    uint32_t operation, uid, flags, groups;
    uint64_t generation;
    char name[32], picture[96];
    char password[64], confirm[64], old_password[64];
    uint64_t attempt;
    uint64_t request_id;
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
};
struct acct_job {
    uint64_t ticket;
    struct acct_subject subject;
    struct acct_request request;
    int occupied;
};
struct acct_service {
    struct acct_store store;
    struct acct_db db;
    atomic_flag queue_lock, worker_lock;
    struct acct_job active, pending;
    struct acct_response completion;
    uint64_t next_ticket, retry_at;
    int leased, completed;
    atomic_int available;
};
/* host_test=0 demands real kernel, owner AND protected-storage bindings.
 * Caller must not close while a worker is running. No pthread/UI dependencies.
 */
int acct_service_open(struct acct_service *, const struct acct_store *, int host_test);
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
    uint32_t action, endpoint_generation, console_generation;
    uint64_t attempt, accounts_generation;
    uint8_t credential[64], profile[112];
    char name[32];
};
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
