#include <ciuki/runtime.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <stddef.h>
#define SAME(type,wire) _Static_assert(sizeof(type)==sizeof(wire),#type " size"); _Static_assert(_Alignof(type)==_Alignof(wire),#type " alignment")
SAME(off_t,ciuki_off_t);
SAME(time_t,ciuki_time_t);
SAME(clock_t,ciuki_clock_t);
SAME(ino_t,ciuki_ino_t);
SAME(dev_t,ciuki_dev_t);
SAME(nlink_t,ciuki_nlink_t);
SAME(mode_t,ciuki_mode_t);
SAME(struct stat,struct ciuki_stat);
SAME(struct timespec,struct ciuki_timespec);
SAME(struct timeval,struct ciuki_timeval);
SAME(struct sigaction,struct ciuki_sigaction);
SAME(pthread_mutex_t,struct ciuki_pthread_mutex);
SAME(pthread_cond_t,struct ciuki_pthread_cond);
SAME(pthread_attr_t,struct ciuki_pthread_attr);
SAME(pthread_mutexattr_t,struct ciuki_pthread_mutexattr);
SAME(pthread_condattr_t,struct ciuki_pthread_condattr);
_Static_assert(sizeof(off_t)==8&&sizeof(time_t)==8,"64-bit file/time");
_Static_assert(sizeof(pthread_once_t)==4&&sizeof(pthread_t)==4,"pthread scalar widths");
_Static_assert(offsetof(struct sigaction,sa_mask)==offsetof(struct ciuki_sigaction,mask),"signal mask offset");
_Static_assert(offsetof(struct sigaction,sa_restorer)==offsetof(struct ciuki_sigaction,restorer),"restorer offset");
_Static_assert(sizeof(_off_t)==sizeof(off_t)&&sizeof(_fpos_t)==sizeof(off_t),"stdio offset is 64-bit");
_Static_assert(sizeof(_TIME_T_)==sizeof(time_t)&&sizeof(_CLOCK_T_)==sizeof(clock_t),"newlib private widths");
#ifdef _POSIX_VERSION
#error complete POSIX conformance must not be advertised
#endif
#ifdef _POSIX_THREADS
#error complete pthread option must not be advertised
#endif
int ciuki_layout_errors(void) {
    int failures=0;
    failures+=ciuki_error((uint32_t)-EINTR)!=EINTR;
    failures+=ciuki_error((uint32_t)-ENOSYS)!=ENOSYS;
    failures+=ciuki_error(0xb0000000u)!=0;
    failures+=ciuki_error((uint32_t)-CIUKI_SYSCALL_ERROR_MAX)!=CIUKI_SYSCALL_ERROR_MAX;
    failures+=ciuki_error((uint32_t)-CIUKI_SYSCALL_ERROR_MAX-1)!=0;
    return failures;
}
