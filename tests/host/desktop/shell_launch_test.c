/* Production shell launcher with an explicit descriptor/process boundary.
 * SPDX-License-Identifier: MIT */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "fake.h"
#undef DT_UNKNOWN
#undef DT_CHR
#undef DT_DIR
#undef DT_REG
#include <dirent.h>
#undef ILL_ILLOPC
#undef FPE_INTDIV
#undef SEGV_MAPERR
#undef SEGV_ACCERR
#undef BUS_ADRALN
#include <signal.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CIUKI_RUNTIME_H
#define CIUKI_RAW_H
#define CU_PTR(p) ((uintptr_t)(p))
static int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }
uint32_t ciuki_raw_probe_report(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
int shell_close(int);
#define fcntl shell_fcntl
#define close shell_close
#define main desktop_main
#include "../../../apps/desktop/desktop.c"
#undef main
#undef close
#undef fcntl
static int fail_mapping, fail_spawn, pid_source=100;
static char captured_report[CIUKI_PROBE_REPORT_MAX+1];
uint32_t ciuki_raw_probe_report(uintptr_t address,uint32_t n,uint32_t a,uint32_t b,uint32_t c,uint32_t e)
{
    assert(!a && !b && !c && !e && n<=CIUKI_PROBE_REPORT_MAX);
    memcpy(captured_report,(void *)address,n); captured_report[n]=0; return 0;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{ return fake_ops.send(fd,m,flags); }
int ciuki_surface_info(int fd,struct ciuki_surface_info *info)
{ return fake_ops.info(fd,info); }
void *ciuki_surface_map(int fd,int prot) { return fake_ops.map(fd,prot); }
int ciuki_present(int fd,int surface,const struct ciuki_rect *rect)
{ return fake_ops.present(fd,surface,rect); }
int ciuki_channel_pair(int pair[2])
{ pair[0]=20; pair[1]=21; fake_channel(20); fake_channel(21); return 0; }
int shell_fcntl(int fd, int command, ...)
{
    assert(fd==21 && command==F_SETFD);
    va_list a; va_start(a,command); assert(va_arg(a,int)==0); va_end(a);
    if (fail_mapping) { errno=EBADF; return -1; }
    return 0;
}
int shell_close(int fd) { return fake_ops.close(fd); }
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],
                      const struct ciuki_spawn_fd *mapping,uint32_t count,uint32_t flags)
{
    assert(!strcmp(path,"/bin/demo") && !strcmp(argv[0],"demo") && !strcmp(argv[1],"--channel-fd=3") && !argv[2]);
    assert(!strcmp(env[0],"LC_ALL=C") && !strcmp(env[1],"TZ=UTC0") && !env[5]);
    assert(count==1 && mapping[0].source==21 && mapping[0].target==3 && !flags);
    if (fail_spawn) { errno=ENOEXEC; return -1; }
    return pid_source++;
}
static void message(struct desktop *d,int index,unsigned opcode,unsigned length)
{
    struct ciuki_message m;
    desk_message(&m,opcode,length); m.data[0]=DESK_SHELL_VERSION; m.sender_pid=d->shell_pid;
    desk_receive(d,index,&m);
}
int main(void)
{
    struct desktop d;
    fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    struct desk_program program={"demo","/bin/demo"};
    fail_mapping=1; assert(launch_program(&d,&program)==-1 && errno==EBADF && !fake_live());
    fail_mapping=0; fail_spawn=1;
    assert(launch_program(&d,&program)==-1 && errno==ENOEXEC && !fake_live());
    fail_spawn=0;
    d.shell_test=1; d.mouse_x=40; d.mouse_y=464;
    assert(desk_shell_click(&d) && d.menu_open && d.shell_stage==1);
    int pid=launch_program(&d,&program);
    assert(pid==100 && d.clients[0].pid==100 && d.children[0].pid==100 && fake_live()==1);
    d.shell_pid=(unsigned)pid; d.shell_stage=2;
    message(&d,0,DESK_HELLO,4);
    fake_surface(22,240,160,0xabcdef);
    struct ciuki_message m;
    desk_message(&m,DESK_CREATE_WINDOW,20); m.data[0]=2; m.sender_pid=100;
    m.fd_count=1; m.fds[0]=22;
    desk_put32(m.data+4,240); desk_put32(m.data+8,160); desk_put32(m.data+12,4);
    memcpy(m.data+16,"Demo",4); desk_receive(&d,0,&m);
    assert(d.clients[0].window && d.shell_stage==3);
    d.menu_open=0;
    struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
    d.mouse_x=40; d.mouse_y=60; desk_input(&d,&e); assert(d.drag==0);
    e.type=CIUKI_INPUT_MOTION; e.code=0; e.value=60; e.value2=60; desk_input(&d,&e);
    assert(d.shell_stage==4 && d.clients[0].frame.x==80 && d.clients[0].frame.y==112);
    e.type=CIUKI_INPUT_BUTTON; e.code=1; e.value=e.value2=0; desk_input(&d,&e);
    struct desk_box close=desk_close_box(&d.clients[0]);
    d.mouse_x=close.x+4; d.mouse_y=close.y+4; e.value=1; desk_input(&d,&e);
    assert(d.shell_stage==5 && d.children[0].close_ns==DESK_CLOSE_NS && d.clients[0].window);
    desk_flush(&d,0);
    message(&d,0,DESK_CLOSE,4);
    assert(!fake_live() && d.shell_stage==5); /* EOF alone cannot qualify exit. */
    desk_child_exited(&d,101,0); assert(d.shell_stage==5);
    gate_keys=2; d.shell_keys=2; gate_report(&d,0,1,42,0,100);
    assert(strstr(captured_report," keys=0 motion=0 buttons=0"));
    desk_child_exited(&d,100,0); assert(d.shell_stage==6 && !d.children[0].pid);
    gate_keys=1; gate_report(&d,0,1,42,0,100);
    assert(strstr(captured_report," keys=0 motion=0 buttons=0"));
    gate_keys=2; gate_report(&d,0,1,42,0,100);
    assert(strstr(captured_report," keys=4 motion=0 buttons=0"));
    for (int i=0;i<DESK_CLIENTS;i++) d.children[i].pid=200+i;
    assert(launch_program(&d,&program)==-1 && errno==EAGAIN && !fake_live());
    fake_reset();
    puts("PASS production launcher: explicit channel-only inheritance/group, CLOEXEC/spawn rollback, PID binding, menu/drag/close/reap evidence, capacity");
    return 0;
}
