/* SPDX-License-Identifier: MIT
 * One synchronous layout owner. All exits, including rejection, are measured. */
#include <time.h>
#include "budget.h"
static uint64_t deadline;
uint64_t ciuki_layout_max_ns;
static uint64_t now(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return UINT64_MAX;
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
uint64_t ciuki_layout_begin(void)
{
    uint64_t start=now();
    deadline=start>UINT64_MAX-75000000u?UINT64_MAX:start+75000000u;
    return start;
}
bool ciuki_layout_expired(void) { return deadline && now()>=deadline; }
bool ciuki_layout_end(uint64_t start,bool accepted)
{
    uint64_t finish=now(),elapsed=finish>=start?finish-start:UINT64_MAX;
    if(elapsed>ciuki_layout_max_ns)ciuki_layout_max_ns=elapsed;
    deadline=0;
    return accepted && elapsed<=75000000u;
}
