/* Atomic nonempty-directory profile lock, including dead-owner recovery.
 * A reaper removes only the old owner's uniquely named file. It cannot remove
 * a replacement owner's nonempty lock directory. SPDX-License-Identifier: MIT */
#ifndef CIUKI_PROFILE_LOCK_H
#define CIUKI_PROFILE_LOCK_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct internet_profile_lock { char directory[600],owner[680]; };
static inline void internet_profile_unlock(struct internet_profile_lock *lock)
{ if(lock->owner[0]) { unlink(lock->owner);rmdir(lock->directory);lock->owner[0]=0; } }
static inline int internet_profile_lock(const char *profile,struct internet_profile_lock *lock)
{
    *lock=(struct internet_profile_lock){0};char candidate[600],owner[680],base[80];
    int n=snprintf(lock->directory,sizeof(lock->directory),"%s/.profile-lock",profile);
    if(n<0 || (size_t)n>=sizeof(lock->directory)) { errno=ENAMETOOLONG;return -1; }
    n=snprintf(candidate,sizeof(candidate),"%s/.lock-XXXXXX",profile);
    if(n<0 || (size_t)n>=sizeof(candidate)) { errno=ENAMETOOLONG;return -1; }
    if(!mkdtemp(candidate))return -1;
    snprintf(base,sizeof(base),"owner-%ld-%s",(long)getpid(),strrchr(candidate,'/')+1);
    snprintf(owner,sizeof(owner),"%s/%s",candidate,base);
    int fd=open(owner,O_WRONLY|O_CREAT|O_EXCL,0600);int error=errno;
    if(fd<0) { rmdir(candidate);errno=error;return -1; }
    char pid[32];n=snprintf(pid,sizeof(pid),"%ld\n",(long)getpid());
    if(write(fd,pid,(size_t)n)!=n || fsync(fd)) { error=errno?errno:EIO;close(fd);unlink(owner);rmdir(candidate);errno=error;return -1; }
    if(close(fd)) { error=errno;unlink(owner);rmdir(candidate);errno=error;return -1; }
    for(unsigned attempt=0;attempt<100;attempt++) {
        if(!rename(candidate,lock->directory)) {
            snprintf(lock->owner,sizeof(lock->owner),"%s/%s",lock->directory,base);return 0;
        }
        error=errno;
        if(error!=EEXIST && error!=ENOTEMPTY && error!=EACCES)break;
        DIR *d=opendir(lock->directory);
        if(d) {
            struct dirent *e;while((e=readdir(d)))if(!strncmp(e->d_name,"owner-",6)) {
                char old[900];int length=snprintf(old,sizeof(old),"%s/%s",lock->directory,e->d_name);
                if(length<0 || (size_t)length>=sizeof(old))continue;
                FILE *f=fopen(old,"r");long owner_pid=0;if(f) { if(fscanf(f,"%ld",&owner_pid)!=1)owner_pid=0;fclose(f); }
                if(owner_pid>0 && kill((pid_t)owner_pid,0) && errno==ESRCH) { unlink(old);rmdir(lock->directory); }
            }
            closedir(d);
        }
        struct timespec delay={.tv_nsec=10000000};nanosleep(&delay,NULL);
    }
    unlink(owner);rmdir(candidate);errno=error==EEXIST || error==ENOTEMPTY || error==EACCES?EBUSY:error;return -1;
}
#endif
