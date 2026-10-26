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

struct acct_store {
    struct acct_io io;
    struct acct_owner owner;
    struct acct_kernel kernel;
    uint64_t volume;
    const struct acct_subject *subject; /* borrowed retained worker subject, or NULL for recovery */
};
/* Store operations require the live service lease and its serialized worker. */
int acct_store_recover(struct acct_store *, struct acct_db *);
int acct_store_commit(struct acct_store *, const struct acct_db *,
                      const struct acct_db *, const struct acct_owner_request *, unsigned);
int acct_store_bootstrap(struct acct_store *, struct acct_db *);
int acct_owner_requests(struct acct_store *, const struct acct_db *,
                        struct acct_owner_request *, unsigned *);

enum acct_operation {
    ACCT_LIST = 1, ACCT_AUTH, ACCT_UNLOCK, ACCT_CREATE, ACCT_DELETE,
    ACCT_RENAME, ACCT_ENABLE, ACCT_ROLE, ACCT_PASSWORD, ACCT_PICTURE,
    ACCT_ENROLL_VERIFY
};
struct acct_request {
    uint32_t operation, uid, flags, groups;
    uint64_t generation;
    char name[32], picture[96];
    char password[64], confirm[64], old_password[64];
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
