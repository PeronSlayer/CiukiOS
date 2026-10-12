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
static int next_pair=20, last_child=-1;
int ciuki_channel_pair(int pair[2])
{ pair[0]=next_pair; pair[1]=next_pair+1; fake_channel(pair[0]); fake_channel(pair[1]); last_child=pair[1]; next_pair+=2; return 0; }
int shell_fcntl(int fd, int command, ...)
{
    assert(fd==last_child && command==F_SETFD);
    va_list a; va_start(a,command); assert(va_arg(a,int)==0); va_end(a);
    if (fail_mapping) { errno=EBADF; return -1; }
    return 0;
}
int shell_close(int fd) { return fake_ops.close(fd); }
static int expect_files_test, expect_note_test, expect_authority=-1, expect_test=0;
static const char *expect_note_arg;
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],
                      const struct ciuki_spawn_fd *mapping,uint32_t count,uint32_t flags)
{
    assert(!strcmp(env[0],"LC_ALL=C") && !strcmp(env[1],"TZ=UTC0") && !strncmp(env[5],"CIUKI_DISPLAY=",14) && !strcmp(env[6],"CIUKI_KEYMAP=us") && !env[7]);
    assert(count>=1 && mapping[0].source==last_child && mapping[0].target==3 && !flags);
    if (!strcmp(path,"/bin/files")) {
        assert(!strcmp(argv[0],"files") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        assert(expect_files_test ? argv[2] && !strcmp(argv[2],"--test=files") && !argv[3] : !argv[2]);
    } else if (!strcmp(path,"/bin/ciuknote")) {
        /* f2-32: --test=ciuknote only in its scenario; the path argument only through LAUNCH from Files. */
        assert(!strcmp(argv[0],"ciuknote") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        unsigned at=2;
        if (expect_note_test) assert(!strcmp(argv[at++],"--test=ciuknote"));
        if (expect_note_arg) assert(!strcmp(argv[at++],expect_note_arg));
        assert(!argv[at]);
    } else if (!strcmp(path,"/bin/hello")) assert(!strcmp(argv[0],"hello") && !argv[1] && count==1);
    else if (!strcmp(path,"/bin/tasks")) {
        /* f2-33: only the Task Manager receives the authority, at fd 4. */
        assert(!strcmp(argv[0],"tasks") && !strcmp(argv[1],"--channel-fd=3"));
        unsigned at=2;
        if (expect_authority>=0) {
            assert(count==2 && mapping[1].source==expect_authority && mapping[1].target==4);
            assert(!strcmp(argv[at++],"--authority-fd=4"));
        } else assert(count==1);
        if (expect_test) assert(!strcmp(argv[at++],"--test=tasks"));
        assert(!argv[at]);
    } else assert(!strcmp(path,"/bin/demo") && !strcmp(argv[0],"demo") && !strcmp(argv[1],"--channel-fd=3") && !argv[2] && count==1);
    if (fail_spawn) { errno=ENOEXEC; return -1; }
    return pid_source++;
}
static void drop_all(struct desktop *d)
{ for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].channel>=0) desk_drop_client(d,i); }
static struct ciuki_proc_record fake_records[8];
static int fake_record_count;
/* Desktop shell services (desktop.c): no mixer and no power interface on the host. */
int ciuki_devctl(int fd,uint32_t op,void *record,uint32_t size)
{ (void)fd; (void)op; (void)record; (void)size; errno=ENODEV; return -1; }
int ciuki_power(uint32_t action) { (void)action; errno=ENOSYS; return -1; }
int ciuki_proc_list(void *buffer,uint32_t bytes)
{
    assert(bytes>=CIUKI_PROC_LIST_MIN+8*sizeof(struct ciuki_proc_record));
    struct ciuki_proc_system sys={.version=CIUKI_PROC_VERSION,.size=sizeof(sys),.tick_hz=1000,
        .records=(uint32_t)fake_record_count,.processes=(uint32_t)fake_record_count};
    memcpy(buffer,&sys,sizeof(sys));
    memcpy((uint8_t *)buffer+sizeof(sys),fake_records,(size_t)fake_record_count*sizeof(fake_records[0]));
    return fake_record_count;
}
static uint32_t terminated_pid; static int terminated_signal;
static int fake_terminate(uint32_t pid,int signal) { terminated_pid=pid; terminated_signal=signal; return 0; }
static void v2(struct desktop *d,int index,uint32_t pid,struct ciuki_message *m)
{ (void)d; m->data[0]=DESK_SHELL_VERSION; m->sender_pid=pid; desk_receive(d,index,m); }
static unsigned queued(const struct desktop *d,int index,unsigned opcode,struct desk_packet *out)
{
    const struct desk_client *c=&d->clients[index]; unsigned n=0;
    for (unsigned k=0;k<c->tx_count;k++) {
        const struct ciuki_message *m=&c->tx[(c->tx_head+k)%DESK_TX_MAX];
        struct desk_packet p; assert(desk_parse(m,0,NULL,&p));
        if (p.opcode==opcode) { if (out) out[n]=p; n++; }
    }
    return n;
}
static void drain(struct desktop *d,int index) { d->clients[index].tx_head=d->clients[index].tx_count=0; }
static int32_t request(struct desktop *d,int index,uint32_t pid,unsigned opcode,uint32_t a,uint32_t b,const char *name)
{
    struct ciuki_message m; unsigned n=name?(unsigned)strlen(name):0;
    desk_message(&m,opcode,opcode==DESK_END_TASK?16:opcode==DESK_LAUNCH?12+n:opcode==DESK_SWITCH_TO?12:8);
    desk_put32(m.data+4,0x1234);
    if (opcode==DESK_LAUNCH) { desk_put32(m.data+8,n); memcpy(m.data+12,name,n); }
    else { desk_put32(m.data+8,a); desk_put32(m.data+12,b); }
    if (opcode==DESK_SWITCH_TO) desk_put32(m.data+12,0);
    if (opcode==DESK_WINDOWS_QUERY) desk_put32(m.data+8,0);
    drain(d,index); v2(d,index,pid,&m);
    struct desk_packet r[DESK_TX_MAX];
    if (opcode==DESK_WINDOWS_QUERY && !queued(d,index,DESK_TASK_RESULT,r)) return 0;
    assert(queued(d,index,DESK_TASK_RESULT,r)==1 && r[0].serial==0x1234 && r[0].request==opcode);
    return r[0].result;
}
static void window(struct desktop *d,int index,uint32_t pid,int surface,const char *title)
{
    struct ciuki_message m; unsigned n=(unsigned)strlen(title);
    desk_message(&m,DESK_HELLO,4); v2(d,index,pid,&m);
    fake_surface(surface,120,80,0x123456);
    desk_message(&m,DESK_CREATE_WINDOW,16+n); m.fd_count=1; m.fds[0]=surface;
    desk_put32(m.data+4,120); desk_put32(m.data+8,80); desk_put32(m.data+12,n); memcpy(m.data+16,title,n);
    v2(d,index,pid,&m);
}
static int parses(unsigned version,unsigned opcode,unsigned length,int client,const uint32_t *words,unsigned count,const char *text,unsigned at)
{
    struct ciuki_message m; struct desk_packet p;
    desk_message(&m,opcode,length); m.data[0]=(uint8_t)version;
    for (unsigned k=0;k<count;k++) desk_put32(m.data+4+4*k,words[k]);
    if (text) memcpy(m.data+at,text,strlen(text));
    return desk_parse(&m,client,NULL,&p);
}
/* f2-33 wire rules: v2 only, direction, exact lengths, bounded titles. */
static void test_task_protocol(void)
{
    const uint32_t query[]={7};
    assert(parses(2,DESK_WINDOWS_QUERY,8,1,query,1,0,0) && !parses(1,DESK_WINDOWS_QUERY,8,1,query,1,0,0));
    assert(!parses(2,DESK_WINDOWS_QUERY,8,0,query,1,0,0) && !parses(2,DESK_WINDOWS_QUERY,12,1,query,1,0,0));
    const uint32_t info[]={7,0,1,5,100,DESK_WINDOW_FOCUSED,4};
    assert(parses(2,DESK_WINDOW_INFO,36,0,info,7,"Demo",32) && !parses(2,DESK_WINDOW_INFO,36,1,info,7,"Demo",32));
    assert(!parses(1,DESK_WINDOW_INFO,36,0,info,7,"Demo",32) && !parses(2,DESK_WINDOW_INFO,37,0,info,7,"Demo",32));
    assert(!parses(2,DESK_WINDOW_INFO,36,0,info,7,"De\tm",32));
    const uint32_t empty[]={7,0,0,0,0,0,0}, stray[]={7,0,0,5,0,0,0}, beyond[]={7,1,1,5,100,0,0},
        noid[]={7,0,1,0,100,0,0}, flags[]={7,0,1,5,100,32,0}, many[]={7,0,17,5,100,0,0};
    assert(parses(2,DESK_WINDOW_INFO,32,0,empty,7,0,0) && !parses(2,DESK_WINDOW_INFO,32,0,stray,7,0,0));
    assert(!parses(2,DESK_WINDOW_INFO,32,0,beyond,7,0,0) && !parses(2,DESK_WINDOW_INFO,32,0,noid,7,0,0));
    assert(!parses(2,DESK_WINDOW_INFO,32,0,flags,7,0,0) && !parses(2,DESK_WINDOW_INFO,32,0,many,7,0,0));
    const uint32_t end[]={7,5,0}, nothing[]={7,0,0}, pid_only[]={7,0,100};
    assert(parses(2,DESK_END_TASK,16,1,end,3,0,0) && parses(2,DESK_END_TASK,16,1,pid_only,3,0,0));
    assert(!parses(2,DESK_END_TASK,16,1,nothing,3,0,0) && !parses(2,DESK_END_TASK,16,0,end,3,0,0) && !parses(1,DESK_END_TASK,16,1,end,3,0,0));
    const uint32_t sw[]={7,5}, sw0[]={7,0};
    assert(parses(2,DESK_SWITCH_TO,12,1,sw,2,0,0) && !parses(2,DESK_SWITCH_TO,12,1,sw0,2,0,0) && !parses(2,DESK_SWITCH_TO,16,1,sw,2,0,0));
    const uint32_t launch[]={7,4}, zero[]={7,0};
    assert(parses(2,DESK_LAUNCH,16,1,launch,2,"demo",12) && !parses(2,DESK_LAUNCH,16,1,launch,2,"de o",12));
    assert(!parses(2,DESK_LAUNCH,12,1,zero,2,0,0) && !parses(2,DESK_LAUNCH,16,0,launch,2,"demo",12));
    const uint32_t result[]={7,DESK_LAUNCH,(uint32_t)-2}, odd[]={7,DESK_PING,0}, low[]={7,DESK_LAUNCH,(uint32_t)-4096};
    assert(parses(2,DESK_TASK_RESULT,16,0,result,3,0,0) && !parses(2,DESK_TASK_RESULT,16,1,result,3,0,0));
    assert(!parses(2,DESK_TASK_RESULT,16,0,odd,3,0,0) && !parses(2,DESK_TASK_RESULT,16,0,low,3,0,0));
}
/* f2-33: the Task Manager scenario through the production desktop paths. */
static void test_tasks_scenario(void)
{
    struct desktop d;
    fake_reset(); next_pair=40; desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.launch=launch_program; d.terminate=fake_terminate; d.self_pid=50; d.authority_fd=6; d.tasks_test=1;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    desk_program_add(&d,"demo",elf,sizeof(elf)); desk_program_add(&d,"tasks",elf,sizeof(elf));
    assert(d.program_count==2 && !strcmp(d.programs[1].name,"tasks"));
    expect_authority=6; expect_test=1; pid_source=300;
    int tasks=launch_program(&d,&d.programs[1]);
    assert(tasks==300 && d.clients[0].taskman && d.children[0].pid==300);
    d.tasks_pid=300; d.tasks_stage=1;                 /* main loop: menu action on "tasks" */
    window(&d,0,300,60,"Task Manager"); assert(d.tasks_stage==2 && d.clients[0].window_id==1);
    struct desk_packet r[DESK_TX_MAX];
    assert(!request(&d,0,300,DESK_WINDOWS_QUERY,0,0,0) && queued(&d,0,DESK_WINDOW_INFO,r)==1);
    assert(r[0].count==1 && r[0].window_id==1 && r[0].pid==300 && r[0].flags==DESK_WINDOW_FOCUSED && !strcmp(r[0].title,"Task Manager"));
    assert(request(&d,0,300,DESK_LAUNCH,0,0,"nothing")==-ENOENT && d.tasks_stage==2);
    expect_authority=-1; expect_test=0;
    int demo=request(&d,0,300,DESK_LAUNCH,0,0,"demo");
    assert(demo==301 && d.tasks_stage==3 && d.tasks_demo_pid==301 && !d.clients[1].taskman);
    window(&d,1,301,61,"Ciuki Demo"); assert(d.tasks_stage==4 && d.tasks_demo_window==2);
    assert(!request(&d,0,300,DESK_WINDOWS_QUERY,0,0,0) && queued(&d,0,DESK_WINDOW_INFO,r)==2);
    assert(r[0].index==0 && r[1].index==1 && r[0].count==2 && r[1].window_id==2 && r[1].pid==301);
    assert(r[1].flags==DESK_WINDOW_FOCUSED && !r[0].flags && !strcmp(r[1].title,"Ciuki Demo"));
    /* Ordinary clients hold no task-manager rights. */
    assert(request(&d,1,301,DESK_END_TASK,1,0,0)==-EPERM && request(&d,1,301,DESK_SWITCH_TO,1,0,0)==-EPERM);
    assert(request(&d,1,301,DESK_LAUNCH,0,0,"demo")==-EPERM && request(&d,1,301,DESK_WINDOWS_QUERY,0,0,0)==-EPERM);
    assert(request(&d,0,300,DESK_SWITCH_TO,1,0,0)==0 && d.focus==0);
    assert(request(&d,0,300,DESK_SWITCH_TO,99,0,0)==-ESRCH);
    desk_minimize(&d,1); assert(d.clients[1].minimized);
    assert(request(&d,0,300,DESK_SWITCH_TO,2,0,0)==0 && !d.clients[1].minimized && d.focus==1);
    assert(request(&d,0,300,DESK_END_TASK,2,999,0)==-ESRCH && d.tasks_stage==4);
    assert(request(&d,0,300,DESK_END_TASK,0,77,0)==-EPERM && request(&d,0,300,DESK_END_TASK,0,50,0)==-EPERM);
    drain(&d,1);
    assert(request(&d,0,300,DESK_END_TASK,2,301,0)==0 && d.tasks_stage==5);
    assert(d.clients[1].closing && queued(&d,1,DESK_CLOSE_REQUEST,0)==1 && d.children[1].close_ns);
    struct ciuki_message m; desk_message(&m,DESK_PING,8); desk_put32(m.data+4,DESK_TASKS_CHECKPOINT);
    v2(&d,1,301,&m); assert(!d.tasks_checkpoint);   /* only the Task Manager reports */
    desk_child_exited(&d,301,9); assert(d.tasks_stage==5); /* a killed demo cannot qualify */
    d.children[1].pid=301; desk_child_exited(&d,301,0); assert(d.tasks_stage==6 && !d.clients[1].window);
    v2(&d,0,300,&m); assert(d.tasks_checkpoint);
    fake_records[0]=(struct ciuki_proc_record){.pid=1,.state=CIUKI_PROC_LIVE};
    fake_records[1]=(struct ciuki_proc_record){.pid=50,.state=CIUKI_PROC_LIVE};
    fake_records[2]=(struct ciuki_proc_record){.pid=300,.state=CIUKI_PROC_LIVE};
    fake_records[3]=(struct ciuki_proc_record){.pid=301,.state=CIUKI_PROC_ZOMBIE};
    fake_record_count=4; assert(!tasks_verify(&d));   /* a zombie record is not gone */
    fake_record_count=3; assert(tasks_verify(&d));
    fake_records[2].state=CIUKI_PROC_STOPPING; assert(!tasks_verify(&d)); fake_records[2].state=CIUKI_PROC_LIVE;
    gate_keys=2; d.shell_keys=2; gate_report(&d,0,1,42,0,100); assert(strstr(captured_report," keys=0 "));
    d.tasks_stage=DESK_TASKS_DONE; gate_report(&d,0,1,42,0,100); assert(strstr(captured_report," keys=4 "));
    gate_keys=1; gate_report(&d,0,1,42,0,100); assert(strstr(captured_report," keys=0 "));
    /* A windowless desktop child ends through SIGTERM on the same path. */
    d.children[2].pid=302; assert(request(&d,0,300,DESK_END_TASK,0,302,0)==0 && terminated_pid==302 && terminated_signal==SIGTERM);
    assert(d.children[2].term_sent);
    /* "/bin/" names resolve to the same admitted program list. */
    int again=request(&d,0,300,DESK_LAUNCH,0,0,"/bin/demo"); assert(again==302 || again==303);
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].channel>=0 || d.clients[i].internal) desk_drop_client(&d,i);
    gate_keys=0; d.tasks_test=0;
    fake_reset();
    puts("PASS f2-33 desktop: v2 task opcodes, window list, End Task/Switch To/launch rights, authority mapping to fd 4, scenario stages, kernel list verification");
}
static void message_for(struct desktop *d,int index,uint32_t pid,unsigned opcode,unsigned length)
{
    struct ciuki_message m;
    desk_message(&m,opcode,length); m.data[0]=DESK_SHELL_VERSION; m.sender_pid=pid;
    desk_receive(d,index,&m);
}

