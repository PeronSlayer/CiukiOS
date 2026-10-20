/* The production fixture controller must prove service readiness before PE
 * spawn, including the first desktop-v3 fixture (not only w32-dialogs). */
#include "desktop.h"
#include "win32_adapter.h"
#include <ciuki/spawn.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int ready,spawns;
static char last_record[256],last_path[128];
uint32_t desk_w32_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    assert(n>=42 && cap>=40);const struct w32r_request *r=in;
    assert(r->op==W32R_OPEN || r->op==W32R_CLOSE);
    if(!ready)return 1062;
    struct w32r_reply reply={.version=1,.key=r->op==W32R_OPEN?7:0};memcpy(out,&reply,40);*used=40;return 0;
}
int ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t count,uint32_t flags)
{
    assert(ready && argv && env && !fds && !count && !flags);strcpy(last_path,path);spawns++;return 71;
}
void desk_v3_fixture_report(const char *line){assert(strlen(line)<sizeof(last_record));strcpy(last_record,line);}
void desk_request_close(struct desktop *d,int i){(void)d;(void)i;}
void desk_queue(struct desktop *d,int i,const struct ciuki_message *m){(void)d;(void)i;(void)m;assert(0); }
int ciuki_win32_uninstall_list(struct desk_uninstall_entry *r,unsigned n){(void)r;(void)n;return 0;}
int desk_w32_native_shortcut(struct desktop *d,const struct desk_launch_record *r){(void)d;(void)r;return 1;}
int desk_v3_fixture_begin(struct desktop *);
void desk_v3_fixture_tick(struct desktop *);
int desk_dialogs_fixture_begin(struct desktop *);
void desk_dialogs_fixture_tick(struct desktop *);
int main(void)
{
    setbuf(stdout,NULL);static struct desktop d;
    assert(!desk_v3_fixture_begin(&d));desk_v3_fixture_tick(&d);assert(!spawns);
    ready=1;desk_v3_fixture_tick(&d);assert(spawns==1 && !strcmp(last_path,"/System/Win32/Fixtures/desktop-v3.exe"));
    desk_v3_fixture_tick(&d);assert(spawns==1);
    d.now_ns=15000000000ull;desk_v3_fixture_tick(&d);assert(strstr(last_record,"errors=1"));
    d.now_ns=0;ready=0;assert(!desk_dialogs_fixture_begin(&d));desk_dialogs_fixture_tick(&d);assert(spawns==1);
    ready=1;desk_dialogs_fixture_tick(&d);assert(spawns==2 && !strcmp(last_path,"/System/Win32/Fixtures/dialogs-exercise.exe"));
    puts("PASS both PE fixtures wait for a registered w32srv");return 0;
}
