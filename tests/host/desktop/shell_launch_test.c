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
#include <time.h>
#include <fcntl.h>
/* Keep the cfg_writer.write member out of the write() syscall macro below. */
#include "../../../apps/desktop/settings_io.h"
#define CIUKI_RUNTIME_H
#define CIUKI_RAW_H
#define CU_PTR(p) ((uintptr_t)(p))
static int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }
uint32_t ciuki_raw_probe_report(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
int shell_close(int);
int shell_open(const char *, int, ...);
int shell_fcntl(int, int, ...);
static int shell_clock_gettime(clockid_t, struct timespec *);
#define clock_gettime shell_clock_gettime
struct stat;
int shell_fstat(int, struct stat *);
static char launch_record[512];
static ssize_t shell_launch_write(int fd,const void *bytes,size_t size)
{
    if (fd==STDERR_FILENO && size>9 && !memcmp(bytes,"[launch] ",9)) {
        assert(size<sizeof(launch_record));memcpy(launch_record,bytes,size);launch_record[size]=0;
        errno=EPIPE;return -1; /* diagnostics must preserve the launch errno */
    }
    return write(fd,bytes,size);
}
#define fcntl shell_fcntl
#define close shell_close
#define open(...) shell_open(__VA_ARGS__)
#define fstat shell_fstat
/* Parse the writer's field names before redirecting the POSIX write call. */
#include "../../../apps/desktop/settings_io.h"
#define write shell_launch_write
#ifdef DESK_FOOTPRINT_TEST
#include "../../../apps/desktop/sound_storage.h"
static const char *footprint_registry_directory;
static int footprint_master_save(const char *,unsigned,unsigned,unsigned);
#define master_preference_save footprint_master_save
#endif
static unsigned refuse_program_scan;
static DIR *scan_opendir(const char *path)
{if(refuse_program_scan && !strcmp(path,"/Programs")){errno=ENOENT;return NULL;}return opendir(path);}
#define opendir scan_opendir
#define main desktop_main
#ifdef DESK_FOOTPRINT_TEST
static const char *sequence_settings_path;
static unsigned sequence_main_reads;
static const char *boot_cache_directory;
#include "../../../apps/desktop/sound_runtime.h"
static void boot_wall_stage(const char *);
static const struct fm_vfs *boot_native_vfs(void);
static int boot_cfg_writer_start(struct cfg_writer *w,const char *path,const char *temporary)
{
    return cfg_writer_start(w,boot_cache_directory ? sequence_settings_path : path,
                            boot_cache_directory ? NULL : temporary);
}
#include "../../../apps/desktop/wallpaper_io.h"
static int boot_wall_worker_start(struct desk_wall_worker *w,const char *dir,void (*stage)(const char *))
{ return desk_wall_worker_start(w,boot_cache_directory ? boot_cache_directory : dir,
                               boot_cache_directory ? boot_wall_stage : stage); }
static void boot_sound_init(struct desktop *d)
{ if (!boot_cache_directory) desk_sound_init(d); }
static int boot_wall_preload(const struct desk_wall_request *r,const char *dir,uint32_t **out,void (*stage)(const char *))
{ return desk_wall_preload(r,boot_cache_directory ? boot_cache_directory : dir,out,boot_cache_directory ? boot_wall_stage : stage); }
#define desk_wall_preload boot_wall_preload
#define cfg_writer_start boot_cfg_writer_start
#define desk_wall_worker_start boot_wall_worker_start
#define fm_native_vfs boot_native_vfs
#define desk_sound_init boot_sound_init
static FILE *sequence_fopen(const char *path,const char *mode)
{
    if (sequence_settings_path && !strcmp(path,CFG_FILE)) {
        sequence_main_reads++;
        return fopen(sequence_settings_path,mode);
    }
    return fopen(path,mode);
}
#define fopen sequence_fopen
#endif
#include "../../../apps/desktop/desktop.c"
#undef write
#undef opendir
#ifdef DESK_FOOTPRINT_TEST
#undef fopen
#undef cfg_writer_start
#undef desk_wall_worker_start
#undef fm_native_vfs
#undef desk_sound_init
#undef master_preference_save
static unsigned footprint_master_saves;
int ciuki_devices(struct ciuki_devices_info *info)
{ (void)info; errno=ENODEV; return -1; }
static int footprint_master_save(const char *directory,unsigned left,unsigned right,unsigned mute)
{
    assert(!strcmp(directory,REG_DIRECTORY));
    assert(pthread_equal(pthread_self(),settings_writer.thread)); footprint_master_saves++;
    return master_preference_save(footprint_registry_directory,left,right,mute);
}
#endif
#undef main
#undef close
#undef open
#undef fcntl
#undef clock_gettime
#undef fstat
static int timing_model, trace_verbose;
static uint64_t model_us;
static struct {
    uint64_t click_us, spawn_us, elf_us, configure_us, damage_us, frame_us;
    uint64_t bytes;
    unsigned image_requests, frames;
} launch_timing;
static unsigned model_elf_bytes, model_batch=2048;
static int shell_clock_gettime(clockid_t clock, struct timespec *t)
{
    if (!timing_model) return clock_gettime(clock,t);
    assert(clock==CLOCK_MONOTONIC);
    t->tv_sec=(time_t)(model_us/1000000); t->tv_nsec=(long)(model_us%1000000)*1000;
    return 0;
}
int ciuki_launch_mark(uint32_t event, ciuki_pid_t pid, uint64_t click_us)
{
    if (event==CIUKI_LAUNCH_STATUS) return trace_verbose;
    assert(trace_verbose && timing_model);
    if (event==CIUKI_LAUNCH_BEGIN) { assert(!pid); launch_timing.click_us=click_us; }
    else {
        assert(pid>0 && !click_us);
        if (event==CIUKI_LAUNCH_CONFIGURE) launch_timing.configure_us=model_us;
        else if (event==CIUKI_LAUNCH_DAMAGE) launch_timing.damage_us=model_us;
        else { assert(event==CIUKI_LAUNCH_FRAME); launch_timing.frame_us=model_us; launch_timing.frames++; }
    }
    return 0;
}
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
static int media_available, media_error, media_query_error;
static struct ciuki_media_state media_fixture;
static struct ciuki_media_volume media_volumes[26];
#define MEDIA_FD 1002
static int network_available, network_error;
static unsigned network_opens, network_closes, network_queries;
static struct ciuki_net_state network_fixture;
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
#define MIXER_FD 1001
static struct ciuki_mixer_master mixer_fixture;
static unsigned mixer_queries,mixer_sets,mixer_refreshes;
static int mixer_available, mixer_error;
#endif
static int media_volume_error;
static int media_slow;
static unsigned media_slow_queries;
static pthread_t media_frame_thread;
#ifdef DESK_POLL_LEDGER_TEST
void desktop_poll_host_init(void);
void desktop_poll_host_open(unsigned);
void desktop_poll_host_close(unsigned);
void desktop_poll_host_baseline(void);
void desktop_poll_host_leak(int);
void desktop_poll_host_check(int);
void desktop_poll_host_finish(void);
static int poll_bridge, poll_snapshot, poll_equal;
#define POLL_OPEN(kind) do { if (poll_bridge) desktop_poll_host_open(kind); } while (0)
#define POLL_CLOSE(kind) do { if (poll_bridge) desktop_poll_host_close(kind); } while (0)
#else
#define POLL_OPEN(kind) ((void)0)
#define POLL_CLOSE(kind) ((void)0)
#endif
#define NETWORK_FD 1000
int shell_open(const char *path, int flags, ...)
{
    if (media_available && !strcmp(path,"/dev/media")) {
        assert(flags==(O_RDONLY|O_CLOEXEC)); POLL_OPEN(2); return MEDIA_FD;
    }
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
    if (mixer_available && !strcmp(path,"/dev/mixer")) {
        assert(flags==(O_RDONLY|O_CLOEXEC)); POLL_OPEN(1); return MIXER_FD;
    }
#endif
    if (!strcmp(path, "/dev/net")) {
        assert(flags == (O_RDONLY | O_CLOEXEC)); network_opens++;
        if (network_available) { POLL_OPEN(0); return NETWORK_FD; }
        errno = ENODEV; return -1;
    }
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
        return open(path, flags, mode);
    }
    return open(path, flags);
}
int shell_close(int fd)
{
    if (fd==MEDIA_FD) { POLL_CLOSE(2); return 0; }
    if (fd==NETWORK_FD) { POLL_CLOSE(0); network_closes++; return 0; }
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
    if (fd==MIXER_FD) { POLL_CLOSE(1); return 0; }
#endif
    return fake_ops.close(fd);
}
static int shell_regular_fd=-1, shell_directory_fd=-1;
int shell_fstat(int fd, struct stat *st)
{
    memset(st,0,sizeof(*st));
    if (fd==shell_regular_fd) { st->st_mode=S_IFREG; return 0; }
    if (fd==shell_directory_fd) { st->st_mode=S_IFDIR; return 0; }
    errno=EBADF; return -1;
}
static int expect_files_test, expect_note_test, expect_authority=-1, expect_test=0;
static const char *expect_note_arg, *expect_dosbox[2], *expect_control_arg, *expect_netsurf[2];
static unsigned dosbox_spawns, netsurf_spawns;
static unsigned control_spawns;
/* fx-8: a slow spawn. With spawn_hold_ms set the call blocks until the test
 * releases it or the hold expires (the T23 trial's NetSurf: 3891 ms). */
