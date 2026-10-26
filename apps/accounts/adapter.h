/* SPDX-License-Identifier: MIT
 * Private SYSTEM service seams, not an SDK ABI. See README.md for binding rules.
 * k01 supplies retained subjects/lease and call-101 QUERY_SELF/QUERY_PEER;
 * k02 supplies the authenticated owner channel. No client provides these tables.
 */
#ifndef CIUKI_ACCOUNTS_ADAPTER_H
#define CIUKI_ACCOUNTS_ADAPTER_H
#include <stddef.h>
#include <stdint.h>

enum acct_security_operation {
    ACCT_SESSION_LAUNCH = 8, ACCT_LAUNCH_POLL = 9, ACCT_SECURE_ENTER = 10,
    ACCT_SECURE_LEAVE = 11, ACCT_SERVICE_CONNECT = 12,
    ACCT_SERVICE_ACCEPT = 13, ACCT_SERVICE_CANCEL = 14
};
/* Exact frozen 64-byte operation 12..14 input/output. Not public SDK types. */
struct acct_security_link {
    uint32_t version;
    uint32_t size;
    uint32_t service;
    uint32_t flags;
    int32_t endpoint_fd;
    uint32_t endpoint_generation;
    uint32_t server_role;
    uint32_t peer_pid;
    uint32_t peer_process_generation;
    uint32_t peer_role;
    uint32_t session_id;
    uint32_t service_generation;
    uint32_t request_id_lo;
    uint32_t request_id_hi;
    uint32_t reserved[2];
};

struct acct_subject {
    uintptr_t retained;
    uint64_t request_id; /* actual retained exchange, zero for internal work */
    uint32_t uid;
    uint32_t groups[8];
    uint32_t group_count;
    uint32_t session_id;
    /* Kernel/supervisor attestation, never bits in an account IPC request. */
    int safe_mode;
    uint32_t role, service, endpoint_generation, service_generation;
};
struct acct_kernel {
    void *ctx;
    int real;
    int (*lease_acquire)(void *); /* sealed SYSTEM/session 0 only; one live lease */
    void (*lease_release)(void *);
    int (*retain_peer)(void *, int endpoint, struct acct_subject *);
    int (*peer_alive)(void *, const struct acct_subject *);
    void (*release_peer)(void *, struct acct_subject *);
    int (*active_uid)(void *, uint32_t); /* 1 active/closing, 0 inactive, -errno */
    uint64_t (*monotonic_ms)(void *);
    int (*entropy)(void *, void *, size_t); /* 0 or -errno, all bytes or none */
    void *(*allocate)(void *, size_t);
    void (*deallocate)(void *, void *, size_t); /* caller wipes first */
    int (*picture_valid)(void *, uint32_t uid, const char *relative);
    /* LOGON owns challenges and consumes them once; ACCOUNT cannot mint one.
     * claim validates UI link/action/session/console and account generation.
     * outcome transmits the 240-byte body only to a retained ROLE_LOGON peer.
     */
    int (*claim_attempt)(void *, const struct acct_subject *, uint64_t attempt,
                         uint32_t action, uint32_t session, uint32_t console,
                         uint64_t accounts_generation, uint32_t resolved_uid,
                         uint32_t *intended_uid);
    int (*auth_outcome)(void *, const struct acct_subject *, uint64_t attempt,
                        int status, const void *, size_t);
    int (*service_link)(void *, unsigned operation, const struct acct_security_link *,
                        struct acct_security_link *); /* call 101, 12..14 */
    int (*exchange_begin)(void *, const struct acct_subject *, uint64_t request_id);
    /* Atomically checks the original retained link/request, queues only matching
     * reply frames, and retains its subject/charge until final drain. No I/O,
     * KDF, descriptor attachment or callback to caller-selected endpoints. */
    int (*deliver_reply)(void *, const struct acct_subject *, unsigned operation,
                          uint64_t generation, int status, const void *, size_t);
    void (*exchange_end)(void *, const struct acct_subject *);
};

enum acct_owner_op { ACCT_OWNER_PREPARE = 1, ACCT_OWNER_COMMIT,
                     ACCT_OWNER_ABORT, ACCT_OWNER_STATUS };
enum acct_owner_state { ACCT_OWNER_NONE, ACCT_OWNER_PENDING,
                        ACCT_OWNER_COMMITTED, ACCT_OWNER_ABORTED };
enum acct_owner_action { ACCT_OWNER_CREATE = 1, ACCT_OWNER_RETIRE, ACCT_OWNER_KEEP };
struct acct_owner_binding {
    uint32_t uid, action;
    uint64_t key, generation, volume, locator;
    char root[64];
};
struct acct_owner_request {
    uint32_t sid[3], count;
    uint64_t transaction;
    uint64_t expected_generation;
    uint64_t expected_accounts_generation;
    uint64_t volume;
    uint8_t target_digest[32], manifest_digest[32], digest[32];
    struct acct_owner_binding bindings[32];
};
struct acct_owner_reply {
    uint32_t sid[3], count;
    uint64_t volume, transaction;
    uint64_t generation;
    uint32_t state;
    uint8_t digest[32];
    struct { uint32_t uid, index, action; uint64_t key, generation; } results[32];
};
struct acct_owner {
    void *ctx;
    int real;
    int (*authenticate)(void *); /* retained QUERY_PEER: sealed SYSTEM service */
    int (*generation)(void *, uint64_t *);
    int (*binding)(void *, uint64_t volume, uint32_t uid, char root[64],
                   struct acct_owner_binding *); /* ENOENT if never owned; retired denies */
    /* Destination-only deny-first staging and resolver-issued locators. Flush
     * all profile/hive data and namespace metadata before returning its digest.
     * An enrollment implementation validates its protected mapping/move manifest.
     */
    int (*stage)(void *, uint64_t volume, struct acct_owner_binding *, unsigned,
                 uint8_t manifest_digest[32]);
    int (*validate_stage)(void *, const struct acct_owner_request *);
    /* PREPARE stages inaccessible defaults/resolves the root and reserves capacity.
     * CREATE refuses occupied roots (including retained G32/retired roots).
     * RETIRE preserves data and retired-id evidence. All operations are idempotent
     * by (SID,volume,G) and the full immutable vector/digest. Existing-key calls
     * retain their lifetime receipt regardless of current generations; changed
     * content returns EIO. STATUS survives service restart. The real OWNER
     * enforces the 1024-receipt and 32-profile bounds. No user chown route.
     */
    int (*call)(void *, const void *request, size_t, void *reply, size_t, size_t *);
};

/* Names are constants below the SYSTEM-only /System/Security directory.
 * write may leave a partial file on error. flush covers data; barrier covers
 * directory/volume metadata. rename MUST NOT replace an existing destination.
 * All callbacks return 0 or negative errno; read reports ENOENT, never defaults.
 */
struct acct_io {
    void *ctx;
    int real;
    int (*read)(void *, const char *, void *, size_t, size_t *);
    int (*write)(void *, const char *, const void *, size_t);
    int (*flush)(void *, const char *);
    int (*barrier)(void *);
    int (*rename)(void *, const char *, const char *);
    int (*remove)(void *, const char *);
};
#endif
