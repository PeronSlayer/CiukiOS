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
    uint64_t start=ciuki_layout_begin();clock_ns+=20;
    assert(!ciuki_layout_end(start,false) && ciuki_layout_max_ns==20);
    start=ciuki_layout_begin();clock_ns+=75000001;
    assert(ciuki_layout_expired());
    assert(!ciuki_layout_end(start,true) && ciuki_layout_max_ns==75000001);
    assert(!ciuki_layout_expired());
    start=ciuki_layout_begin();clock_ns+=10;
    assert(ciuki_layout_end(start,true));
    assert(ciuki_layout_max_ns==75000001);
    puts("NetSurf layout budget: rejection timing, deadline and next-operation reset passed");fflush(stdout);
}