static pthread_mutex_t spawn_hold_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t spawn_hold_changed=PTHREAD_COND_INITIALIZER;
static unsigned spawn_hold_ms, spawn_released;
static unsigned spawn_entered __attribute__((unused)); /* read only by launch_async_test.h */
static pthread_t spawn_thread __attribute__((unused));
static void spawn_hold(void)
{
    pthread_mutex_lock(&spawn_hold_lock);
    spawn_thread=pthread_self(); spawn_entered++;
    pthread_cond_broadcast(&spawn_hold_changed);
    if (spawn_hold_ms) {
        struct timespec until; clock_gettime(CLOCK_REALTIME,&until);
        until.tv_sec+=spawn_hold_ms/1000; until.tv_nsec+=(long)(spawn_hold_ms%1000)*1000000L;
        if (until.tv_nsec>=1000000000L) { until.tv_sec++; until.tv_nsec-=1000000000L; }
        while (!spawn_released && pthread_cond_timedwait(&spawn_hold_changed,&spawn_hold_lock,&until)!=ETIMEDOUT) {}
    }
    pthread_mutex_unlock(&spawn_hold_lock);
}
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],
                      const struct ciuki_spawn_fd *mapping,uint32_t count,uint32_t flags)
{
    spawn_hold();
    assert(!strcmp(env[0],"LANG=en_US") && !strcmp(env[1],"TZ=UTC0") && !strncmp(env[5],"CIUKI_DISPLAY=",14) && !strcmp(env[6],"CIUKI_KEYMAP=us") && !env[7]);
    assert(!strcmp(env[2],"HOME=/Documents") && !strcmp(env[3],"TMPDIR=/tmp") && !strcmp(env[4],"PATH=/Programs"));
    assert(count>=1 && mapping[0].source==last_child && mapping[0].target==3 && !flags);
    if (!strcmp(path,"/Programs/files")) {
        assert(!strcmp(argv[0],"files") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        assert(expect_files_test ? argv[2] && !strcmp(argv[2],"--test=files") && !argv[3] : !argv[2]);
    } else if (!strcmp(path,"/Programs/ciuknote")) {
        /* f2-32: --test=ciuknote only in its scenario; the path argument only through LAUNCH from Files. */
        assert(!strcmp(argv[0],"ciuknote") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        unsigned at=2;
        if (expect_note_test) assert(!strcmp(argv[at++],"--test=ciuknote"));
        if (expect_note_arg) assert(!strcmp(argv[at++],expect_note_arg));
        assert(!argv[at]);
    } else if (!strcmp(path,"/Programs/dosbox")) {
        /* Programs > Games: the window title, then the command dosbox types at the DOS prompt. */
        assert(!strcmp(argv[0],"dosbox") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        unsigned at=2;
        for (unsigned i=0;i<2 && expect_dosbox[i];i++) assert(argv[at] && !strcmp(argv[at++],expect_dosbox[i]));
        assert(!argv[at]);
        dosbox_spawns++;
    } else if (!strcmp(path,"/Programs/netsurf")) {
        /* f6-19: NetSurf's program record grants the window channel on every
         * route; the GATE_BROWSER route only appends --test=browser <url>. */
        assert(!strcmp(argv[0],"netsurf") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        unsigned at=2;
        for (unsigned i=0;i<2 && expect_netsurf[i];i++) assert(argv[at] && !strcmp(argv[at++],expect_netsurf[i]));
        assert(!argv[at]);
        netsurf_spawns++;
    } else if (!strcmp(path,"/Programs/hello")) assert(!strcmp(argv[0],"hello") && !argv[1] && count==1);
    else if (!strcmp(path,"/Programs/music")) assert(!strcmp(argv[0],"music") && !strcmp(argv[1],"--channel-fd=3") && !argv[2] && count==1);
    else if (!strcmp(path,"/Programs/tasks")) {
        /* f2-33: only the Task Manager receives the authority, at fd 4. */
        assert(!strcmp(argv[0],"tasks") && !strcmp(argv[1],"--channel-fd=3"));
        unsigned at=2;
        if (expect_authority>=0) {
            assert(count==2 && mapping[1].source==expect_authority && mapping[1].target==4);
            assert(!strcmp(argv[at++],"--authority-fd=4"));
        } else assert(count==1);
        if (expect_test) assert(!strcmp(argv[at++],"--test=tasks"));
        assert(!argv[at]);
    } else if (!strcmp(path,"/Programs/control")) {
        assert(!strcmp(argv[0],"control") && !strcmp(argv[1],"--channel-fd=3") && count==1);
        if (expect_control_arg) assert(argv[2] && !strcmp(argv[2],expect_control_arg) && !argv[3]);
        else assert(!argv[2]);
        control_spawns++;
    }
    else assert(!strcmp(path,"/Programs/demo") && !strcmp(argv[0],"demo") && !strcmp(argv[1],"--channel-fd=3") && !argv[2] && count==1);
    if (fail_spawn) { errno=fail_spawn==1 ? ENOEXEC : fail_spawn; return -1; }
    if (timing_model) {
        launch_timing.spawn_us=model_us; launch_timing.bytes=model_elf_bytes;
        launch_timing.image_requests=(model_elf_bytes+model_batch-1)/model_batch;
        /* Explicit fixture model, not T23 calibration: 2 MiB/s disk and
         * 6 us per loader request. The kernel harness verifies real batches. */
        model_us+=(uint64_t)model_elf_bytes*1000000/(2u*1024u*1024u)+launch_timing.image_requests*6;
        launch_timing.elf_us=model_us;
    }
    return pid_source++;
}
static void drop_all(struct desktop *d)
{ for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].channel>=0) desk_drop_client(d,i); }
static struct ciuki_proc_record fake_records[8];
static int fake_record_count;
/* Desktop shell services with explicit device boundaries. */
int ciuki_devctl(int fd,uint32_t op,void *record,uint32_t size)
{
#ifdef DESK_POLL_LEDGER_TEST
    if (poll_snapshot) desktop_poll_host_check(poll_equal);
#endif
    if (fd==MEDIA_FD) {
        if (media_slow) {
            assert(!pthread_equal(pthread_self(),media_frame_thread));
            __atomic_add_fetch(&media_slow_queries,1,__ATOMIC_RELAXED);
            struct timespec pause={0,25000000};nanosleep(&pause,NULL);
        }
        if (op==MEDIA_GET_STATE) {
            assert(size==sizeof(media_fixture));
            if (media_query_error) { errno=EIO;return -1; }
            memcpy(record,&media_fixture,size);return 0;
        }
        if (op==MEDIA_GET_VOLUME) {
            if (media_volume_error) { errno=EIO; return -1; }
            struct ciuki_media_volume *v=record;
            assert(size==sizeof(*v) && v->drive<26);
            *v=media_volumes[v->drive]; return 0;
        }
        if (op==MEDIA_CD_STATE) {
            struct ciuki_media_cd_state *cd=record;
            assert(size==sizeof(*cd) && cd->version==CIUKI_MEDIA_CD_VERSION &&
                   cd->size==sizeof(*cd) && cd->slot<4);
            assert(!cd->present && !cd->state && !cd->generation && !cd->audio_ready &&
                   !cd->mounted && !cd->drive && !cd->reserved[0] && !cd->reserved[1] && !cd->reserved[2]);
            /* This USB fixture has no ATAPI drive bound to any optical slot. */
            cd->state=CIUKI_CD_NO_MEDIUM;
            cd->drive=26;
            return 0;
        }
        if (op==MEDIA_GET_BLOCKERS) {
            struct ciuki_media_blockers *b=record;assert(size==sizeof(*b));
            b->count=1;b->programs[0].pid=42;strcpy(b->programs[0].name,"ciuknote");return 0;
        }
        assert(op==MEDIA_SAFE_REMOVE && size==sizeof(struct ciuki_media_remove));
        model_us+=10000000; /* a real eject may take ten seconds */
        if (media_error) { errno=media_error;return -1; }
        media_fixture.removable_mask=0;media_fixture.type[4]=CIUKI_MEDIA_SAFE_REMOVED;return 0;
    }
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
    if (fd==MIXER_FD) {
        if (mixer_error) { errno=EIO; return -1; }
        assert(size==sizeof(mixer_fixture));
        if (op==MIXER_GET_MASTER || op==MIXER_REFRESH_MASTER) {
            mixer_queries++; mixer_refreshes+=op==MIXER_REFRESH_MASTER; memcpy(record,&mixer_fixture,size);
        }
        else { assert(op==MIXER_SET_MASTER); mixer_sets++; memcpy(&mixer_fixture,record,size); }
        return 0;
    }
#endif
    if (fd == NETWORK_FD) {
        assert(op == NET_GET_STATE && size == sizeof(network_fixture));
        struct ciuki_net_state *n = record;
        assert(n->version == CIUKI_NET_VERSION && n->size == sizeof(*n));
        network_queries++;
        if (!network_error) { *n = network_fixture; return 0; }
    }
    errno=ENODEV; return -1;
}
#include "media_worker_test.h"
/* Existing device-state assertions wait at observation boundaries; the
 * production shell_service and the slow-media test keep nonblocking turns. */
static int media_wait_read(struct desktop *d)
{
    media_worker.rescan=1;
    if (!media_read(d)) return 0;
    for (unsigned i=0;i<5000;i++) {
        pthread_mutex_lock(&media_worker.lock);
        int ready=media_worker.ready && media_worker.completed>=media_worker.requested;
        int ok=media_worker.ok;
        pthread_mutex_unlock(&media_worker.lock);
        if (ready) { media_poll(d); return ok; }
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    assert(!"media worker timeout");return 0;
}
#define media_read media_wait_read
static uint32_t last_power_action;
int ciuki_power(uint32_t action) { last_power_action=action; errno=ENOSYS; return -1; }
int ciuki_power_status(struct ciuki_power_status *out) { (void)out; errno=ENOSYS; return -1; }
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
static int last_authority;
int ciuki_proc_terminate(ciuki_pid_t pid, int signo, int authority_fd)
{ last_authority=authority_fd; return fake_terminate((uint32_t)pid,signo); }
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
static unsigned display_calls; static uint16_t display_mode; static int display_result;
static int display_set(struct desktop *d, uint16_t mode)
{ (void)d; display_calls++; display_mode = mode; return display_result; }
static unsigned sound_calls, sound_request, sound_event;
static int sound_settings(struct desktop *d, int client, unsigned request, unsigned event, uint32_t serial)
{
    (void)d; assert(client == 0 && serial == 42);
    sound_calls++; sound_request = request; sound_event = event; return 0;
}
static unsigned install_requests;
static int install_fixture(struct desktop *d,const char *path)
{(void)d;assert(!strcmp(path,"/mnt/d/SETUP.EXE"));install_requests++;return 0;}
/* Each scenario owns a struct desktop (~600 KiB): keep frames out of main. */
__attribute__((noinline)) static void test_display_settings(void)
{
    struct desktop d; struct ciuki_message m; struct desk_packet reply[DESK_TX_MAX];
    fake_reset(); desk_init(&d, &fake_ops, 1024, 768); desk_shell_enable(&d);
    fake_channel(60); assert(desk_add_client(&d, 60) == 0); window(&d, 0, 500, 70, "Control Panel");
    d.display_set = display_set; display_calls = 0; display_result = 0;
    d.sound_request = sound_settings; sound_calls = 0;
    desk_message(&m, DESK_SETTINGS, 24); desk_put32(m.data + 4, 42);
    desk_put32(m.data + 8, DESK_SETTINGS_DISPLAY); desk_put32(m.data + 12, 0x118);
    drain(&d, 0); v2(&d, 0, 500, &m); /* ordinary client has no display authority */
    assert(queued(&d, 0, DESK_TASK_RESULT, reply) == 1 && reply[0].result == -EPERM && !display_calls);
    d.clients[0].program = DESK_APP_CONTROL;
    drain(&d, 0); v2(&d, 0, 500, &m);
    assert(queued(&d, 0, DESK_TASK_RESULT, reply) == 1 && !reply[0].result && reply[0].serial == 42 && reply[0].request == DESK_SETTINGS);
    assert(display_calls == 1 && display_mode == 0x118 && !d.sh.want_save && !d.sh.want_reload && !d.sh.cfg.display_mode && !sound_calls);
    display_result = -EBUSY; drain(&d, 0); v2(&d, 0, 500, &m);
    assert(queued(&d, 0, DESK_TASK_RESULT, reply) == 1 && reply[0].result == -EBUSY && display_calls == 2);
    d.display_set = NULL; drain(&d, 0); v2(&d, 0, 500, &m);
    assert(queued(&d, 0, DESK_TASK_RESULT, reply) == 1 && reply[0].result == -ENOSYS && display_calls == 2);
    d.display_set = display_set;
    desk_put32(m.data + 20, 0);
    const unsigned requests[] = {DESK_SETTINGS_SOUND_RELOAD, DESK_SETTINGS_SOUND_TEST, DESK_SETTINGS_AUDIO_TEST};
    for (unsigned k = 0; k < sizeof(requests)/sizeof(*requests); k++) {
        desk_put32(m.data + 8, requests[k]);
        desk_put32(m.data + 12, requests[k] == DESK_SETTINGS_SOUND_TEST ? SOUND_ERROR : 0);
        drain(&d, 0); v2(&d, 0, 500, &m);
        assert(queued(&d, 0, DESK_TASK_RESULT, reply) == 1 && !reply[0].result && reply[0].serial == 42);
        assert(sound_calls == k + 1 && sound_request == requests[k] && display_calls == 2);
        assert(sound_event == (requests[k] == DESK_SETTINGS_SOUND_TEST ? SOUND_ERROR : 0));
    }
    d.power.state = DESK_POWER_CLOSING; drain(&d, 0); v2(&d, 0, 500, &m);
    assert(queued(&d, 0, DESK_TASK_RESULT, reply) == 1 && reply[0].result == -EBUSY && sound_calls == 3);
    d.power.state = DESK_POWER_IDLE;
    desk_message(&m,DESK_SETTINGS,24);desk_put32(m.data+4,42);desk_put32(m.data+8,DESK_SETTINGS_PROGRAMS);
    d.sh.want_reload=0;d.v3_links_next=123;
    d.clients[0].program=0;drain(&d,0);v2(&d,0,500,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && reply[0].result==-EPERM && !d.sh.want_reload);
    d.clients[0].program=DESK_APP_CONTROL;drain(&d,0);v2(&d,0,500,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && !reply[0].result && d.sh.want_reload==3 && d.sh.want_programs && !d.v3_links_next);
    d.sh.want_reload=0;d.sh.want_wallpaper=0;d.program_count=1;
    refuse_program_scan=1;shell_service(&d);refuse_program_scan=0;
    assert(!d.program_count && !d.sh.want_programs && !d.sh.want_wallpaper);
    const char *installer="/mnt/d/SETUP.EXE";unsigned length=(unsigned)strlen(installer);
    desk_message(&m,DESK_LAUNCH,23+length);desk_put32(m.data+4,43);desk_put32(m.data+8,7);
    memcpy(m.data+12,"install",7);desk_put32(m.data+19,length);memcpy(m.data+23,installer,length);
    d.install_program=install_fixture;install_requests=0;
    d.clients[0].program=0;drain(&d,0);v2(&d,0,500,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && reply[0].result==-EPERM && !install_requests);
    d.clients[0].program=DESK_APP_CONTROL;drain(&d,0);v2(&d,0,500,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && !reply[0].result && install_requests==1);
    d.power.state=DESK_POWER_CLOSING;drain(&d,0);v2(&d,0,500,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && reply[0].result==-EBUSY && install_requests==1);
    assert(install_program(&d,"/Programs/SETUP.EXE")==-EINVAL);
    assert(install_program(&d,"/mnt/d/GAME.EXE")==-EINVAL);
    assert(install_program(&d,"/mnt/d/../SETUP.EXE")==-EINVAL);
    puts("PASS f2-87 shell: Control Panel-only Install, shutdown refusal, removable installer policy, inventory refresh");
    d.power.state=DESK_POWER_IDLE;
    desk_message(&m,DESK_SETTINGS,24);desk_put32(m.data+4,42);
    desk_put32(m.data + 8, DESK_SETTINGS_DISPLAY); desk_put32(m.data + 12, 0x118);
    desk_put32(m.data + 20, 1); drain(&d, 0); v2(&d, 0, 500, &m);
    assert(display_calls == 2 && !queued(&d, 0, DESK_TASK_RESULT, reply) && sound_calls == 3);
    drop_all(&d); fake_reset();
    puts("PASS display/sound SETTINGS coexistence: independent dispatch, sound event payload, shutdown guard");
    puts("PASS display SETTINGS: Control Panel authority, display mode callback, serial/result, EBUSY/ENOSYS, reserved fields, no premature persistence");
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
__attribute__((noinline)) static void test_tasks_scenario(void)
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
    assert(d.clients[1].closing && queued(&d,1,DESK_CLOSE_REQUEST,0)==1 && !d.children[1].close_ns); /* G3: WM_CLOSE, no deadline */
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
    /* "/Programs/" names resolve to the same admitted program list. */
    int again=request(&d,0,300,DESK_LAUNCH,0,0,"/Programs/demo"); assert(again==302 || again==303);
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
__attribute__((noinline)) static void test_note_scenario(void)
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
    struct desk_program note_program={"ciuknote","/Programs/ciuknote"};
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
/* Owner decision 2026-10-12: Programs > Games (present with /Programs/dosbox)
 * lists the bundled shareware games; each starts /Programs/dosbox titled after
 * the game with its command (apps/desktop/catalog.h desk_games). */
static int item_at(const struct sh_menu *m, const char *label)
{ for (int k=0;k<m->count;k++) if (!strcmp(m->items[k].label,label)) return k; return -1; }
static void key(struct desktop *d,int code) { desk_shell_key(d,code,1); desk_shell_key(d,code,0); }
#ifndef DESK_FOOTPRINT_TEST
#include "../../../apps/dosbox/autorun.h"
#include "launch_input_test.h"
#include "launch_async_test.h"
#endif
static unsigned cd_player_launches;
static int cd_player_error;
static int launch_cd_player(struct desktop *d,const struct desk_launch_record *r)
{
    (void)d;
    assert(desk_launch_valid(r));
    assert(!strcmp(r->label,"CD Player") && !strcmp(r->target,"C:\\WINDOWS\\CDPLAYER.EXE"));
    assert(!r->arguments[0] && !r->directory[0] && r->show==1);
    cd_player_launches++;
    return cd_player_error ? -EIO : 123;
}
__attribute__((noinline)) static void test_cd_player_menu(void)
{
    struct desktop d;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.v3_launch=launch_cd_player;
    desk_menu_open(&d,SH_MENU_PROGRAMS,0,0);
    assert(item_at(&d.sh.menus[0],"CD Player")<0);
    assert(item_at(&d.sh.menus[0],"Accessories")>=0);
    desk_menu_close(&d);
    desk_menu_open(&d,SH_MENU_ACCESSORIES,0,0);
    int row=item_at(&d.sh.menus[0],"CD Player");
    assert(row>=0 && !strcmp(d.sh.menus[0].items[row].icon,"media-optical"));
    assert(d.sh.menus[0].items[row].action==SH_ACT_WIN32_PROGRAM);
    d.sh.menus[0].hot=row; key(&d,DU_KEY_ENTER);
    assert(cd_player_launches==1 && !d.program_count && !d.sh.levels);
    cd_player_error=1;
    desk_menu_open(&d,SH_MENU_ACCESSORIES,0,0);
    d.sh.menus[0].hot=item_at(&d.sh.menus[0],"CD Player"); key(&d,DU_KEY_ENTER);
    assert(cd_player_launches==2 && strstr(d.sh.message_text,"could not be started"));
    cd_player_error=0; desk_render_release(&d); fake_reset();
    puts("PASS CD Player: Accessories catalogue, Tango icon, Windows target, Win32 launch and failure dialog; native inventory unchanged");
}
/* G1: a double-click on the tray clock opens Date/Time Properties; a single
 * click or two clicks too far apart do not. */
__attribute__((noinline)) static void test_clock_double_click(void)
{
    struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.launch=launch_program; d.terminate=fake_terminate; d.authority_fd=-1;
    desk_program_add(&d,"control",elf,sizeof(elf));
    struct desk_box b=desk_shell_clock_box(&d);
    d.mouse_x=b.x+b.w/2; d.mouse_y=b.y+b.h/2;
    expect_control_arg="--panel=datetime"; unsigned before=control_spawns;
    uint64_t gap=(uint64_t)d.sh.cfg.double_click_ms*1000000ull;
    d.now_ns=5000000000ull; assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1)); desk_shell_button(&d,CIUKI_BUTTON_LEFT,0);
    assert(control_spawns==before);
    d.now_ns+=gap+1; assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1)); desk_shell_button(&d,CIUKI_BUTTON_LEFT,0);
    assert(control_spawns==before);                     /* too slow: a new first click */
    d.now_ns+=gap/2; assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1)); desk_shell_button(&d,CIUKI_BUTTON_LEFT,0);
    assert(control_spawns==before+1);
    d.now_ns+=gap/2; assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1)); desk_shell_button(&d,CIUKI_BUTTON_LEFT,0);
    assert(control_spawns==before+1);                   /* a third click starts over */
    expect_control_arg=NULL; drop_all(&d); desk_render_release(&d); fake_reset();
    puts("PASS G1 tray clock: double-click opens Date/Time Properties (--panel=datetime), single and slow clicks do not");
}
__attribute__((noinline)) static void test_games_menu(void)
{
    struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.launch=launch_program; d.terminate=fake_terminate; d.authority_fd=-1;
    desk_program_add(&d,"files",elf,sizeof(elf)); desk_program_add(&d,"calc",elf,sizeof(elf));
    desk_menu_open(&d,SH_MENU_PROGRAMS,0,0);
    assert(item_at(&d.sh.menus[0],"Games")<0);                         /* no /Programs/dosbox: no Games */
    desk_menu_close(&d);
    desk_program_add(&d,"dosbox",elf,sizeof(elf));
    desk_menu_open(&d,SH_MENU_PROGRAMS,0,0);
    const struct sh_menu *programs=&d.sh.menus[0];
    int games=item_at(programs,"Games"), accessories=item_at(programs,"Accessories");
    assert(games==accessories+1 && programs->items[games].action==SH_ACT_SUBMENU &&
           programs->items[games].arg==SH_MENU_GAMES && !strcmp(programs->items[games].icon,"applications-games"));
    for (unsigned g=0; g<DESK_GAME_COUNT; g++) assert(!strcmp(desk_games[g].tango,"input-gaming"));   /* a game, not the Games folder */
    desk_menu_close(&d);
    for (unsigned g=0; g<DESK_GAME_COUNT; g++) {
        /* Start menu, Programs, Games, then the game's row: the keyboard path a user takes. */
        desk_menu_open(&d,SH_MENU_START,0,0);
        key(&d,DU_KEY_DOWN); assert(d.sh.menus[0].hot==0);
        key(&d,DU_KEY_RIGHT); assert(d.sh.levels==2 && d.sh.menus[1].kind==SH_MENU_PROGRAMS);
        while (d.sh.menus[1].hot!=games) key(&d,DU_KEY_DOWN);
        key(&d,DU_KEY_RIGHT); assert(d.sh.levels==3 && d.sh.menus[2].kind==SH_MENU_GAMES && d.sh.menus[2].count==(int)DESK_GAME_COUNT);
        assert(!strcmp(d.sh.menus[2].items[g].label,desk_games[g].label) && d.sh.menus[2].items[g].action==SH_ACT_GAME);
        while (d.sh.menus[2].hot!=(int)g) key(&d,DU_KEY_DOWN);
        char title[48]; snprintf(title,sizeof(title),"--name=%s",desk_games[g].title);
        expect_dosbox[0]=title; expect_dosbox[1]=desk_games[g].command;
        unsigned before=dosbox_spawns;
        key(&d,DU_KEY_ENTER);
        assert(dosbox_spawns==before+1 && !d.sh.levels && !d.launch_argv[0] && !d.launch_argv[1]);
        expect_dosbox[0]=expect_dosbox[1]=NULL;
    }
    d.games_removed=2;
    desk_menu_open(&d,SH_MENU_GAMES,0,0);
    assert(item_at(&d.sh.menus[0],"DOOM (shareware)")<0);
    assert(item_at(&d.sh.menus[0],"Wolfenstein 3D (shareware)")>=0);
    assert(item_at(&d.sh.menus[0],"Quake (shareware)")>=0);
    desk_menu_close(&d);d.games_removed=0;
    assert(DESK_GAME_COUNT == 3 && !strcmp(desk_games[2].label,"Quake (shareware)") &&
           !strcmp(desk_games[2].title,"Quake") && !strcmp(desk_games[2].command,"\\QUAKE\\QUAKE.EXE"));
    assert(!strcmp(desk_games[0].command,"\\WOLF3D\\WOLF3D.EXE") && !strcmp(desk_games[1].command,"\\DOOM\\DOOM.EXE -config CIUKI.CFG"));
    drop_all(&d); fake_reset();
    /* The same Games-menu route on EIO emits the executable and failing
     * operation before opening its error dialog, and releases both channels. */
    memset(d.children,0,sizeof(d.children));
    expect_dosbox[0]="--name=DOOM";expect_dosbox[1]=desk_games[1].command;fail_spawn=EIO;
    desk_menu_open(&d,SH_MENU_GAMES,0,0);d.sh.menus[0].hot=1;
    launch_record[0]=0;key(&d,DU_KEY_ENTER);
    assert(!strcmp(launch_record,"[launch] name=dosbox error=5 path=/Programs/dosbox operation=spawn"));
    assert(strstr(d.sh.message_text,"error 5") && !d.launch_argv[0] && !d.launch_argv[1]);
    for (unsigned i=0;i<DESK_CLIENTS;i++) assert(!d.children[i].pid && d.clients[i].channel<0);
    fail_spawn=0;expect_dosbox[0]=expect_dosbox[1]=NULL;drop_all(&d);fake_reset();
    /* Game shortcut Properties opens the shared per-game panel. */
    desk_program_add(&d,"control",elf,sizeof(elf));
    for (unsigned game=0;game<DESK_GAME_COUNT;game++) {
        snprintf(d.sh.icons[0].program,sizeof(d.sh.icons[0].program),"dosbox");
        snprintf(d.sh.icons[0].argument,sizeof(d.sh.icons[0].argument),"%s",desk_games[game].command);
        char arg[24];snprintf(arg,sizeof(arg),"--game=%u",game+1);expect_control_arg=arg;
        unsigned before=control_spawns;d.sh.menu_owner=0;
        desk_menu_open(&d,SH_MENU_ICON,0,0);d.sh.menus[0].hot=item_at(&d.sh.menus[0],"Properties");
        assert(d.sh.menus[0].hot>=0);key(&d,DU_KEY_ENTER);assert(control_spawns==before+1);
        expect_control_arg=NULL;drop_all(&d);
    }
    puts("PASS F1-63 DOS launch failure: first operation/path/errno recorded before Games dialog, diagnostic failure preserves EIO, channels cleaned");
    puts("PASS games menu: Programs > Games with /Programs/dosbox, keyboard path to each shareware game, dosbox --name=<title> <command>");
}
/* Owner feedback 2026-10-12: one fixed, coherent icon set. Every menu uses
 * the Tango 16 px artwork; inside one menu two rows never share an icon
 * (only the games, rows of one kind, share input-gaming); Run... and the
 * Ciuki Demo no longer borrow the icons of programs and games. */
