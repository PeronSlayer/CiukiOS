/* Host OS I/O isolated from the CiukiOS ABI's O_* values. */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>
int st_hopen(const char *p) { int h=open(p,O_RDONLY); return h<0 ? -errno : h; }
int st_hread(int h,void *p,unsigned n) { ssize_t r=read(h,p,n); return r<0 ? -errno : (int)r; }
int st_hseek(int h,uint64_t n) { return lseek(h,(off_t)n,SEEK_SET)<0 ? -errno : 0; }
int st_hclose(int h) { return close(h) ? -errno : 0; }
int st_hinfo(const char *p,uint64_t *size,int *directory)
{ struct stat s; if(stat(p,&s))return -errno; *size=(uint64_t)s.st_size; *directory=S_ISDIR(s.st_mode); return 0; }

/* Native media calls are linked, but never performed on the Linux host. */
int ciuki_devctl(int h,uint32_t op,void *p,uint32_t n) { (void)h; (void)op; (void)p; (void)n; errno=ENOTSUP; return -1; }
int ciuki_power(uint32_t action) { (void)action; errno=EPERM; return -1; }
