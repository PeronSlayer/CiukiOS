/* Actual desktop native wiring, with kernel calls replaced at the boundary. */
#include "desktop.h"
#include "win32_adapter.h"
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
static unsigned starts,signals;
static int authorized=1;
static unsigned native_launches;
int desk_program_kind(const char *name){return !strcmp(name,"control") ? DESK_APP_CONTROL : 0;}
static int launch(struct desktop *d,const struct desk_program *p)
{
    assert(!strcmp(p->name,"control") && !strcmp(p->path,"/Programs/control"));
    assert(!d->launch_arg && !strcmp(d->launch_argv[0],"--panel=display") && !d->launch_argv[1]);
    native_launches++;return 151;
}
void desk_shell_message(struct desktop *d,const char *title,const char *text,int error)
{(void)d;(void)title;(void)text;(void)error;assert(0);}
uint32_t desk_w32_record(void *record,uint32_t bytes)
{
    struct w32r_service *a=record;assert(bytes==sizeof(*a) && a->service==1);
    if(a->action==1){if(authorized)a->result=1;else a->error=5;}
    else assert(a->action==3);
    return 0;
}
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *maps,uint32_t count,uint32_t flags)
{
    assert(!strcmp(path,"/System/Win32/w32srv") && !strcmp(argv[0],path) && !argv[1]);
    assert(env && !maps && !count && !flags);return (int)(100+ ++starts);
}
int __wrap_kill(pid_t pid,int signo){assert(pid==(int)(100+starts) && signo==SIGTERM);signals++;return 0;}
int desk_add_client(struct desktop *d,int fd){(void)d;(void)fd;assert(0);return -1;}
int ciuki_channel_pair(int *fds){(void)fds;assert(0);return -1;}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{(void)fd;(void)m;(void)flags;assert(0);return -1;}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{(void)fd;(void)m;(void)flags;assert(0);return -1;}
int main(void)
{
    static struct desktop d;
    authorized=0;desk_w32_start(&d);desk_w32_poll(&d);assert(!starts);
    authorized=1;desk_w32_start(&d);assert(starts==1);
    desk_w32_child_exited(&d,101,SIGSEGV);assert(starts==2);
    desk_w32_child_exited(&d,102,SIGKILL);assert(starts==3);
    d.power.state=DESK_POWER_RETRY;
    assert(!desk_w32_shutdown() && signals==1);
    assert(!desk_w32_shutdown() && signals==1);
    desk_w32_child_exited(&d,103,0);assert(desk_w32_shutdown()==1 && starts==3);
    d.power.state=0;desk_w32_poll(&d);assert(starts==4);
    d.power.state=DESK_POWER_RETRY;assert(!desk_w32_shutdown() && signals==2);
    desk_w32_child_exited(&d,104,SIGKILL);assert(desk_w32_shutdown()==-EIO && starts==4);
    d.power.state=0;d.launch=launch;d.launch_arg="retained";d.launch_argv[0]="sentinel";
    struct desk_launch_record r={.label="Control",.target="C:\\Programs\\control",.arguments="--panel=display",.show=1};
    assert(!desk_w32_launch(DESK_W32_ELF,&r));desk_w32_poll(&d);
    assert(native_launches==1 && !strcmp(d.launch_arg,"retained") && !strcmp(d.launch_argv[0],"sentinel"));
    strcpy(r.directory,"C:\\unsupported-cwd");assert(desk_w32_native_shortcut(&d,&r)==-EOPNOTSUPP);
    r.directory[0]=0;strcpy(r.arguments,"1 2 3 4 5 6 7");assert(desk_w32_native_shortcut(&d,&r)==-E2BIG);
    assert(native_launches==1);
    strcpy(r.target,"C:\\Documents\\control");r.arguments[0]=0;
    assert(desk_w32_native_shortcut(&d,&r)==-EPERM && native_launches==1);
    puts("PASS reserved native identity cannot be forged by a similarly named user ELF");
    puts("PASS native shortcut uses existing launcher identity, six-argument bound, retained launch state and explicit unsupported cwd");
    puts("PASS native desktop wiring: exact authorized child, crash/kill restart, flush-exit barrier, no restart during power-off, Cancel resumes service, failed flush blocks power-off");
    return 0;
}