__attribute__((noinline)) static void test_menu_icons(void)
{
    struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    static const char *const bin[]={"calc","charmap","ciukide","ciuknote","clock","control","demo","dosbox","files","hello","help",
                                    "libc_smoke","lua","mixer","netsurf","tasks","tcc","viewer","newtool"};
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    for (unsigned i=0;i<sizeof(bin)/sizeof(bin[0]);i++) desk_program_add(&d,bin[i],elf,sizeof(elf));
    d.sh.cfg.recent_count=1; snprintf(d.sh.cfg.recent[0],sizeof(d.sh.cfg.recent[0]),"/notes.txt");
    static const int kinds[]={SH_MENU_START,SH_MENU_PROGRAMS,SH_MENU_ACCESSORIES,SH_MENU_GAMES,SH_MENU_DEVELOPER,SH_MENU_DOCUMENTS,
                              SH_MENU_SETTINGS,SH_MENU_HELP,SH_MENU_DESKTOP,SH_MENU_ICON,SH_MENU_TASK};
    for (unsigned k=0;k<sizeof(kinds)/sizeof(kinds[0]);k++) {
        d.sh.menu_owner=0; desk_menu_open(&d,kinds[k],0,0);
        const struct sh_menu *m=&d.sh.menus[0];
        assert(m->icon==16);
        for (int a=0;a<m->count;a++) for (int b=a+1;b<m->count;b++) {
            const struct sh_item *x=&m->items[a], *y=&m->items[b];
            if (x->icon && y->icon && !(x->action==SH_ACT_GAME && y->action==SH_ACT_GAME)) assert(strcmp(x->icon,y->icon));
        }
        if (kinds[k]==SH_MENU_DEVELOPER) {   /* only programs outside Programs and Accessories */
            assert(m->count==5 && item_at(m,"Hello")>=0 && item_at(m,"libc smoke test")>=0 && item_at(m,"newtool")>=0 &&
                   item_at(m,"CiukIDE")>=0 && item_at(m,"Tiny C Compiler")>=0);
            assert(item_at(m,"Calculator")<0 && item_at(m,"CiukNote")<0);
        }
        desk_menu_close(&d);
    }
    /* Every settings shortcut is drawn from the same category catalog. */
    unsigned seen[CP_PAGES]={0};
    for (int cat=0;cat<CP_CATEGORIES;cat++) {
        desk_menu_open(&d,SH_MENU_SETTINGS_CATEGORY+cat,0,0);
        const struct sh_menu *menu=&d.sh.menus[0];
        for (int k=0;k<menu->count;k++) {
            const struct sh_item *it=&menu->items[k];
            assert(it->action==SH_ACT_CONTROL && it->arg>0 && it->arg<=CP_PAGES);
            assert(!seen[it->arg-1]++);
            unsigned match=0;
            for (unsigned a=0;a<CP_APPLETS;a++) if (cp_applets[a].page==it->arg-1)
                match=cp_applets[a].category==cat && !strcmp(it->label,cp_applets[a].label) && !strcmp(it->icon,cp_applets[a].icon);
            assert(match);
        }
    }
    for (unsigned a=0;a<CP_APPLETS;a++) {
        assert(seen[cp_applets[a].page]==1);
        d.sh.menu_owner=cp_applets[a].page; desk_menu_open(&d,SH_MENU_TRAY_SETTINGS,0,0);
        assert(d.sh.menus[0].count==3 && !strcmp(d.sh.menus[0].items[0].label,cp_categories[cp_applets[a].category].label) &&
            !strcmp(d.sh.menus[0].items[2].label,cp_applets[a].label));
    }
    desk_menu_open(&d,SH_MENU_DESKTOP,0,0);
    static const char *const context[]={"Arrange icons","Line up","Refresh","New","Paste","","Properties"};
    assert(d.sh.menus[0].count==7);
    for (unsigned k=0;k<7;k++) assert(!strcmp(d.sh.menus[0].items[k].label,context[k]));
    assert(d.sh.menus[0].items[6].action==SH_ACT_CONTROL && d.sh.menus[0].items[6].arg==CP_DISPLAY+1);
    desk_menu_open(&d,SH_MENU_NEW,0,0);assert(d.sh.menus[0].count==2);
    /* Start > Settings > Input > Keyboard keeps keyboard submenu traversal. */
    desk_menu_open(&d,SH_MENU_START,0,0);d.sh.menus[0].hot=item_at(&d.sh.menus[0],"Settings");key(&d,DU_KEY_RIGHT);
    assert(d.sh.levels==2);d.sh.menus[1].hot=item_at(&d.sh.menus[1],"Input");key(&d,DU_KEY_RIGHT);
    assert(d.sh.levels==3); key(&d,DU_KEY_LEFT); assert(d.sh.levels==2);
    desk_menu_open(&d,SH_MENU_START,0,0);
    const char *run=d.sh.menus[0].items[item_at(&d.sh.menus[0],"Run...")].icon;
    const char *settings=d.sh.menus[0].items[item_at(&d.sh.menus[0],"Settings")].icon;
    desk_menu_close(&d);
    assert(strcmp(run,"application-x-executable") && strcmp(settings,desk_program_icon_name("control")));
    for (unsigned i=0;i<sizeof(bin)/sizeof(bin[0]);i++) assert(strcmp(run,desk_program_icon_name(bin[i])));
    assert(strcmp(desk_program_icon_name("demo"),"applications-games") && strcmp(desk_program_icon_name("demo"),desk_games[0].tango));
    drop_all(&d); fake_reset();
    puts("PASS menu icons: every menu 16 px, no two rows of a menu share an icon, Run.../Demo/Settings distinct, Developer lists only its programs");
}
/* f6-19 (T23 1.0.0 alpha: "[console] Unable to initialise framebuffer" after
 * a Programs-menu launch): NetSurf is the browser application. Its catalog.h
 * record carries the name, the description, the Tango icon and the desktop
 * window grant; the Programs root lists it (never Developer) and every launch
 * route passes --channel-fd=3, the same argv as the GATE_BROWSER route. */
