/* SPDX-License-Identifier: MIT
 * Private SYSTEM service seams, not an SDK ABI. See README.md for binding rules.
 * k01 supplies retained subjects/lease and call-101 QUERY_SELF/QUERY_PEER;
 * k02 supplies the authenticated owner channel. No client provides these tables.
 */
#ifndef CIUKI_ACCOUNTS_ADAPTER_H
#define CIUKI_ACCOUNTS_ADAPTER_H
#include <stddef.h>
#include <stdint.h>

struct acct_subject {
    uintptr_t retained;
    uint32_t uid;
    uint32_t groups[8];
    uint32_t group_count;
    uint32_t session_id;
    /* Kernel/supervisor attestation, never bits in an account IPC request. */
    int trusted_gate;
    int safe_mode;
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
};

enum acct_owner_op { ACCT_OWNER_PREPARE = 1, ACCT_OWNER_COMMIT,
                     ACCT_OWNER_ABORT, ACCT_OWNER_STATUS };
enum acct_owner_state { ACCT_OWNER_NONE, ACCT_OWNER_PENDING,
                        ACCT_OWNER_COMMITTED, ACCT_OWNER_ABORTED };
enum acct_owner_action { ACCT_OWNER_CREATE = 1, ACCT_OWNER_RETIRE };
struct acct_owner_request {
    uint64_t transaction;
    uint64_t expected_generation;
    uint64_t volume;
    uint32_t uid;
    uint32_t action;
    char root[64];
};
struct acct_owner_reply {
    uint64_t key;
    uint64_t generation;
    uint32_t state;
};
struct acct_owner {
    void *ctx;
    int real;
    int (*authenticate)(void *); /* retained QUERY_PEER: sealed SYSTEM service */
    int (*generation)(void *, uint64_t *);
    int (*binding)(void *, uint64_t volume, uint32_t uid, char root[64],
                   struct acct_owner_reply *); /* ENOENT if never owned; retired denies */
    /* PREPARE stages inaccessible defaults/resolves the root and reserves capacity.
     * CREATE refuses occupied roots (including retained G32/retired roots).
     * RETIRE preserves data and retired-id evidence. All operations are idempotent
     * by transaction id; uid is part of its immutable content. Changed content
     * or generation is rejected. STATUS reports
     * that exact transaction even after a later transaction. No user chown route.
     */
    int (*call)(void *, unsigned op, const struct acct_owner_request *,
                struct acct_owner_reply *);
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
