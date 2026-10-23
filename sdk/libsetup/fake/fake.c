/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <string.h>
struct setup_fake setup_fake;
void setup_fake_reset(void) { memset(&setup_fake, 0, sizeof(setup_fake)); }
void setup_fake_progress(enum setup_phase p, const char *s, uint64_t done, uint64_t total, uint32_t ms)
{
    assert(setup_fake.count < SETUP_FAKE_EVENTS);
    struct setup_progress *v = &setup_fake.events[setup_fake.count++].progress;
    *v = (struct setup_progress){.phase=p, .step=s, .bytes_done=done, .bytes_total=total,
        .files_done=(uint32_t)(done/4096), .files_total=(uint32_t)(total/4096), .elapsed_ms=ms};
    if (ms) { v->throughput_kbps=(uint32_t)(done*1000/1024/ms); v->eta_ms=done ? (uint32_t)((total-done)*ms/done) : 0; }
}
void setup_fake_error(enum setup_error_class c, unsigned attempt, bool retryable)
{
    assert(setup_fake.count < SETUP_FAKE_EVENTS);
    struct setup_error *e=&setup_fake.events[setup_fake.count++].error;
    *e=(struct setup_error){.cls=c,.phase=SETUP_PHASE_COPY,.attempt=attempt,.retryable=retryable};
    strcpy(e->path,"D:\\PAYLOAD\\SYSTEM.ZIP");
}
int setup_txn_prepare(const struct setup_answers *a, struct setup_plan *p)
{
    setup_fake.prepare_calls++;
    *p=setup_fake.plan; p->version=SETUP_TXN_VERSION; p->answers=*a; p->components=a->components;
    return setup_fake.prepare_result;
}
int setup_txn_execute(const struct setup_plan *p, const struct setup_callbacks *cb, void *ctx)
{
    (void)p; setup_fake.execute_calls++;
    if (setup_fake.entered) setup_fake.entered(setup_fake.ctx);
    for (unsigned i=0;i<setup_fake.count;i++) {
        if (cb->cancel_requested && cb->cancel_requested(ctx)) {
            setup_fake.cancels++; setup_fake.last=(struct setup_error){.cls=SETUP_ERR_CANCELLED,.phase=setup_fake.phase};
            return -SETUP_ERR_CANCELLED;
        }
        const struct setup_fake_event *v=&setup_fake.events[i];
        if (v->error.cls) {
            setup_fake.last=v->error; setup_fake.phase=v->error.phase; setup_fake.errors++;
            bool retryable=v->error.retryable && (v->error.cls==SETUP_ERR_SOURCE_READ || v->error.cls==SETUP_ERR_MEDIA_REMOVED);
            if (!retryable) return -(int)v->error.cls;
            enum setup_decision d=cb->on_error ? cb->on_error(ctx,&v->error) : SETUP_DECISION_CANCEL;
            if (d==SETUP_DECISION_CANCEL || v->error.attempt>=SETUP_RETRY_MAX) return -(int)v->error.cls;
            setup_fake.retries++;
        } else {
            setup_fake.phase=v->progress.phase;
            if (setup_fake.phase>SETUP_PHASE_PLAN && setup_fake.phase<SETUP_PHASE_COMPLETE) setup_fake.mutations++;
            if (cb->log) cb->log(ctx,"fake: verified sector run");
            if (cb->progress) cb->progress(ctx,&v->progress);
        }
    }
    setup_fake.phase=SETUP_PHASE_COMPLETE; return 0;
}
int setup_txn_last_error(struct setup_error *e) { *e=setup_fake.last; return 0; }
int setup_txn_state(enum setup_phase *p) { *p=setup_fake.phase; return 0; }