static const char *netsurf_icon_request;
static uint32_t netsurf_icon_pixels[48*48];
static const uint32_t *netsurf_store(const char *name,unsigned size)
{ (void)size; netsurf_icon_request=name; return netsurf_icon_pixels; }
static void test_hwreport_menu(void)
{
    static struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    desk_program_add(&d,"hwreport",elf,sizeof(elf));
    assert(desk_program_graphical("hwreport") && !desk_catalog_takes_file("hwreport"));
    assert(!strcmp(desk_program_category("hwreport"),"system-tools"));
    assert(!strcmp(desk_program_label("hwreport"),"Hardware Report"));
    desk_menu_open(&d,SH_MENU_PROGRAMS,0,0);
    int row=item_at(&d.sh.menus[0],"System Tools");
    assert(row>=0 && d.sh.menus[0].items[row].arg==SH_MENU_SYSTEM_TOOLS);
    assert(item_at(&d.sh.menus[0],"Developer")<0);
    desk_menu_close(&d); desk_menu_open(&d,SH_MENU_SYSTEM_TOOLS,0,0);
    row=item_at(&d.sh.menus[0],"Hardware Report");
    assert(row>=0 && d.sh.menus[0].items[row].action==SH_ACT_LAUNCH);
    assert(!strcmp(d.sh.menus[0].items[row].icon,"preferences-system"));
    desk_menu_close(&d); desk_menu_open(&d,SH_MENU_DEVELOPER,0,0);
    assert(item_at(&d.sh.menus[0],"Hardware Report")<0);
    desk_menu_close(&d); desk_render_release(&d); fake_reset();
    puts("PASS Hardware Report: Programs > System Tools, graphical grant, identity icon and file exclusion");
}
__attribute__((noinline)) static void test_netsurf_record(void)
{
    const struct desk_application_entry *app=desk_application_find("netsurf");
    assert(app && DESK_APPLICATION_COUNT==1 && !strcmp(app->label,"NetSurf") && !strcmp(app->description,"Web browser"));
    assert(!strcmp(app->tango,"internet-web-browser") && !desk_catalog_find("netsurf"));
    assert(desk_program_graphical("netsurf") && !desk_program_kind("netsurf") && !desk_catalog_takes_file("netsurf"));
    assert(!strcmp(desk_program_label("netsurf"),"NetSurf") && !strcmp(desk_program_description("netsurf"),"Web browser"));
    assert(!desk_program_description("files") && !desk_program_description("hello"));
    assert(!strcmp(desk_program_icon_name("netsurf"),app->tango) && !strcmp(desk_program_category("netsurf"),"programs"));
    struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.launch=launch_program; d.terminate=fake_terminate; d.authority_fd=-1;
    desk_program_add(&d,"hello",elf,sizeof(elf)); desk_program_add(&d,"netsurf",elf,sizeof(elf));
    desk_menu_open(&d,SH_MENU_PROGRAMS,0,0);
    int row=item_at(&d.sh.menus[0],"NetSurf");
    assert(row>=0 && !strcmp(d.sh.menus[0].items[row].icon,"internet-web-browser"));
    assert(d.sh.menus[0].items[row].action==SH_ACT_LAUNCH && !strcmp(d.programs[d.sh.menus[0].items[row].arg].name,"netsurf"));
    assert(item_at(&d.sh.menus[0],"Developer")>=0);   /* hello still has its folder */
    desk_menu_close(&d);
    desk_menu_open(&d,SH_MENU_DEVELOPER,0,0);
    assert(item_at(&d.sh.menus[0],"NetSurf")<0 && item_at(&d.sh.menus[0],"netsurf")<0 && item_at(&d.sh.menus[0],"Hello")>=0);
    desk_menu_close(&d);
    /* Start > Programs > NetSurf with the keyboard, then the pending launch. */
    desk_menu_open(&d,SH_MENU_START,0,0);
    key(&d,DU_KEY_DOWN); key(&d,DU_KEY_RIGHT); assert(d.sh.levels==2 && d.sh.menus[1].kind==SH_MENU_PROGRAMS);
    while (d.sh.menus[1].hot!=row) key(&d,DU_KEY_DOWN);
    key(&d,DU_KEY_ENTER); assert(!d.sh.levels && d.action>=0 && !strcmp(d.programs[d.action].name,"netsurf"));
    int pid=launch_pending(&d);
    assert(pid>0 && netsurf_spawns==1 && d.action<0);
    int slot=-1;
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].channel>=0 && d.clients[i].pid==(uint32_t)pid) slot=i;
    assert(slot>=0 && !strcmp(d.clients[slot].application,"netsurf") && d.clients[slot].awaiting_frame && !d.clients[slot].program);
    /* The window keeps the browser icon when the page title replaces "NetSurf - CiukiOS". */
    snprintf(d.clients[slot].title,sizeof(d.clients[slot].title),"Example Domain");
    d.icon=netsurf_store; netsurf_icon_request=NULL;
    assert(desk_shell_window_icon(&d,slot,16)==netsurf_icon_pixels && !strcmp(netsurf_icon_request,"internet-web-browser"));
    d.icon=NULL;
    drop_all(&d); memset(d.children,0,sizeof(d.children)); fake_reset();
    /* Run dialog (and the Terminal's DESK_LAUNCH): the same record. */
    char command[]="netsurf";
    assert(run_command(&d,command)>0 && netsurf_spawns==2);
    drop_all(&d); memset(d.children,0,sizeof(d.children)); fake_reset();
    /* GATE_BROWSER: no gate-only grant any more; the record supplies the channel. */
    expect_netsurf[0]="--test=browser"; expect_netsurf[1]="https://ciuki.test/";
    captured_report[0]=0;
    pid=desk_browser_fixture_begin(&d,"https://ciuki.test/");
    char line[48]; snprintf(line,sizeof(line),"case=browser child=%d",pid);
    assert(pid>0 && netsurf_spawns==3 && !strcmp(captured_report,line) && !d.launch_argv[0]);
    expect_netsurf[0]=expect_netsurf[1]=NULL;
    drop_all(&d); fake_reset();
    puts("PASS f6-19 NetSurf record: Programs root (not Developer), internet-web-browser icon, \"Web browser\", "
         "--channel-fd=3 from the menu, Run and GATE_BROWSER routes, window icon by launch identity");
}
static unsigned music_mixer_calls;
static int music_mixer_set(struct desktop *d,uint32_t l,uint32_t r,uint32_t mute)
{ (void)d; assert(l==30 && r==60 && !mute); music_mixer_calls++; return 0; }
__attribute__((noinline)) static void test_music_mixer(void)
{
    struct desktop d; fake_reset(); next_pair=20; desk_init(&d,&fake_ops,640,480);
    d.mixer_set=music_mixer_set; music_mixer_calls=0;
    struct desk_program program={"music","/Programs/music"};
    int pid=launch_program(&d,&program); assert(pid>0 && d.clients[0].mixer);
    window(&d,0,(uint32_t)pid,60,"Music Player");
    struct ciuki_message m; desk_message(&m,DESK_MIXER_SET,20);
    desk_put32(m.data+4,42); desk_put32(m.data+8,30); desk_put32(m.data+12,60); desk_put32(m.data+16,0);
    struct desk_packet reply[DESK_TX_MAX]; drain(&d,0); v2(&d,0,(uint32_t)pid,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && !reply[0].result && music_mixer_calls==1);
    d.power.state=DESK_POWER_CLOSING; drain(&d,0); v2(&d,0,(uint32_t)pid,&m);
    assert(queued(&d,0,DESK_TASK_RESULT,reply)==1 && reply[0].result==-EBUSY && music_mixer_calls==1);
    d.power.state=DESK_POWER_IDLE; program=(struct desk_program){"demo","/Programs/demo"};
    pid=launch_program(&d,&program); assert(pid>0 && !d.clients[1].mixer);
    window(&d,1,(uint32_t)pid,61,"Music Player"); /* a forged title grants no mixer rights */
    drain(&d,1); v2(&d,1,(uint32_t)pid,&m);
    assert(queued(&d,1,DESK_TASK_RESULT,reply)==1 && reply[0].result==-EPERM && music_mixer_calls==1);
    drop_all(&d); fake_reset(); pid_source=100; puts("PASS Music Player mixer: launch-owned permission, delegated callback, shutdown EBUSY, ordinary-client EPERM");
}
__attribute__((noinline)) static void test_launch_timing(void)
{
    static const char *const names[]={"files","ciuknote","control"};
    static const unsigned bytes[]={512u*1024u,384u*1024u,448u*1024u};
    uint64_t totals[2][3];
    trace_verbose=timing_model=1;
    for(unsigned mode=0;mode<2;mode++) for(unsigned program=0;program<3;program++) {
        struct desktop d; fake_reset(); next_pair=20; desk_init(&d,&fake_ops,640,480);
        d.pixels=calloc(640*480,4); assert(d.pixels);
        model_us=1000000; d.launch_click_ns=model_us*1000;
        memset(&launch_timing,0,sizeof(launch_timing)); model_elf_bytes=bytes[program];
        model_batch=mode ? 2048 : 512; model_us+=3000; /* menu close/channel setup fixture */
        struct desk_program p; snprintf(p.name,sizeof(p.name),"%s",names[program]);
        snprintf(p.path,sizeof(p.path),"/Programs/%s",p.name);
        int pid=launch_program(&d,&p); assert(pid>0);
        assert(desk_launch_wait(&d,model_us*1000)==1000000);
        assert(desk_launch_wait(&d,(model_us+5000000)*1000)==10000000);
        model_us+=1000+(mode ? 1000 : 10000); /* surface + CONFIGURE polling model */
        window(&d,0,(uint32_t)pid,60,p.name);
        assert(launch_timing.configure_us==model_us && !launch_timing.frames);
        assert(!desk_redraw(&d) && !launch_timing.frames); /* CREATE is not the app's frame */
        model_us+=15000+(mode ? 1000 : 10000); /* full app render + desktop polling model */
        struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20);
        desk_put32(m.data+12,120); desk_put32(m.data+16,80); v2(&d,0,(uint32_t)pid,&m);
        assert(!d.clients[0].awaiting_frame && d.clients[0].launch_frame_pending && !launch_timing.frames);
        assert(desk_launch_wait(&d,model_us*1000)==10000000);
        fake_fail_present=1;
        assert(desk_redraw(&d)==-1 && !launch_timing.frames && d.clients[0].launch_frame_pending);
        fake_fail_present=0; model_us+=1000;
        assert(!desk_redraw(&d) && launch_timing.frames==1 && !d.clients[0].launch_frame_pending);
        assert(!desk_redraw(&d) && launch_timing.frames==1);
        totals[mode][program]=model_us-launch_timing.click_us;
        printf("HOST_LAUNCH policy=%s name=%s click_us=%llu spawn_us=%llu elf_us=%llu configure_us=%llu damage_us=%llu first_frame_us=%llu total_us=%llu read_bytes=%llu image_requests=%u page_faults=0 model=2MiBps_6us_per_request\n",
               mode ? "after" : "before",p.name,(unsigned long long)launch_timing.click_us,
               (unsigned long long)launch_timing.spawn_us,(unsigned long long)launch_timing.elf_us,
               (unsigned long long)launch_timing.configure_us,(unsigned long long)launch_timing.damage_us,
               (unsigned long long)launch_timing.frame_us,(unsigned long long)totals[mode][program],
               (unsigned long long)launch_timing.bytes,launch_timing.image_requests);
        drop_all(&d); free(d.pixels);
    }
    for(unsigned i=0;i<3;i++) assert(totals[1][i]<totals[0][i]);
    trace_verbose=timing_model=0;
    fake_reset(); next_pair=20;
    struct desktop d; desk_init(&d,&fake_ops,640,480);
    struct desk_program p={"files","/Programs/files"};
    assert(launch_program(&d,&p)>0 && !d.launch_mark); drop_all(&d); fake_reset(); next_pair=20;
    timing_model=trace_verbose=1; model_us=1000000;
    struct ciuki_input_event input={.type=CIUKI_INPUT_KEY,.code=0x1e,.value=1,.monotonic_ns=900000000};
    desk_input(&d,&input); d.action=0; input.monotonic_ns=950000000; desk_input(&d,&input);
    assert(d.launch_click_ns==900000000); /* later events cannot replace a deferred launching click */
    struct desk_child children_before[DESK_CLIENTS];
    memcpy(children_before,d.children,sizeof(children_before));
    d.power.state=DESK_POWER_CLOSING;
    assert(launch_program(&d,&p)==-1 && errno==EBUSY);
    assert(!d.launch_mark && d.launch_click_ns==900000000 && !fake_live());
    assert(!memcmp(children_before,d.children,sizeof(children_before)));
    d.power.state=DESK_POWER_IDLE; /* Cancel restores the measured launch path. */
    assert(launch_program(&d,&p)>0 && d.launch_mark); /* console verbose after a quiet launch */
    assert(launch_timing.click_us==900000 && !d.launch_click_ns);
    drop_all(&d); fake_reset(); timing_model=trace_verbose=0;
    puts("PASS launch timing: three policies/models, quiet mode, CREATE/DAMAGE/present distinction, failed present retry, no duplicate frames, bounded startup polling, shutdown blocks launch before tracing");
}
#include "shutdown_sequence_test.h"
#include "end_session_test.h"
static pthread_mutex_t wallpaper_test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wallpaper_test_changed=PTHREAD_COND_INITIALIZER;
static int wallpaper_test_entered, wallpaper_test_release;
static void wallpaper_test_stage(const char *name)
{
    startup_stage(name);
    if (strcmp(name,"wallpaper_file_read")) return;
    pthread_mutex_lock(&wallpaper_test_lock); wallpaper_test_entered=1;
    pthread_cond_signal(&wallpaper_test_changed);
    while (!wallpaper_test_release) pthread_cond_wait(&wallpaper_test_changed,&wallpaper_test_lock);
    pthread_mutex_unlock(&wallpaper_test_lock);
}
__attribute__((noinline)) static void test_wallpaper_startup(void)
{
    /* Hold the loader after file I/O, before decoding; run production wallpaper
     * service, menu input and rendering while it is still held. */
    unsigned char bmp[70]={ 'B','M',70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,
        2,0,0,0,2,0,0,0,1,0,24,0,0,0,0,0,16,0,0,0 };
    for (unsigned i=54;i<70;i++) bmp[i]=(unsigned char)(i*3);
    char path[]="/tmp/f2-44-XXXXXX"; int fd=mkstemp(path); assert(fd>=0);
    assert(write(fd,bmp,sizeof(bmp))==sizeof(bmp)); assert(!close(fd));
    struct desktop d; fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    d.pixels=calloc(640*480,4); assert(d.pixels);
    snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"%s",path);
    memset(startup_us,0,sizeof(startup_us)); startup_cached=startup_sent=0;
    startup_begin=monotonic(); wallpaper_worker_failed=0;
    wallpaper_load(&d);
    wallpaper_poll(&d); assert(!wallpaper_worker_started); /* no loader before first frame */
    assert(!desk_redraw(&d)); startup_stage("first_frame_presented");
    assert(!desk_wall_worker_start(&wallpaper_worker,NULL,wallpaper_test_stage)); wallpaper_worker_started=1;
    wallpaper_serial=desk_wall_worker_submit(&wallpaper_worker,&wallpaper_request);
    pthread_mutex_lock(&wallpaper_test_lock);
    while (!wallpaper_test_entered) pthread_cond_wait(&wallpaper_test_changed,&wallpaper_test_lock);
    pthread_mutex_unlock(&wallpaper_test_lock);
    uint64_t start=monotonic();
    struct ciuki_input_event key={.type=CIUKI_INPUT_KEY,.code=0x15b,.value=1}; desk_input(&d,&key);
    key.value=0; desk_input(&d,&key); /* left Logo opens Start on release */
    assert(d.menu_open);
    wallpaper_poll(&d); assert(d.sh.wallpaper_pending);
    assert(!desk_redraw(&d));
    assert(monotonic()-start<50000000ull);
    assert(!startup_us[7]); /* wallpaper not applied, input and a reaction already drawn */
    pthread_mutex_lock(&wallpaper_test_lock); wallpaper_test_release=1;
    pthread_cond_signal(&wallpaper_test_changed); pthread_mutex_unlock(&wallpaper_test_lock);
    for (unsigned i=0;i<10000 && d.sh.wallpaper_pending;i++) {
        wallpaper_poll(&d);
        if (d.sh.wallpaper_pending) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    assert(!d.sh.wallpaper_pending && !d.sh.image_failed && startup_us[7]);
    assert(!d.sh.image.pixels && d.sh.wall_valid && d.menu_open);
    /* A theme/icon invalidation keeps the image; colour/size changes resubmit. */
    uint32_t *frame=d.sh.wall; d.sh.wall_valid=0; wallpaper_update(&d); assert(d.sh.wall==frame && d.sh.wall_valid);
    d.sh.cfg.desktop_color++; wallpaper_update(&d); assert(d.sh.wallpaper_pending);
    d.width=800; d.height=600; wallpaper_update(&d); assert(wallpaper_request.width==800 && wallpaper_request.height==600);
    desk_wall_worker_stop(&wallpaper_worker); wallpaper_worker_started=0;
    free(d.pixels); drop_all(&d); fake_reset(); assert(!unlink(path));
    puts("PASS startup: first solid frame before worker, Start input/reaction under 50 ms while loader held, pointer ownership, theme retention, colour/geometry invalidation");
}

