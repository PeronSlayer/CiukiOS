/* SPDX-License-Identifier: MIT
 * Setup CD: the shared installation transaction, progress and error
 * interface, frozen in docs/design/cd-setup-plan.md §6.4 (decision D14).
 * f9-04 implements it (sdk/libsetup); f9-03 (apps/setup) consumes it and
 * tests against the scripted fake in sdk/libsetup/fake. A change needs the
 * lead's amendment to the plan first. struct setup_answers is defined by the
 * answer-file interface (setup_inf.h, owned by f9-03). */
#ifndef CIUKI_SETUP_TXN_H
#define CIUKI_SETUP_TXN_H
#include <stdbool.h>
#include <stdint.h>
#include "setup_inf.h"

/* on_error is called at most SETUP_RETRY_MAX times for the same operation. */
#define SETUP_RETRY_MAX 5

enum setup_phase { SETUP_PHASE_PLAN, SETUP_PHASE_PARTITION, SETUP_PHASE_FORMAT,
    SETUP_PHASE_COPY, SETUP_PHASE_CONFIGURE, SETUP_PHASE_PUBLISH, SETUP_PHASE_COMPLETE };
enum setup_error_class { SETUP_ERR_NONE, SETUP_ERR_SOURCE_READ /* retryable */,
    SETUP_ERR_SOURCE_MISMATCH, SETUP_ERR_MEDIA_REMOVED /* retryable after reinsert */,
    SETUP_ERR_DEST_WRITE, SETUP_ERR_DEST_FLUSH, SETUP_ERR_DEST_FULL, SETUP_ERR_DEST_CHANGED,
    SETUP_ERR_LEASE, SETUP_ERR_CONSISTENCY, SETUP_ERR_CANCELLED, SETUP_ERR_MEMORY, SETUP_ERR_INTERNAL };
struct setup_disk_identity { char model[41], serial[21]; uint64_t sectors; uint8_t table_fingerprint[32]; };
struct setup_plan {            /* produced by setup_txn_prepare(), shown on the summary page */
    struct setup_disk_identity disk; unsigned erased_partitions; struct { uint8_t type; uint64_t start, count; char label[12]; } erased[16];
    uint64_t partition_start, partition_count; uint64_t required_bytes, overhead_bytes, reserve_bytes, free_after_bytes;
    uint32_t components; /* bitmask over the catalogue */ struct setup_answers answers; char source_image_sha[65], source_iso_sha[65];
};
struct setup_progress { enum setup_phase phase; const char *step; uint64_t bytes_done, bytes_total; uint32_t files_done, files_total; uint32_t elapsed_ms, eta_ms, throughput_kbps; };
struct setup_error { enum setup_error_class cls; int err; enum setup_phase phase; char path[260]; uint64_t lba, count; unsigned attempt; bool retryable; };
enum setup_decision { SETUP_DECISION_RETRY, SETUP_DECISION_CANCEL };
struct setup_callbacks {
    void (*progress)(void *ctx, const struct setup_progress *);
    enum setup_decision (*on_error)(void *ctx, const struct setup_error *); /* retryable errors only; bounded by libsetup */
    bool (*cancel_requested)(void *ctx);
    void (*log)(void *ctx, const char *line);                               /* the SETUP.LOG ring */
};
int setup_txn_prepare(const struct setup_answers *, struct setup_plan *out);       /* no writes; dry run through libdisk */
int setup_txn_execute(const struct setup_plan *, const struct setup_callbacks *, void *ctx); /* P1..P5; returns 0 or -(setup_error_class) */
int setup_txn_last_error(struct setup_error *out);
int setup_txn_state(enum setup_phase *phase);                                      /* after a failure or cancel: the documented state */
#endif
