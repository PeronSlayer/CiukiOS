/* Cross-process history deletion; the marker invalidates old pending visits.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_INTERNET_HISTORY_H
#define CIUKI_INTERNET_HISTORY_H
#include "profile_lock.h"
static inline int internet_history_clear(const char *profile)
{
    struct internet_profile_lock lock;if(internet_profile_lock(profile,&lock))return -1;
    char path[600],temporary[600],marker[96];struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
    snprintf(path,sizeof(path),"%s/History.clear",profile);snprintf(temporary,sizeof(temporary),"%s/History.clear.XXXXXX",profile);
    int fd=mkstemp(temporary),error=fd<0?errno:0;
    int bytes=snprintf(marker,sizeof(marker),"%ld:%lld:%ld\n",(long)getpid(),(long long)now.tv_sec,(long)now.tv_nsec);
    if(fd>=0) {
        if(write(fd,marker,(size_t)bytes)!=bytes || fsync(fd))error=errno?errno:EIO;
        if(close(fd) && !error)error=errno;
        if(!error && rename(temporary,path))error=errno;
        if(error)unlink(temporary);
    }
    if(!error) { snprintf(path,sizeof(path),"%s/History.html",profile);if(unlink(path) && errno!=ENOENT)error=errno; }
    internet_profile_unlock(&lock);if(error) { errno=error;return -1; }return 0;
}
#endif
