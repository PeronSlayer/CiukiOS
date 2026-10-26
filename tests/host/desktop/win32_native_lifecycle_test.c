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
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <ciuki/w32_diagnostics.h>
static unsigned directory_state=CIUKI_PROC_LIVE;
int ciuki_proc_list(void *p,uint32_t n)
{
    assert(n>=sizeof(struct ciuki_proc_system)+sizeof(struct ciuki_proc_record));
    struct ciuki_proc_system *s=p;memset(p,0,n);s->version=CIUKI_PROC_VERSION;s->size=sizeof(*s);s->records=1;
    struct ciuki_proc_record *r=(void *)(s+1);r->pid=700;r->state=directory_state;return 1;
}
extern int desk_restart_process_alive(uint32_t);
static unsigned starts,signals;
static int authorized=1;
static pthread_t desktop_thread;
static unsigned native_launches,dos_launches;
static int expected_show=1;
int desk_task_launch(struct desktop *d,const char *name,const char *arg)
{assert(pthread_equal(pthread_self(),desktop_thread));assert(d && d->launch_show==3 && !strcmp(name,"dosbox") && !strcmp(arg,"D:\\INSTALL.BAT"));dos_launches++;return 152;}
static int readiness;
static unsigned idle_enabled,idle_calls,idle_finished,idle_boot_checks;
static uint64_t fixture_ms;
static pthread_t idle_thread;
static struct w32r_packet idle_packet;
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *t)
{
    if(clock==CLOCK_MONOTONIC){uint64_t ms=__atomic_load_n(&fixture_ms,__ATOMIC_ACQUIRE);t->tv_sec=(time_t)(ms/1000);t->tv_nsec=(long)(ms%1000)*1000000;return 0;}
    return __real_clock_gettime(clock,t);
}
static uint64_t real_ms(void)
{struct timespec t;assert(!__real_clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static void advance(uint64_t ms){__atomic_store_n(&fixture_ms,ms,__ATOMIC_RELEASE);}
static void wait_for(unsigned *counter,unsigned value)
{
    uint64_t end=real_ms()+3000;
    while(__atomic_load_n(counter,__ATOMIC_ACQUIRE)<value){
        assert(real_ms()<end);struct timespec t={0,1000000};nanosleep(&t,NULL);
    }
}
int desk_w32_ready(void){return readiness;}
int desk_program_kind(const char *name){return !strcmp(name,"control") ? DESK_APP_CONTROL : 0;}
static int launch(struct desktop *d,const struct desk_program *p)
{
    assert(!strcmp(p->name,"control") && !strcmp(p->path,"/Programs/control"));
    assert(!d->launch_arg && !strcmp(d->launch_argv[0],"--panel=display") && !d->launch_argv[1]);
    assert(d->launch_show==expected_show);
    native_launches++;return 151;
}
void desk_shell_message(struct desktop *d,const char *title,const char *text,int error)
{(void)d;(void)title;(void)text;(void)error;assert(0);}
uint32_t desk_w32_record(void *record,uint32_t bytes)
{
    if(((struct ciuki_w32_header *)record)->operation==W32_REPORT){assert(bytes==sizeof(struct ciuki_w32_report_args));return 0;}
    struct w32r_service *a=record;assert(bytes==sizeof(*a));if(a->service==2){
        if(__atomic_load_n(&idle_enabled,__ATOMIC_ACQUIRE)){a->result=1;a->endpoint=7;}else a->error=1062;
        return 0;
    }assert(a->service==1);
    if(a->action==1){if(authorized)a->result=1;else a->error=5;}
    else assert(a->action==3);
    return 0;
}
static int spawn_after_registration;   /* after a registry exchange succeeded the respawn carries no --fresh-clients */
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *maps,uint32_t count,uint32_t flags)
{
    /* The snapshot lists a live pid 700: a survivor of an earlier desktop
     * when this process's pid is above it (no reset), a child otherwise;
     * after a registry exchange succeeded the respawn carries no flag. */
    const char *expected=700u<(uint32_t)getpid() ? NULL : "--fresh-clients";
    assert(!strcmp(path,"/System/Win32/w32srv") && !strcmp(argv[0],path));
    if(spawn_after_registration)assert(!argv[1]);
    else assert(expected ? argv[1] && !strcmp(argv[1],expected) && !argv[2] : !argv[1]);
    assert(env && !maps && !count && !flags);return (int)(100+ ++starts);
}
static unsigned kills;   /* review P2-7: the bounded shutdown's SIGKILL */
int __wrap_kill(pid_t pid,int signo){assert(pid==(int)(100+starts) && (signo==SIGTERM || signo==SIGKILL));if(signo==SIGKILL)kills++;else signals++;return 0;}
int desk_add_client(struct desktop *d,int fd){(void)d;(void)fd;assert(0);return -1;}
int ciuki_channel_pair(int *fds){(void)fds;assert(0);return -1;}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert(fd==7 && flags==DONTWAIT && !pthread_equal(pthread_self(),desktop_thread));
    memcpy(&idle_packet,m->data,sizeof(idle_packet));
    if(idle_packet.kind==W32R_BEGIN){
        struct w32r_request r;memcpy(&r,idle_packet.bytes,sizeof(r));
        assert(r.op==W32R_SHUTDOWN && r.index==W32R_SHUTDOWN_POLL);
        if(r.flags)__atomic_add_fetch(&idle_boot_checks,1,__ATOMIC_RELEASE);
        unsigned n=__atomic_add_fetch(&idle_calls,1,__ATOMIC_RELEASE);
        if(n==1)idle_thread=pthread_self();else assert(pthread_equal(idle_thread,pthread_self()));
        if(n==1){struct timespec t={2,0};nanosleep(&t,NULL);}
    }
    return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==7 && flags==DONTWAIT && !pthread_equal(pthread_self(),desktop_thread));
    memset(m,0,sizeof(*m));m->length=256;
    struct w32r_packet p={.version=1,.id=idle_packet.id,.kind=idle_packet.kind,.total=sizeof(struct w32r_reply)};
    p.offset=p.kind==W32R_READ?sizeof(struct w32r_reply):idle_packet.total;
    if(p.kind==W32R_READ){struct w32r_reply r={.version=1};memcpy(p.bytes,&r,sizeof(r));__atomic_add_fetch(&idle_finished,1,__ATOMIC_RELEASE);}
    memcpy(m->data,&p,sizeof(p));return 1;
}
static void idle_traffic(struct desktop *d)
{
    /* Drain the earlier service-unavailable poll before exposing the fake
     * endpoint; otherwise that old request would use the newly enabled gate. */
    for(unsigned i=0;i<20;i++){desk_w32_poll(d);struct timespec t={0,1000000};nanosleep(&t,NULL);}
    advance(100000);__atomic_store_n(&idle_enabled,1,__ATOMIC_RELEASE);
    /* An earlier failed poll can still be in the mailbox. Repeated same-time
     * frames drain it, then publish exactly one new request. */
    uint64_t end=real_ms()+1000;
    while(!__atomic_load_n(&idle_calls,__ATOMIC_ACQUIRE)){
        assert(real_ms()<end);desk_w32_poll(d);struct timespec t={0,1000000};nanosleep(&t,NULL);
    }
    uint64_t start=real_ms();desk_w32_poll(d);desk_w32_poll(d);
    assert(real_ms()-start<50 && __atomic_load_n(&idle_calls,__ATOMIC_ACQUIRE)==1);
    wait_for(&idle_finished,1);
    /* Wait for result publication without advancing the fixture clock. */
    for(unsigned i=0;i<20;i++){desk_w32_poll(d);struct timespec t={0,1000000};nanosleep(&t,NULL);}
    advance(104999);desk_w32_poll(d);assert(idle_calls==1);
    advance(105000);desk_w32_poll(d);wait_for(&idle_finished,2);
    for(unsigned i=0;i<20;i++){desk_w32_poll(d);struct timespec t={0,1000000};nanosleep(&t,NULL);}
    assert(idle_boot_checks==1);
    d->clients[0].v3_win32_hwnd=123;
    advance(105499);desk_w32_poll(d);assert(idle_calls==2);
    advance(105500);desk_w32_poll(d);wait_for(&idle_finished,3);
    for(unsigned i=0;i<20;i++){desk_w32_poll(d);struct timespec t={0,1000000};nanosleep(&t,NULL);}
    d->clients[0].v3_win32_hwnd=0;
    advance(106000);
    struct desk_launch_record r={.label="Control",.target="C:\\Programs\\control",.arguments="--panel=display",.show=1};
    expected_show=1;
    assert(!desk_w32_launch(DESK_W32_ELF,&r));desk_w32_poll(d);wait_for(&idle_finished,4);
    for(unsigned i=0;i<20;i++){desk_w32_poll(d);struct timespec t={0,1000000};nanosleep(&t,NULL);}
    advance(110999);desk_w32_poll(d);assert(idle_calls==4);
    advance(111000);desk_w32_poll(d);wait_for(&idle_finished,5);
    puts("PASS desktop idle: blocked 2 s RPC off frame thread, one persistent worker, boot once, 500 ms HWND/launch and 5 s quiet intervals");
}
int main(void)
{
    assert(desk_restart_process_alive(700));directory_state=CIUKI_PROC_STOPPING;assert(desk_restart_process_alive(700));
    directory_state=CIUKI_PROC_ZOMBIE;assert(!desk_restart_process_alive(700));assert(!desk_restart_process_alive(701));directory_state=CIUKI_PROC_LIVE;

    desktop_thread=pthread_self();
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
    /* The production registry ping precedes PE readiness; this fixture has
     * no service yet, so it must reject the launch at that first barrier. */
    readiness=-EACCES;assert(desk_w32_launch(DESK_W32_PE,&r)==-EAGAIN && starts==4);readiness=0;
    r.show=7;expected_show=7;d.launch_show=1;
    assert(!desk_w32_launch(DESK_W32_ELF,&r));desk_w32_poll(&d);
    assert(native_launches==1 && d.launch_show==1 && !strcmp(d.launch_arg,"retained") && !strcmp(d.launch_argv[0],"sentinel"));
    strcpy(r.directory,"C:\\unsupported-cwd");assert(desk_w32_native_shortcut(&d,&r)==-EOPNOTSUPP);
    r.directory[0]=0;strcpy(r.arguments,"1 2 3 4 5 6 7");assert(desk_w32_native_shortcut(&d,&r)==-E2BIG);
    assert(native_launches==1);
    struct desk_launch_record dos={.label="Install",.target="/mnt/d/INSTALL.BAT",.show=3};
    assert(!desk_w32_launch(DESK_W32_DOS,&dos) && !dos_launches);
    desk_w32_poll(&d);assert(dos_launches==1 && native_launches==1 && d.launch_show==1);
    strcpy(r.target,"C:\\Documents\\control");r.arguments[0]=0;
    assert(desk_w32_native_shortcut(&d,&r)==-EPERM && native_launches==1);
    puts("PASS reserved native identity cannot be forged by a similarly named user ELF");
    puts("PASS native shortcut uses existing launcher identity, six-argument bound, retained launch state and explicit unsupported cwd");
    puts("PASS native desktop wiring: exact authorized child, crash/kill restart, flush-exit barrier, no restart during power-off, Cancel resumes service, failed flush blocks power-off");
    idle_traffic(&d);
    /* Review P2-7 (T23 c42f556e): the service's shutdown flush is bounded.
     * Not exited 10 s after SIGTERM: SIGKILL; reaped after that: the flush is
     * lost but the shutdown goes on (1, not -EIO); not reaped 10 s after the
     * SIGKILL: abandoned, the shutdown goes on. Before, desk_w32_shutdown
     * returned 0 forever and the power sequence never reached call 73. */
    unsigned starts_before=starts,signals_before=signals; spawn_after_registration=1;
    d.power.state=DESK_POWER_RETRY;
    advance(200000);assert(!desk_w32_shutdown() && signals==signals_before+1 && !kills);
    advance(209999);assert(!desk_w32_shutdown() && !kills);
    advance(210000);assert(!desk_w32_shutdown() && kills==1);
    advance(215000);assert(!desk_w32_shutdown() && kills==1);
    desk_w32_child_exited(&d,(uint32_t)(100+starts),SIGKILL);assert(desk_w32_shutdown()==1);
    d.power.state=0;desk_w32_poll(&d);assert(starts==starts_before+1);
    d.power.state=DESK_POWER_RETRY;
    advance(300000);assert(!desk_w32_shutdown() && signals==signals_before+2 && kills==1);
    advance(310000);assert(!desk_w32_shutdown() && kills==2);
    advance(319999);assert(!desk_w32_shutdown());
    advance(320000);assert(desk_w32_shutdown()==1 && kills==2);
    puts("PASS w32srv shutdown bound: SIGKILL 10 s after SIGTERM, a reaped kill is a lost flush not a failed shutdown, unreaped 10 s later abandoned");
    return 0;
}
