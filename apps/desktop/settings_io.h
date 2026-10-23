/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SETTINGS_IO_H
#define CIUKI_SETTINGS_IO_H
#include "settings.h"
#include <pthread.h>
#define CFG_STARTUP_STAGES 10
#define CFG_LOG_MAX 1536 /* display latency plus per-frame demo diagnostics */
extern const char *const cfg_startup_stage_names[CFG_STARTUP_STAGES];
/* One bounded latest-snapshot mailbox. Disk I/O never holds lock. The caller
 * owns lifetime; stop only after busy() is false (join then cannot wait on I/O). */
struct cfg_writer {
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    int live, stopping, busy, pending, completed, error;
    int activity; /* lock-protected: 1 write, 2 read, 3 mixer job, 4 log */
    char text[CFG_TEXT_MAX]; size_t length;
    char *scratch;
    char log[CFG_LOG_MAX]; int log_pending;
    uint64_t stage_us[CFG_STARTUP_STAGES]; unsigned stage_pending, stage_cached;
    char read_text[CFG_TEXT_MAX]; size_t read_length;
    int read_pending, read_ready, read_error;
    unsigned read_serial;
    unsigned char job[64]; size_t job_length;
    int job_pending, job_ready, job_error;
    int (*run_job)(void *, size_t); /* bounded copied device setting; worker may block on its durable save */
    const char *path, *temporary;
    int (*write)(struct cfg_writer *, const char *, size_t);
    /* Optional event publication after a result is visible under lock.
     * Must not perform blocking I/O or access a UI; set before start().
     * Event consumers take the short mailbox lock without retry polling;
     * writers without a notifier preserve the legacy try-lock interface. */
    void (*notify)(void *);
    void *notify_ctx;
};
int cfg_writer_start(struct cfg_writer *, const char *path, const char *temporary);
int cfg_writer_submit(struct cfg_writer *, const char *, size_t);
/* Desktop render thread: retain its latest snapshot and retry on EAGAIN.
 * submit() keeps the existing blocking-mailbox contract for other callers. */
int cfg_writer_try_submit(struct cfg_writer *, const char *, size_t);
int cfg_writer_busy(struct cfg_writer *);
int cfg_writer_result(struct cfg_writer *, int *error);
int cfg_writer_log(struct cfg_writer *, const char *);
int cfg_writer_stage(struct cfg_writer *, unsigned stage, uint64_t us, int cached);
/* Nonblocking producers return -1/EAGAIN on contention; retry copied settings/jobs.
 * A new read invalidates both ready and in-flight older snapshots. */
int cfg_writer_read(struct cfg_writer *);
int cfg_writer_read_result(struct cfg_writer *, char *, size_t *, int *);
int cfg_writer_job(struct cfg_writer *, const void *, size_t);
int cfg_writer_job_result(struct cfg_writer *, void *, size_t *, int *);
void cfg_writer_stop(struct cfg_writer *);
/* Durable destination-aware file writer shared with offline Setup.
 * Paths may include a target volume prefix. Returns 0 or positive errno. */
int cfg_write_at(const char *path, const char *temporary, const char *, size_t);
int cfg_write_file(struct cfg_writer *, const char *, size_t);
#endif
