/* SPDX-License-Identifier: MIT
 * Execute the native applet's positioned reader on a host fixture. */
#include "replay_base.h"
int cp_test_open(const char *,int,...);
ssize_t cp_test_pread(int,void *,size_t,off_t);
#define main cp_native_main
#define open cp_test_open
#define pread cp_test_pread
#include "../../../apps/control/main.c"
#undef main
#undef open
#undef pread
#include <assert.h>

/* These callbacks are retained by ASan's global store instrumentation, but
 * this focused reader fixture must never invoke program management. */
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *c)
{(void)op;(void)c;assert(0);return -1;}
void cfg_icon_forget(struct ciuki_settings *s,const char *name)
{(void)s;(void)name;assert(0);}
size_t cfg_format(const struct ciuki_settings *s,char *out,size_t cap)
{(void)s;(void)out;(void)cap;assert(0);return 0;}
int cfg_writer_submit(struct cfg_writer *w,const char *text,size_t length)
{(void)w;(void)text;(void)length;assert(0);return -1;}
int cfg_writer_result(struct cfg_writer *w,int *error)
{(void)w;(void)error;assert(0);return -1;}
static const char *fixture;
static unsigned reads, interruptions;
static int fail_open;
int cp_test_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, CP_LOG_FILE));
    assert(flags == (O_RDONLY | O_CLOEXEC));
    if (fail_open) { errno=ENOENT; return -1; }
    return open(fixture,flags);
}
ssize_t cp_test_pread(int fd, void *out, size_t size, off_t offset)
{
    reads++;
    if (interruptions) { interruptions--; errno=EINTR; return -1; }
    if (size>97) size=97;
    return pread(fd,out,size,offset);
}
int main(int argc, char **argv)
{
    assert(argc==2); fixture=argv[1];
    static char actual[CP_LOG_BYTES], expected[CP_LOG_BYTES];
    size_t n; uint32_t total;
    FILE *f=fopen(fixture,"rb"); assert(f && !fseek(f,0,SEEK_END));
    long size=ftell(f); assert(size==320*1024);
    interruptions=2;
    for (uint32_t offset=0;offset<(uint32_t)size;offset+=CP_LOG_BYTES) {
        assert(!log_read(NULL,offset,actual,sizeof(actual),&n,&total));
        assert(total==(uint32_t)size && n<=sizeof(actual));
        assert(!fseek(f,(long)offset,SEEK_SET));
        assert(fread(expected,1,sizeof(expected),f)==n && !memcmp(actual,expected,n));
    }
    assert(reads>40 && !interruptions);
    assert(!log_read(NULL,total,actual,sizeof(actual),&n,&total) && !n);
    fail_open=1; assert(log_read(NULL,0,actual,sizeof(actual),&n,&total)==-ENOENT);
    fclose(f);
    puts("PASS native boot log: fixed read-only path, complete 320 KiB in bounded pages, short reads, EINTR, EOF and missing file");
    return 0;
}