static void test_media_removal_service(void)
{
    static struct desktop d;
    fake_reset();desk_init(&d,&fake_ops,640,480);desk_shell_enable(&d);
    timing_model=1;model_us=1000000;media_available=1;media_error=EBUSY;
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture),.removable_mask=1u<<4};
    media_fixture.type[4]=CIUKI_MEDIA_USB;media_fixture.generation[4]=77;
    media_read(&d);media_query_error=1;
    assert(!media_read(&d) && d.sh.media.removable_mask==(1u<<4));
    media_query_error=0;d.sh.want_remove=4;d.sh.remove_generation=77;media_remove(&d);media_read(&d);
    assert(d.sh.media.removable_mask && strstr(d.sh.notification.text,"ciuknote (42)"));
    assert(d.sh.notification.expires==monotonic()+8000000000ull);
    media_error=0;d.sh.want_remove=4;media_remove(&d);media_read(&d);
    assert(!d.sh.media.removable_mask && strstr(d.sh.notification.text,"safe to unplug"));
    assert(!d.sh.notification.balloon && d.sh.notification.started==monotonic());
    desk_shell_tick(&d);assert(d.sh.notification.active);
    media_read(&d);assert(strstr(d.sh.notification.text,"safe to unplug"));
    observers_close(&d);
    media_available=0;timing_model=0;desk_render_release(&d);fake_reset();
    puts("PASS media service: busy program names, refresh-before-success, ten-second eject starts a fresh eight-second notification, later detach preserves success");
}
__attribute__((noinline)) static void test_network_service(void)
{
    struct desktop d; fake_reset(); desk_init(&d, &fake_ops, 640, 480); desk_shell_enable(&d);
    network_fixture = (struct ciuki_net_state){ .version = 1, .size = sizeof(network_fixture),
        .present = 1, .link = CIUKI_NET_LINK_UP, .state = CIUKI_NET_RUNNING, .driver = "e100" };
    network_available = 1; network_error = 0; network_opens = network_closes = network_queries = 0;
    d.now_ns = 1000000000ull; d.damage.count = 0;
    desk_shell_tick(&d); assert(d.sh.want_reload & 16); shell_service(&d);
    assert(!d.sh.want_reload && d.sh.network.present && d.sh.network.link == CIUKI_NET_LINK_UP);
    assert(network_opens == 1 && network_queries == 1 && !network_closes && d.damage.count == 1);
    d.damage.count = 0; d.now_ns += 1999999999ull; desk_shell_tick(&d); shell_service(&d);
    assert(network_queries == 1 && !d.damage.count);
    d.now_ns++; desk_shell_tick(&d); shell_service(&d);
    assert(network_opens == 1 && network_queries == 2 && !network_closes && !d.damage.count);
    network_error = 1; d.now_ns += 2000000000ull; desk_shell_tick(&d); shell_service(&d);
    assert(!d.sh.network.present && network_closes == 1 && d.network_fd<0 && d.damage.count && d.sh.notification.active);
    assert(d.sh.notification.action==SH_NOTIFY_NETWORK);
    desk_shell_key(&d,DU_KEY_ESC,1); d.damage.count=0;
    network_error = 0; network_available = 0; d.damage.count = 0;
    d.now_ns += 2000000000ull; desk_shell_tick(&d); shell_service(&d);
    assert(!d.sh.network.present && network_queries == 3 && network_closes == 1 && !d.damage.count);
    network_available = 1; d.now_ns += 2000000000ull; desk_shell_tick(&d); shell_service(&d);
    assert(d.sh.network.present && network_opens == 3 && network_closes == 1 && d.damage.count && d.sh.notification.active);
    observers_close(&d); assert(network_closes==2);
    network_available = 0; desk_render_release(&d); fake_reset();
    puts("PASS network service: timer/reload, retained descriptor, unchanged state, failures and recovery");
}
#ifdef DESK_POLL_LEDGER_TEST
#include "poll_handles_test.h"
#endif
#ifndef DESK_FOOTPRINT_TEST
#include "autoplay_stall_test.h"
#endif
#ifdef DESK_FOOTPRINT_TEST
#include "memory_footprint_test.h"
#include "theme_wallpaper_sequence_test.h"
#include "theme_wallpaper_performance_test.h"
#include "wallpaper_boot_test.h"
#endif
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--slow-media-panel")) { test_slow_media_panel(); return 0; }
#ifdef DESK_POLL_LEDGER_TEST
    if (argc==2 && !strcmp(argv[1],"--poll-handles")) { test_poll_handles(); return 0; }
