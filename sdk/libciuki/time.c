/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
int clock_gettime(clockid_t c,struct timespec *t) { uint32_t r=CU_CALL(CLOCK_GETTIME,c,CU_PTR(t),0,0,0,0);int e=ciuki_error(r);if(e) { errno=e;return -1; }return 0; }
int clock_getres(clockid_t c,struct timespec *t) {
    if(c!=CLOCK_REALTIME&&c!=CLOCK_MONOTONIC&&c!=CLOCK_PROCESS_CPUTIME_ID) { errno=EINVAL;return -1; }
    /* Contract: all three clocks use 1 ms accounting. */
    if(t)*t=(struct timespec){0,1000000,0};return 0;
}
int nanosleep(const struct timespec *t,struct timespec *rem) { uint32_t r=CU_CALL(NANOSLEEP,CU_PTR(t),CU_PTR(rem),0,0,0,0);int e=ciuki_error(r);if(e) { errno=e;return -1; }return 0; }
unsigned sleep(unsigned n) { struct timespec t={n,0,0},r={0,0,0};if(!nanosleep(&t,&r))return 0;return errno==EINTR?(unsigned)r.tv_sec+(r.tv_nsec!=0):n; }
int usleep(useconds_t n) { struct timespec t={n/1000000,(n%1000000)*1000,0};return nanosleep(&t,NULL); }
time_t time(time_t *p) { struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t)<0)return -1;if(p)*p=t.tv_sec;return t.tv_sec; }
int _gettimeofday_r(struct _reent *r,struct timeval *t,void *zone) { (void)zone;struct timespec now;uint32_t v=CU_CALL(CLOCK_GETTIME,CLOCK_REALTIME,CU_PTR(&now),0,0,0,0);int e=ciuki_error(v);if(e) { r->_errno=e;return -1; }if(t)*t=(struct timeval){now.tv_sec,now.tv_nsec/1000,0};return 0; }
int gettimeofday(struct timeval *t,void *zone) { return _gettimeofday_r(__getreent(),t,zone); }
clock_t clock(void) { struct timespec t;if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)<0)return -1;return t.tv_sec*CLOCKS_PER_SEC+t.tv_nsec/(1000000000/CLOCKS_PER_SEC); }
/* F2 calendar functions are UTC even if the caller sets TZ. */
struct tm *localtime_r(const time_t *t,struct tm *out) { return gmtime_r(t,out); }
struct tm *localtime(const time_t *t) { return gmtime(t); }
/* Upstream mktime uses the default zero-offset tzinfo. Do not parse TZ. */
extern void __tz_lock(void),__tz_unlock(void);
void _tzset_unlocked_r(struct _reent *r) { (void)r; }
void _tzset_unlocked(void) { }
void _tzset_r(struct _reent *r) { (void)r;__tz_lock();__tz_unlock(); }
void tzset(void) { _tzset_r(__getreent()); }
