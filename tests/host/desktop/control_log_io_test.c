/* SPDX-License-Identifier: MIT
 * Execute the native applet's positioned reader on a host fixture. */
#include "replay_base.h"
#undef IPPROTO_IP
#undef IPPROTO_TCP
#undef IPPROTO_UDP
#include <netinet/in.h>
int cp_test_open(const char *,int,...);
ssize_t cp_test_pread(int,void *,size_t,off_t);
int cp_test_fputs(const char *,FILE *);
#define main cp_native_main
#define open cp_test_open
#define pread cp_test_pread
#define fputs cp_test_fputs
#include "../../../apps/control/main.c"
#undef main
#undef open
#undef pread
#undef fputs
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
static unsigned state_records;
static char last_state_record[256];
int cp_test_fputs(const char *line, FILE *stream)
{
    assert(stream == stderr && strstr(line, "[control] display state="));
    state_records++; snprintf(last_state_record, sizeof(last_state_record), "%s", line); return 0;
}
int cp_system_thumbnail(const char *path, char *out, size_t size)
{ (void)out; (void)size; return !strcmp(path, "/system/wallpapers/Ciuk2.png"); }
static void state_log_test(void)
{
    static struct cp_main m;
    m.app.page = CP_DISPLAY; m.app.decode_wait = 1;
    strcpy(m.app.decode_path, "/system/wallpapers/Ciuk2.png");
    display_log(&m, "event");
    assert(state_records == 1 && strstr(last_state_record, "state=background decode=thumbnail ui_wait=event"));
    for (unsigned i = 0; i < 1000; i++) display_log(&m, "event");
    assert(state_records == 1);
    m.app.display_tab = CP_APPEARANCE; m.app.decode_cancelled = 1; display_log(&m, "event");
    assert(state_records == 2 && strstr(last_state_record, "state=appearance decode=cancelling"));
    m.app.decode_wait = m.app.decode_cancelled = 0; m.app.decode_done++; display_log(&m, "event");
    assert(state_records == 3 && strstr(last_state_record, "decode=idle"));
    m.app.page = CP_HOME; display_log(&m, "event"); display_log(&m, "event");
    assert(state_records == 4 && strstr(last_state_record, "state=closed"));
    m.app.page = CP_DISPLAY; m.app.display_tab = CP_SCREEN_SETTINGS; display_log(&m, "timer");
    assert(state_records == 5 && strstr(last_state_record, "state=settings") && strstr(last_state_record, "ui_wait=timer"));
    puts("PASS display state records: one per transition, 1000 idle calls emit none, cancel, completion, tabs and close");
}
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
    assert(argc==2); fixture=argv[1]; state_log_test();
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