#endif
#ifdef DESK_FOOTPRINT_TEST
    if (argc==3 && !strcmp(argv[1],"--wallpaper-boot")) { test_wallpaper_boot(argv[2]); return 0; }
    if (argc==3 && !strcmp(argv[1],"--theme-wallpaper")) { test_theme_wallpaper(argv[2]); return 0; }
    if (argc==3 && !strcmp(argv[1],"--theme-wallpaper-performance")) { test_theme_wallpaper_performance(argv[2]); return 0; }
    if (argc==3 && !strcmp(argv[1],"--memory-footprint")) { test_memory_footprint(argv[2]); return 0; }
#else
    if (argc==2 && !strcmp(argv[1],"--autoplay-startup")) { test_autoplay_startup(); return 0; }
    if (argc==2 && !strcmp(argv[1],"--slow-stick-folder")) { test_slow_stick_folder(); return 0; }
    if (argc==2 && !strcmp(argv[1],"--launch-input")) { test_launch_input(); return 0; }
    if (argc==2 && !strcmp(argv[1],"--launch-async")) { test_launch_async(); return 0; }
#endif
    test_network_service();
    test_clock_double_click();
    test_media_removal_service();
    test_music_mixer();
    struct desktop d;
    fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    struct desk_program program={"demo","/Programs/demo"};
    struct launch_job regional_job;
    reg_defaults(&d.sh.cfg.regional,1);
    assert(!launch_prepare(&d,&program,&regional_job) && !strcmp(regional_job.env[0],"LANG=it_IT"));
    reg_defaults(&d.sh.cfg.regional,0);
    assert(!strcmp(regional_job.env[0],"LANG=it_IT")); /* queued spawn owns its environment snapshot */
    struct desk_program long_program;
    memset(&long_program,'a',sizeof(long_program));
    long_program.name[sizeof(long_program.name)-1]=0;long_program.path[sizeof(long_program.path)-1]=0;
    assert(launch_failed(&long_program,EIO,"channel-inherit")==-1 && errno==EIO);
    assert(strlen(launch_record)<=240 && strstr(launch_record,"... operation=channel-inherit"));
    fail_mapping=1; assert(launch_program(&d,&program)==-1 && errno==EBADF && !fake_live());
    assert(!strcmp(launch_record,"[launch] name=demo error=9 path=/Programs/demo operation=channel-inherit"));
    fail_mapping=0; fail_spawn=1;
    assert(launch_program(&d,&program)==-1 && errno==ENOEXEC && !fake_live());
    assert(!strcmp(launch_record,"[launch] name=demo error=8 path=/Programs/demo operation=spawn"));
    fail_spawn=0;
    d.shell_test=1; d.mouse_x=DESK_START_X+DESK_START_W/2; d.mouse_y=DESK_START_Y+DESK_START_H/2;   /* the Ciuki button in the top bar */
    assert(desk_shell_click(&d) && d.menu_open && d.shell_stage==1);
    int pid=launch_program(&d,&program);
    assert(pid==100 && d.clients[0].pid==100 && d.children[0].pid==100 && fake_live()==1);
    assert(!strcmp(d.clients[0].application,"demo"));
    d.shell_pid=(unsigned)pid; d.shell_stage=2;
    message(&d,0,DESK_HELLO,4);
    fake_surface(22,240,160,0xabcdef);
    struct ciuki_message m;
    desk_message(&m,DESK_CREATE_WINDOW,20); m.data[0]=2; m.sender_pid=100;
    m.fd_count=1; m.fds[0]=22;
    desk_put32(m.data+4,240); desk_put32(m.data+8,160); desk_put32(m.data+12,4);
    memcpy(m.data+16,"Demo",4); desk_receive(&d,0,&m);
    assert(d.clients[0].window && d.shell_stage==3);
    /* The gate scenarios hold input for a launched program until its first frame (desktop.c scenario_hold). */
    assert(d.clients[0].awaiting_frame && scenario_hold(&d,d.clients[0].launched_ns));
    assert(!scenario_hold(&d,d.clients[0].launched_ns+GATE_HOLD_MAX_NS));
    desk_message(&m,DESK_DAMAGE,20); m.data[0]=2; m.sender_pid=100; desk_put32(m.data+12,240); desk_put32(m.data+16,160);
    desk_receive(&d,0,&m); assert(!d.clients[0].awaiting_frame && !scenario_hold(&d,d.clients[0].launched_ns));
    /* f2-30: the A edges count whatever shell stage they arrive in (the 8042 may deliver them before the
     * close click); gate_report publishes them only at stage 6 (below). */
    struct ciuki_input_event a={.type=CIUKI_INPUT_KEY,.code=0x1e,.value=1};
    gate_keys=0; gate_input(&d,&a); a.value=0; gate_input(&d,&a); assert(gate_keys==2 && d.shell_stage==3);
    gate_keys=2; d.shell_keys=2; gate_report(&d,0,1,42,0,100); assert(strstr(captured_report," keys=0 "));
    gate_keys=0; d.shell_keys=0;
    desk_menu_close(&d);   /* as a menu launch does (shell.c activate): the menu drops over the demo's title now */
    assert(!d.menu_open && d.focus==0);
    struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
    d.mouse_x=70; d.mouse_y=60; desk_input(&d,&e); assert(d.drag==0);   /* G4: x 40 would be the caption icon, whose click opens the window menu */
    e.type=CIUKI_INPUT_MOTION; e.code=0; e.value=60; e.value2=60; desk_input(&d,&e);
    assert(d.shell_stage==4 && d.clients[0].frame.x==80 && d.clients[0].frame.y==112);
    e.type=CIUKI_INPUT_BUTTON; e.code=1; e.value=e.value2=0; desk_input(&d,&e);
    struct desk_box close=desk_close_box(&d.clients[0]);
    d.mouse_x=close.x+4; d.mouse_y=close.y+4; e.value=1; desk_input(&d,&e);
    assert(d.shell_stage==5 && !d.children[0].close_ns && d.clients[0].closing && d.clients[0].window); /* G3: no deadline for v2 */
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
    struct desk_program hello={"hello","/Programs/hello"}, files={"files","/Programs/files"};
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
    /* The Run dialog strips the complete Programs prefix before menu lookup. */
    desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    d.programs[0]=hello; d.program_count=1; d.launch=launch_program;
    char command[]="/Programs/hello";
    assert(run_command(&d,command)>0);
    drop_all(&d); fake_reset();
    {
        int saved_errno=errno;
        errno=0;assert(win32_lease(&d,42,1,320,200,8,1)==-ENOSYS);
        errno=EACCES;assert(win32_lease(&d,42,1,320,200,8,1)==-ENOSYS);
        errno=saved_errno;
        puts("PASS presenter query: initialized version/size, negative status preserved independently of errno");
    }
    test_task_protocol();
    test_display_settings();
    test_tasks_scenario();
    test_launch_protocol();
    test_note_scenario();
    test_cd_player_menu();
    test_games_menu();
    test_menu_icons();
    test_hwreport_menu();
    test_netsurf_record();
    test_launch_timing();
    test_wallpaper_startup();
    test_shutdown_sequence();
    test_end_session();
    return 0;
}

int ciuki_display_query(struct ciuki_display_info *i) { assert(i->version==CIUKI_DISPLAY_VERSION && i->size==sizeof(*i)); return -ENOSYS; }
int ciuki_display_set(uint16_t id) { (void)id; return -EPERM; }

/* This desktop fixture has no installed SETUP.STATE. */
int desk_firstboot(struct desktop *d) { (void)d; return 0; }
