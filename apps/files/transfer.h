/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_FILES_TRANSFER_H
#define CIUKI_FILES_TRANSFER_H
#include "model.h"
#include <pthread.h>
/* Only the worker touches fm_job while running. The mailbox lock protects
 * snapshots/commands only; it is never held across VFS calls or cleanup. */
struct transfer_view {
    enum fm_job_state state;
    enum fm_job_kind kind;
    uint64_t bytes, total, file_bytes, file_total, elapsed, rate, files, folders;
    unsigned items, count;
    int counting, prompt_dir, preparing;
    struct fm_meta meta, existing;
    char target[FM_PATH], source[FM_PATH];
};
struct files_transfer {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    struct fm_job *job;
    int (*prepare)(void *);
    void (*complete)(void *);
    void *ctx;
    struct transfer_view view;
    uint64_t samples_started; /* worker-owned, after preflight */
    uint64_t started_at, progress_logged; /* UI-owned; advance during blocked I/O */
    int pending, stopping, cancel, answer, finished;
};

uint64_t job_now(void);
void job_record(const char *, ...);
const char *fm_job_verb(enum fm_job_kind);
int fm_transfer_create(struct files_transfer **, struct fm_job *);
/* Reset an idle mailbox, show its initial view, then submit ownership. */
void fm_transfer_reset(struct files_transfer *);
void fm_transfer_submit(struct files_transfer *);
void fm_transfer_command(struct files_transfer *, int cancel, int answer);
void fm_transfer_destroy(struct files_transfer *);
#endif