/* f2-32: LAUNCH carries an optional single argument (bytes 32..255 except DEL, 1..DESK_ARG_MAX). */
static int launch_parses(const char *name,const void *arg,unsigned arg_length,unsigned declared,int total_extra)
{
    struct ciuki_message m; struct desk_packet p; unsigned n=(unsigned)strlen(name);
    unsigned length=12+n+(declared||arg_length||total_extra ? 4+arg_length+(unsigned)total_extra : 0);
    desk_message(&m,DESK_LAUNCH,length); m.data[0]=DESK_SHELL_VERSION;
    desk_put32(m.data+4,9); desk_put32(m.data+8,n); memcpy(m.data+12,name,n);
    if (length>12+n) { desk_put32(m.data+12+n,declared); memcpy(m.data+16+n,arg,arg_length); }
    int ok=desk_parse(&m,1,NULL,&p);
    if (ok && length>12+n) assert(!strncmp(p.argument,arg,arg_length) && !p.argument[arg_length]);
    if (ok && length==12+n) assert(!p.argument[0]);
    return ok;
}
static void test_launch_protocol(void)
{
    assert(launch_parses("ciuknote","",0,0,0) && launch_parses("ciuknote","/a b/c.txt",10,10,0));
    assert(launch_parses("ciuknote","\xc3\xa9.txt",6,6,0) && !launch_parses("ciuknote","x",1,2,0) && !launch_parses("ciuknote","xy",2,1,0));
    assert(!launch_parses("ciuknote","",0,0,1) && !launch_parses("ciuknote","x",1,0,0) && !launch_parses("ciuknote","a\tb",3,3,0) && !launch_parses("ciuknote","a\x7f",2,2,0));
    char big[DESK_ARG_MAX+8]; memset(big,'a',sizeof(big));
    assert(launch_parses("ciuknote",big,DESK_ARG_MAX,DESK_ARG_MAX,0) && !launch_parses("ciuknote",big,DESK_ARG_MAX+1,DESK_ARG_MAX+1,0));
    char name63[64]; memset(name63,'n',63); name63[63]=0;     /* the longest name still fits with the longest argument */
    assert(launch_parses(name63,big,DESK_ARG_MAX,DESK_ARG_MAX,0));
    /* A trailing byte after the argument, or a length cut short, never parses. */
    struct ciuki_message m; struct desk_packet p;
    desk_message(&m,DESK_LAUNCH,12+8+4+3); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+4,1); desk_put32(m.data+8,8);
    memcpy(m.data+12,"ciuknote",8); desk_put32(m.data+20,3); memcpy(m.data+24,"/ab",3); assert(desk_parse(&m,1,NULL,&p) && !strcmp(p.argument,"/ab"));
    m.length--; m.data[26]=0; assert(!desk_parse(&m,1,NULL,&p));
}
/* f2-32: LAUNCH from Files, and the CiukNote app scenario, through the production desktop paths. */
static int32_t launch_request(struct desktop *d,int index,uint32_t pid,const char *name,const char *arg)
{
    struct ciuki_message m; unsigned n=(unsigned)strlen(name), a=arg?(unsigned)strlen(arg):0;
    desk_message(&m,DESK_LAUNCH,12+n+(a?4+a:0));
    desk_put32(m.data+4,0x4321); desk_put32(m.data+8,n); memcpy(m.data+12,name,n);
    if (a) { desk_put32(m.data+12+n,a); memcpy(m.data+16+n,arg,a); }
    drain(d,index); v2(d,index,pid,&m);
    struct desk_packet r[DESK_TX_MAX];
    assert(queued(d,index,DESK_TASK_RESULT,r)==1 && r[0].serial==0x4321 && r[0].request==DESK_LAUNCH);
    return r[0].result;
}
static void test_note_scenario(void)
{
    struct desktop d;
    fake_reset(); next_pair=60; pid_source=400; desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.launch=launch_program; d.terminate=fake_terminate; d.self_pid=50; d.authority_fd=-1;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    desk_program_add(&d,"tasks",elf,sizeof(elf)); desk_program_add(&d,"hello",elf,sizeof(elf));
    desk_program_add(&d,"files",elf,sizeof(elf)); desk_program_add(&d,"ciuknote",elf,sizeof(elf));
    assert(d.program_count==4 && !strcmp(d.programs[0].name,"ciuknote") && !strcmp(d.programs[1].name,"files"));
    assert(desk_program_graphical("ciuknote") && !strcmp(desk_program_label("ciuknote"),"CiukNote"));
    assert(desk_program_kind("ciuknote")==DESK_APP_NOTE && desk_program_kind("files")==DESK_APP_FILES && !desk_program_kind("tasks"));
    assert(!strcmp(desk_app_program(DESK_APP_NOTE),"ciuknote") && !strcmp(desk_app_program(DESK_APP_FILES),"files"));
    /* Launched from the menu: the desktop passes only the channel; the taskbar knows it as CiukNote. */
    int menu=launch_program(&d,&d.programs[0]); assert(menu==400 && d.clients[0].program==DESK_APP_NOTE && d.clients[0].pid==400);
    drop_all(&d); fake_reset(); next_pair=70; desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d); d.launch=launch_program; d.terminate=fake_terminate; d.authority_fd=-1;
    desk_program_add(&d,"tasks",elf,sizeof(elf)); desk_program_add(&d,"hello",elf,sizeof(elf));
    desk_program_add(&d,"files",elf,sizeof(elf)); desk_program_add(&d,"ciuknote",elf,sizeof(elf));
    /* Files, launched by the desktop, may LAUNCH CiukNote with one absolute path, and nothing else. */
    expect_files_test=0; pid_source=410;
    int files=launch_program(&d,&d.programs[1]); assert(files==410 && d.clients[0].program==DESK_APP_FILES);
    window(&d,0,410,100,"Files");
    expect_note_arg="/system/tests/a file.txt";
    int note=(int)launch_request(&d,0,410,"ciuknote","/system/tests/a file.txt");
    assert(note==411 && d.clients[1].program==DESK_APP_NOTE && !d.clients[1].taskman && !d.launch_arg && d.children[1].pid==411);
    expect_note_arg=NULL;
    assert(launch_request(&d,0,410,"ciuknote",NULL)==-EPERM && launch_request(&d,0,410,"ciuknote","relative.txt")==-EPERM);
    assert(launch_request(&d,0,410,"hello","/system/tests/a.txt")==-EPERM && launch_request(&d,0,410,"tasks","/x")==-EPERM);
    assert(launch_request(&d,0,410,"files","/x")==-EPERM && launch_request(&d,0,410,"nothing","/x")==-EPERM);
    window(&d,1,411,101,"CiukNote - a file.txt");
    assert(launch_request(&d,1,411,"ciuknote","/x")==-EPERM);             /* CiukNote itself holds no rights */
    assert(request(&d,0,410,DESK_END_TASK,1,0,0)==-EPERM && request(&d,0,410,DESK_SWITCH_TO,1,0,0)==-EPERM);
    assert(request(&d,0,410,DESK_WINDOWS_QUERY,0,0,0)==-EPERM);            /* no task-manager right came with it */
    int hello=launch_program(&d,&d.programs[2]); assert(hello>0 && d.clients[2].program==0);
    window(&d,2,(uint32_t)hello,102,"Hello");
    assert(launch_request(&d,2,(uint32_t)hello,"ciuknote","/system/tests/a.txt")==-EPERM);   /* an ordinary client is refused */
    /* The Task Manager keeps its rights, but cannot pass arguments. */
    int tasks=launch_program(&d,&d.programs[3]); assert(tasks>0 && d.clients[3].taskman);
    window(&d,3,(uint32_t)tasks,103,"Task Manager");
    assert(launch_request(&d,3,(uint32_t)tasks,"ciuknote","/system/tests/a.txt")==-EPERM);
    assert(launch_request(&d,3,(uint32_t)tasks,"ciuknote",NULL)>0 && !d.launch_arg);
    /* A failed spawn reports its error and leaves nothing behind. */
    fail_spawn=1; expect_note_arg="/system/tests/b.txt"; assert(launch_request(&d,0,410,"ciuknote","/system/tests/b.txt")==-ENOEXEC && !d.launch_arg); fail_spawn=0; expect_note_arg=NULL;
    drop_all(&d); fake_reset();

    /* The CiukNote scenario: --test=ciuknote, three ordered checkpoints, a fixed clock and the key edges after exit. */
    next_pair=90; pid_source=500;
    struct desk_program note_program={"ciuknote","/bin/ciuknote"};
    desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    d.app_test=DESK_APP_NOTE; expect_note_test=1; expect_note_arg=NULL;
    int pid=launch_program(&d,&note_program); assert(pid==500 && d.clients[0].program==DESK_APP_NOTE);
    d.app_pid=(uint32_t)pid; d.shell_keys=2;
    struct ciuki_message m;
    message_for(&d,0,(uint32_t)pid,DESK_HELLO,4);
    for (unsigned serial=2; serial>0; serial--) {                      /* out of order: ignored */
        desk_message(&m,DESK_PING,8); m.data[0]=2; m.sender_pid=(uint32_t)pid; desk_put32(m.data+4,DESK_NOTE_CHECKPOINT+serial); desk_receive(&d,0,&m);
    }
    assert(d.app_checkpoint==1);
    desk_message(&m,DESK_PING,8); m.data[0]=2; m.sender_pid=(uint32_t)pid; desk_put32(m.data+4,DESK_FILES_CHECKPOINT+2); desk_receive(&d,0,&m);
    assert(d.app_checkpoint==1);                                       /* another scenario's serial range is not counted */
    for (unsigned serial=2; serial<=4; serial++) {
        desk_message(&m,DESK_PING,8); m.data[0]=2; m.sender_pid=(uint32_t)pid; desk_put32(m.data+4,DESK_NOTE_CHECKPOINT+serial); desk_receive(&d,0,&m);
    }
    assert(d.app_checkpoint==DESK_NOTE_CHECKPOINTS);
    desk_message(&m,DESK_PING,8); m.data[0]=2; m.sender_pid=(uint32_t)pid; desk_put32(m.data+4,DESK_NOTE_CHECKPOINT+4); desk_receive(&d,0,&m);
    assert(d.app_checkpoint==DESK_NOTE_CHECKPOINTS);                   /* no checkpoint past the last */
    gate_keys=2; d.app_reported=3; gate_report(&d,0,1,42,0,100);
    assert(strstr(captured_report," keys=0 ") && strstr(captured_report," note=1 checkpoint=3") && !strstr(captured_report," files=1"));
    desk_child_exited(&d,(uint32_t)pid,1); assert(!d.app_done);
    d.children[0].pid=(uint32_t)pid; desk_child_exited(&d,(uint32_t)pid,0); assert(d.app_done);
    gate_report(&d,0,1,42,0,100); assert(strstr(captured_report," keys=4 "));
    /* The A-key count waits for the last checkpoint. */
    gate_keys=0; d.app_checkpoint=2;
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=0x1e,.value=1}; gate_input(&d,&e); assert(!gate_keys);
    d.app_checkpoint=3; gate_input(&d,&e); assert(gate_keys==1);
    gate_keys=0; expect_note_test=0;
    drop_all(&d); fake_reset();
    puts("PASS f2-32 desktop: LAUNCH argument wire rules, Files-only CiukNote launch with a path, rights unchanged for others, CiukNote scenario");
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
    d.shell_test=1; d.mouse_x=40; d.mouse_y=16;   /* the Ciuki button in the top bar */
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
    desk_menu_close(&d);   /* as a menu launch does (shell.c activate): the menu drops over the demo's title now */
    assert(!d.menu_open && d.focus==0);
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

    /* f2-31: Files is a graphical program with its own menu label/icon. */
    assert(desk_program_graphical("files") && desk_program_graphical("demo") && !desk_program_graphical("hello"));
    assert(!strcmp(desk_program_label("files"),"Files") && !strcmp(desk_program_label("lua"),"lua"));
    desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    struct desk_program hello={"hello","/bin/hello"}, files={"files","/bin/files"};
    assert(launch_program(&d,&hello)>0 && !d.clients[0].program);
    drop_all(&d); fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    pid=launch_program(&d,&files); assert(pid>0 && d.clients[0].program==1);
    /* Files test mode: --test=files, ordered checkpoint PINGs, keys after exit. */
    drop_all(&d); fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    d.app_test=DESK_APP_FILES; expect_files_test=1;
    pid=launch_program(&d,&files); assert(pid>0);
    d.app_pid=(uint32_t)pid; d.shell_pid=(uint32_t)pid; d.shell_keys=2;
    message(&d,0,DESK_HELLO,4);
    for (unsigned serial=2; serial>0; serial--) {          /* out of order: ignored */
        desk_message(&m,DESK_PING,8); m.data[0]=2; m.sender_pid=(uint32_t)pid; desk_put32(m.data+4,DESK_FILES_CHECKPOINT+serial);
        desk_receive(&d,0,&m);
    }
    assert(d.app_checkpoint==1);
    for (unsigned serial=2; serial<=6; serial++) {
        desk_message(&m,DESK_PING,8); m.data[0]=2; m.sender_pid=(uint32_t)pid; desk_put32(m.data+4,DESK_FILES_CHECKPOINT+serial);
        desk_receive(&d,0,&m);
    }
    assert(d.app_checkpoint==DESK_FILES_CHECKPOINTS);
    gate_keys=2; d.app_reported=5; gate_report(&d,0,1,42,0,100);
    assert(strstr(captured_report," keys=0 ") && strstr(captured_report," files=1 checkpoint=5"));
    desk_child_exited(&d,(uint32_t)pid,1); assert(!d.app_done);
    d.children[0].pid=(uint32_t)pid; desk_child_exited(&d,(uint32_t)pid,0); assert(d.app_done);
    gate_report(&d,0,1,42,0,100); assert(strstr(captured_report," keys=4 "));
    drop_all(&d); fake_reset();
    puts("PASS production launcher: explicit channel-only inheritance/group, CLOEXEC/spawn rollback, PID binding, menu/drag/close/reap evidence, capacity, Files launch/checkpoints");
    test_task_protocol();
    test_tasks_scenario();
    test_launch_protocol();
    test_note_scenario();
    return 0;
}
