/* SPDX-License-Identifier: MIT
 * One synchronous layout owner. All exits, including rejection, are measured.
 * f6-20: only remote content is charged against the time limit. */
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include "budget.h"
#include "logline.h"
static uint64_t deadline;            /* 0: no timed layout is active */
static enum ciuki_layout_cause last_cause;
static uint64_t last_ns;
uint64_t ciuki_layout_max_ns, ciuki_layout_local_max_ns, ciuki_layout_remote_max_ns;
unsigned ciuki_layout_rejections;
uint32_t ciuki_layout_clock_scale=1;
static uint64_t now(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return UINT64_MAX;
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
/* The scale applies to durations, never to the absolute clock: the limit in
 * host nanoseconds is LIMIT/scale and an elapsed time is charged elapsed*scale. */
static uint64_t scale(void) { return ciuki_layout_clock_scale?ciuki_layout_clock_scale:1; }
static uint64_t charged(uint64_t elapsed) { return elapsed>UINT64_MAX/scale()?UINT64_MAX:elapsed*scale(); }
/* Content NetSurf or the user's own disk supplies; http(s), data: and every
 * other scheme are remote. NetSurf refuses file/about/resource redirect
 * targets from remote fetches (llcache), so a remote page cannot become local. */
bool ciuki_layout_local_url(const char *url)
{
    static const char *const local[]={"about:","resource:","file:"};
    if(!url)return false;
    for(unsigned i=0;i<sizeof(local)/sizeof(*local);i++) {
        size_t n=0;while(local[i][n])n++;
        if(!strncasecmp(url,local[i],n))return true;
    }
    return false;
}
struct ciuki_layout_span ciuki_layout_begin(bool timed)
{
    struct ciuki_layout_span span={.start=now(),.outer_deadline=deadline,.timed=timed || deadline};
    if(timed && !deadline) {
        uint64_t limit=(CIUKI_LAYOUT_LIMIT_NS+scale()-1)/scale();
        deadline=span.start>UINT64_MAX-limit?UINT64_MAX:span.start+limit;
    }
    return span;
}
void (*ciuki_ui_pump)(void);
static unsigned keepalive_calls;
static uint64_t keepalive_last;
void ciuki_ui_keepalive(void)
{
    if(!ciuki_ui_pump || (++keepalive_calls & 7))return;
    uint64_t t=now();
    if(t!=UINT64_MAX && t-keepalive_last<20000000u)return;
    keepalive_last=t;ciuki_ui_pump();
}
bool ciuki_layout_expired(void) { ciuki_ui_keepalive();return deadline && now()>=deadline; }
bool ciuki_layout_end(struct ciuki_layout_span span,bool accepted)
{
    uint64_t finish=now(),elapsed=charged(finish>=span.start?finish-span.start:UINT64_MAX);
    if(elapsed>ciuki_layout_max_ns)ciuki_layout_max_ns=elapsed;
    uint64_t *bucket=span.timed?&ciuki_layout_remote_max_ns:&ciuki_layout_local_max_ns;
    if(elapsed>*bucket)*bucket=elapsed;
    bool late=span.timed && (elapsed>CIUKI_LAYOUT_LIMIT_NS || (deadline && finish>=deadline));
    deadline=span.outer_deadline;
    last_ns=elapsed;
    last_cause=accepted && !late?CIUKI_LAYOUT_ACCEPTED:late?CIUKI_LAYOUT_TIME:CIUKI_LAYOUT_ADMISSION;
    if(last_cause!=CIUKI_LAYOUT_ACCEPTED)ciuki_layout_rejections++;
    /* Hardware evidence: a layout that takes a large part of the remote
     * budget is reported with its class, local or remote. */
    if(elapsed>=CIUKI_LAYOUT_LIMIT_NS/2)
        ciuki_log_line("[netsurf] layout slow elapsed_us=%llu class=%s accepted=%d",
                (unsigned long long)(elapsed/1000u),span.timed?"remote":"local",last_cause==CIUKI_LAYOUT_ACCEPTED);
    return last_cause==CIUKI_LAYOUT_ACCEPTED;
}
enum ciuki_layout_cause ciuki_layout_last_cause(void) { return last_cause; }
uint64_t ciuki_layout_last_ns(void) { return last_ns; }
