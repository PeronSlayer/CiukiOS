/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "sdk/netsurf/port/budget.h"
static uint64_t clock_ns=1000000000u;
int __wrap_clock_gettime(clockid_t clock,struct timespec *out)
{ assert(clock==CLOCK_MONOTONIC);out->tv_sec=clock_ns/1000000000u;out->tv_nsec=clock_ns%1000000000u;return 0; }
int main(void)
{
    /* Remote (timed) layouts: rejection timing, deadline and next-operation reset. */
    struct ciuki_layout_span span=ciuki_layout_begin(true);clock_ns+=20;
    assert(!ciuki_layout_end(span,false) && ciuki_layout_max_ns==20 && ciuki_layout_last_cause()==CIUKI_LAYOUT_ADMISSION);
    span=ciuki_layout_begin(true);clock_ns+=75000001;
    assert(ciuki_layout_expired());
    assert(!ciuki_layout_end(span,true) && ciuki_layout_max_ns==75000001 && ciuki_layout_last_cause()==CIUKI_LAYOUT_TIME);
    assert(ciuki_layout_remote_max_ns==75000001 && !ciuki_layout_local_max_ns);
    assert(!ciuki_layout_expired());
    span=ciuki_layout_begin(true);clock_ns+=10;
    assert(ciuki_layout_end(span,true) && ciuki_layout_last_cause()==CIUKI_LAYOUT_ACCEPTED);
    assert(ciuki_layout_max_ns==75000001 && ciuki_layout_rejections==2);
    /* f6-20: local (untimed) layouts never expire and are never rejected by
     * time, however long they take; structural rejection still applies. */
    span=ciuki_layout_begin(false);clock_ns+=5000000000u;
    assert(!ciuki_layout_expired());
    assert(ciuki_layout_end(span,true) && ciuki_layout_local_max_ns==5000000000u && ciuki_layout_last_ns()==5000000000u);
    assert(ciuki_layout_remote_max_ns==75000001 && ciuki_layout_rejections==2);
    span=ciuki_layout_begin(false);clock_ns+=1;
    assert(!ciuki_layout_end(span,false) && ciuki_layout_last_cause()==CIUKI_LAYOUT_ADMISSION && ciuki_layout_rejections==3);
    /* A local span nested in a remote layout (an HTML object) keeps the outer
     * deadline; the outer deadline is restored when it ends. */
    struct ciuki_layout_span outer=ciuki_layout_begin(true);clock_ns+=50000000;
    struct ciuki_layout_span inner=ciuki_layout_begin(false);clock_ns+=30000000;
    assert(ciuki_layout_expired());
    assert(!ciuki_layout_end(inner,true) && ciuki_layout_last_cause()==CIUKI_LAYOUT_TIME);
    assert(ciuki_layout_expired());
    assert(!ciuki_layout_end(outer,true) && !ciuki_layout_expired());
    /* A remote span nested in a local one gets its own deadline; the local
     * outer span returns to untimed. */
    outer=ciuki_layout_begin(false);inner=ciuki_layout_begin(true);clock_ns+=80000000;
    assert(ciuki_layout_expired() && !ciuki_layout_end(inner,true));
    assert(!ciuki_layout_expired() && ciuki_layout_end(outer,true));
    /* Host T23 model: the budget clock is multiplied, so 3 ms of host time
     * at scale 30 is 90 ms: a remote rejection, a local acceptance. */
    ciuki_layout_clock_scale=30;
    span=ciuki_layout_begin(true);clock_ns+=3000000;
    assert(!ciuki_layout_end(span,true) && ciuki_layout_last_cause()==CIUKI_LAYOUT_TIME && ciuki_layout_last_ns()==90000000u);
    span=ciuki_layout_begin(false);clock_ns+=3000000;
    assert(ciuki_layout_end(span,true) && ciuki_layout_last_ns()==90000000u);
    ciuki_layout_clock_scale=1;
    /* Classification: what NetSurf or the user's disk supplies is local. */
    assert(ciuki_layout_local_url("about:welcome") && ciuki_layout_local_url("about:query/fetcherror"));
    assert(ciuki_layout_local_url("resource:welcome.html") && ciuki_layout_local_url("file:///system/netsurf/welcome.html"));
    assert(ciuki_layout_local_url("FILE:///Documents/a.html"));
    assert(!ciuki_layout_local_url("http://example.com/") && !ciuki_layout_local_url("https://example.com/"));
    assert(!ciuki_layout_local_url("data:text/html,x") && !ciuki_layout_local_url("filex:y") && !ciuki_layout_local_url(NULL));
    assert(!ciuki_layout_local_url("about") && !ciuki_layout_local_url(""));
    puts("NetSurf layout budget: remote deadline/reset, local never time-limited, nesting, T23 clock scale and classification passed");fflush(stdout);
}
