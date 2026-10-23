/* SPDX-License-Identifier: MIT -- host tests only; never link into Setup. */
#ifndef SETUP_FAKE_H
#define SETUP_FAKE_H
#include "setup_txn.h"
#define SETUP_FAKE_EVENTS 32
struct setup_fake_event { struct setup_progress progress; struct setup_error error; };
struct setup_fake {
    struct setup_plan plan;
    struct setup_fake_event events[SETUP_FAKE_EVENTS];
    unsigned count, prepare_calls, execute_calls, errors, retries, cancels, mutations;
    int prepare_result;
    enum setup_phase phase;
    struct setup_error last;
    /* Called at execution entry; tests assert the application is on Copy,
     * and that the preceding page was the prepared summary. */
    void (*entered)(void *ctx); void *ctx;
};
extern struct setup_fake setup_fake;
void setup_fake_reset(void);
void setup_fake_progress(enum setup_phase, const char *step, uint64_t done, uint64_t total, uint32_t elapsed_ms);
void setup_fake_error(enum setup_error_class, unsigned attempt, bool retryable);
#endif
