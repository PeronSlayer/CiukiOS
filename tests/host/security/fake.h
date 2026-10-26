/* SPDX-License-Identifier: MIT */
#ifndef ACCT_TEST_FAKE_H
#define ACCT_TEST_FAKE_H
#include "accounts.h"
#include <pthread.h>

#define FAKE_FILES 48
#define FAKE_PEERS 12
#define FAKE_FILE_BYTES (2u * 1024u * 1024u)
#define FAKE_OWNERS 1024
struct fake_file {
    char name[128], durable_name[128];
    size_t size, durable_size;
    unsigned char *data, *durable_data;
    size_t capacity, durable_capacity;
};
struct fake_owner_entry {
    struct acct_owner_request request;
    struct acct_owner_reply reply;
};
struct fake_peer {
    struct acct_subject subject;
    unsigned epoch, refs;
    uint64_t last_request, active_request, cancelled_request, queued_request;
    int alive;
};
struct fake {
    struct fake_file files[FAKE_FILES];
    struct fake_owner_entry *owners;
    struct acct_owner_binding profiles[32];
    struct acct_import_map enrollment[16];
    unsigned enrollment_count;
    struct { uint32_t uid; char from[64], to[64]; uint64_t before, after; } move;
    unsigned profile_count;
    uint64_t owner_high, key_high, locator_high, owner_floor;
    struct acct_owner_request checkpoint_candidate;
    uint64_t checkpoint_floor_candidate;
    uint8_t checkpoint_digest[32];
    uint32_t logon_generation, account_generation, authentication_epoch;
    unsigned reserve_calls;
    int replace_root, change_tree, corrupt_manifest, deny_enrollment, locked, reserve_create;
    unsigned char manifest[ACCT_MANIFEST_MAX];
    size_t manifest_bytes;
    uint64_t attempt, consumed_attempt;
    uint32_t console_generation, intended_uid, locked_session;
    uint32_t attempt_endpoint_generation;
    uint64_t attempt_accounts_generation;
    unsigned outcomes;
    uint8_t last_outcome[304];
    int fail_outcome, revoke_after_commit;
    struct fake_peer peers[FAKE_PEERS];
    uint64_t owner_generation, now;
    unsigned allocations, fail_alloc, outstanding, entropy_calls, fail_entropy;
    unsigned event, cut, failed, owner_used, sid_nonce;
    unsigned prepare_calls, commit_calls, abort_calls;
    uint32_t active_uid;
    int leased, deny_lease, deny_owner, owner_full, wrong_owner, deallocated_dirty;
    int cancel_on_prepare;
    int write_error;
    char write_error_name[32];
    pthread_mutex_t peer_lock;
};
void fake_init(struct fake *);
void fake_destroy(struct fake *);
struct acct_store fake_store(struct fake *);
void fake_powercut(struct fake *);
void fake_put(struct fake *, const char *, const void *, size_t);
int fake_file_index(struct fake *, const char *);
void fake_seed(struct acct_db *, int ready);
void fake_provision(struct fake *, int ready);
void fake_boot(struct fake *, struct acct_service *);
void fake_request(struct acct_request *, unsigned, uint64_t, uint32_t, const char *, const char *);
int fake_challenge(struct acct_service *, int, struct acct_request *);
int fake_reserve(struct acct_service *, int, struct acct_request *);
int fake_run(struct acct_service *, int endpoint, struct acct_request *, struct acct_response *);
#endif
