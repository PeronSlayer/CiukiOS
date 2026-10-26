/* Real objects, copies, fd inheritance, presenter and framing on fake RAM.
 * The optional integration build additionally exercises the proposed uaddr
 * hook, kept outside the directive's allowed production files.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main proc_fixture_main
#include "proc_test.c"
#undef main
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/i8042.h>
#include <ciuki/registry.h>
#include <ciuki/storage.h>
#include <ciuki/files.h>
#include <ciuki/pager.h>
#include <ciuki/setup_mode.h>
uint32_t pmm_alloc_contig(uint32_t count)
{
    if (fail_alloc()) return 0;
    for (unsigned i=1;i+count<=HOST_PAGES;i++) {
        unsigned n=0; while (n<count && !allocated[i+n]) n++;
        if (n!=count) continue;
        for (n=0;n<count;n++) { allocated[i+n]=true; pages_used++; memset(P2V((i+n)*PAGE_SIZE),0xa5,PAGE_SIZE); }
        return i*PAGE_SIZE;
    }
    return 0;
}
/* Direct-map reply publication uses the same fake page tables as uaddr. */
uint32_t as_lookup(const struct aspace *as, uint32_t va, uint32_t *raw)
{
    uint32_t *entry=pte((struct uaddr *)(void *)as,va,false);
    uint32_t value=entry ? *entry : 0;
    if (raw) *raw=value;
    return value&PTE_P ? (value&~4095u)|(va&4095u) : 0;
}

/* Device-node transport for the desktop poll interleaving test. The process
 * ledger, slot comparison and record formatter remain production code. */
static struct file_description poll_files[3];
static struct px_node poll_nodes[3];
struct file_description *file_fd(const struct process *p,int32_t fd)
{
    if (!p || !p->fds || fd<0 || fd>=CIUKI_OPEN_MAX) return 0;
    for (unsigned i=0;i<3;i++)
        if (p->fds[fd].object==&poll_files[i].object) return &poll_files[i];
    return 0;
}

#define memcpy gate_memcpy
#define memmove gate_memmove
#define memset gate_memset
#define memcmp gate_memcmp
#define strlen gate_strlen
#define strncmp gate_strncmp
#include "../../../src/kernel/lib/string.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#undef strncmp
struct ciuki_boot_info g_boot;
static bool gate_simulation;
static bool print_diagnostics;
static bool routing_records;
static unsigned record_seq;
static char last_launch[241], last_step[241];
static unsigned gate_progress, gate_victims, gate_restored, gate_interactions;
static unsigned poll_handle_records;

static struct resource desktop_resource = { .generation = 1, .state = RS_ACTIVE };
static bool usb_ready;
bool usb_input_ready(void) { return usb_ready; }
static struct i8042_stats input_state = { .active = true, .generation = 1 };
static struct input_stats input_status;
static struct input_event input_events[CIUKI_INPUT_QUEUE_MAX];
static unsigned input_count, input_at, pipe_calls, frame_count;
static uint8_t frame_bytes[8192];
static unsigned frame_length;
static char frame_probe[24];
#ifndef CIUKI_DESKTOP_REAL_INPUT
void klog(const char *fmt, ...) { (void)fmt; }
void klog_periodic(const char *fmt, ...) __attribute__((alias("klog")));
#endif
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
void framebuffer_write_drain(void) {}
uint64_t framebuffer_cycles(void) { return 0; }
static unsigned operator_prompts;
void console_operator_prompt(bool announce) { if (announce) operator_prompts++; }
/* f3-02: the screen console stays dark while a process holds the display grant. */
static bool console_owned; static unsigned console_owned_changes;
static unsigned display_frames, display_uploads;
static bool display_full_frame;
static unsigned pager_boot_releases;
void pager_bootstrap_complete(void) { pager_boot_releases++; }
void console_display_owned(bool owned) { console_owned = owned; console_owned_changes++; }
static unsigned console_wakes;
void console_wake(void) { console_wakes++; }
static bool setup_media_ready;
bool setup_mode(void) { return setup_mode_parse(g_boot.flags, g_boot.options, sizeof(g_boot.options)); }
bool storage_setup_media_ready(void) { return setup_media_ready; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
static unsigned input_records;
static char last_input_record[241];
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[241] = { 0 };
    va_list ap; va_start(ap, fmt);
    int n = fmt ? kvsnprintf(extra, sizeof(extra), fmt, ap) : 0;
    va_end(ap);
    CHECK(n >= 0 && n < (int)sizeof(extra));
    char record[256];
    CHECK(ksnprintf(record,sizeof(record),"CIUKI_TEST v=1 run=00000000 seq=4294967295 probe=%s event=%s%s%s",probe,event,n ? " " : "",extra)<=240);
    if (strstr(extra,"case=gate server=desktop ")) {
        input_records++;
        strcpy(last_input_record,extra);
    }
    if (strstr(extra,"case=handle server=desktop ")) poll_handle_records++;
    if (routing_records) {
        CHECK(ksnprintf(record,sizeof(record),"CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s%s%s",++record_seq,probe,event,n ? " " : "",extra)<=240);
        puts(record);
        return;
    }
    if (print_diagnostics) puts(record);
    if (strstr(extra,"case=launch ")) strcpy(last_launch,extra);
    if (strstr(extra,"case=step ")) strcpy(last_step,extra);
    frame_count++;
    CHECK(!strcmp(event, "DATA") || ((!strcmp(probe, "libc-smoke") || !strcmp(probe,"crash-isolation")) &&
          (!strcmp(event, "BEGIN") || !strcmp(event, "END") || !strcmp(event, "ARM") || !strcmp(event, "ERROR"))));
    if (gate_simulation) {
        if (strstr(extra,"case=progress ")) {
            CHECK(strstr(extra,"server=desktop") && strstr(extra,"server_replies=100 replies=100") && strstr(extra,"unauthorized_access=0 desktop_restarts=0")); gate_progress++;
        }
        if (strstr(extra,"case=victim ")) { CHECK(strstr(extra,"server=desktop") && strstr(extra,"kind=")); gate_victims++; }
        if (strstr(extra,"case=restored ")) { CHECK(strstr(extra,"equal=1 processes_equal=1")); gate_restored++; }
        if (strstr(extra,"case=interaction ") || !strcmp(event,"ARM")) {
            extern bool probes_operator_mode;
            CHECK(strstr(extra,probes_operator_mode ? "criteria=operator" : "criteria=exact"));
            if (strstr(extra,"case=interaction ")) gate_interactions++;
        }
    }
    strcpy(frame_probe, probe);
    const char *data = strstr(extra, "data_hex=");
    if (data) {
        CHECK(!strstr(extra, "CIUKI_TEST"));
        data += 9;
        while (*data) {
            unsigned value;
            CHECK(sscanf(data, "%2x", &value) == 1 && frame_length < sizeof(frame_bytes));
            frame_bytes[frame_length++] = (uint8_t)value;
            data += 2;
        }
    }
}
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t deadline)
{
    (void)q; (void)deadline;
    if (!cond(arg)) task_sleep_ms(1);
    return cond(arg);
}
void proc_signal_pipe(struct proc_thread *t) { pipe_calls++; t->pending |= CIUKI_SIGBIT(SIGPIPE); }
void kmutex_init(struct kmutex *m) { memset(m, 0, sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { CHECK(!m->owner); m->owner = g_current; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner == g_current); m->owner = 0; }
unsigned registry_count(void) { return 1; }
const struct resource *registry_get(unsigned index) { return index ? 0 : &desktop_resource; }
bool registry_valid(int handle, gen_t gen) { return !handle && gen == desktop_resource.generation && desktop_resource.state == RS_ACTIVE; }
int registry_claim_reserved(int handle, gen_t gen, const char *owner) { (void)handle; (void)gen; (void)owner; return 0; }
int registry_activate(int handle, gen_t gen) { (void)handle; (void)gen; return 0; }
int registry_quarantine(int handle, gen_t gen) { (void)handle; (void)gen; return 0; }
void *vmm_map_framebuffer(uint32_t phys, uint32_t bytes) { (void)phys; (void)bytes; return 0; }
#ifndef CIUKI_DESKTOP_REAL_INPUT
void i8042_snapshot(struct i8042_stats *out) { *out = input_state; }
void input_snapshot(struct input_stats *out) { *out = input_status; out->pending = input_count - input_at; }
bool input_read(struct input_event *out)
{
    if (input_at == input_count) return false;
    *out = input_events[input_at++];
    return true;
}
#endif
static unsigned standin_attempts;
int supervisor_spawn_standin(struct process *parent, struct process **out)
{ (void)parent; (void)out; standin_attempts++; return -ENOENT; }

#include "../../../src/kernel/drivers/video/swdib.c"
#include "../../../src/kernel/drivers/video/batch.c"
#include "../../../src/kernel/drivers/fbdev.c"
#include "../display_driver_software_fixture.h"
#include "../../../src/kernel/proc/surface.c"
#include "../../../src/kernel/proc/channel.c"
#include "../../../src/kernel/proc/grants.c"
#include "../../../src/kernel/proc/display_batch.c"
#include "../../../src/kernel/proc/supervisor.c"
#include "../../../src/kernel/proc/syscalls_desktop.c"
static struct storage libc_storage;
static int libc_gate_error;
static unsigned libc_gate_calls, libc_spawn_calls;
struct storage *storage_get(void) { return &libc_storage; }
int storage_enable_write(struct storage *s, unsigned drive)
{
    CHECK(s == &libc_storage && drive == 2); libc_gate_calls++; return libc_gate_error;
}
/* f6-18: the browser probe's network and entropy calls are never reached by
 * these gate replays (no fixture selects the browser case); keep the host
 * binary linkable and fail if the harness accidentally enters them. */
int net_stack_primary(void) { CHECK(!"net_stack_primary outside the browser case"); return -1; }
void net_stack_address(unsigned port, uint32_t *address) { (void)port; CHECK(!"net_stack_address"); if (address) *address = 0; }
int net_stack_dhcp_error(unsigned port) { (void)port; CHECK(!"net_stack_dhcp_error"); return 0; }
int net_resolve_override(const char *name, uint32_t address) { (void)name; (void)address; CHECK(!"net_resolve_override"); return -1; }
int entropy_get(void *p, size_t n) { (void)p; (void)n; CHECK(!"entropy_get"); return -1; }
void entropy_erase(void *p, size_t n) { (void)p; (void)n; }
/* f2-31: only the Files desktop case flushes; no host fixture selects it. */
int storage_sync(void) { CHECK(!"storage_sync outside the Files case"); return -EIO; }
/* These native gate replays never select the installer controller. Keep
 * its VFS dependency explicit and fail if the harness accidentally enters it. */
#ifndef CIUKI_DESKTOP_REAL_VFS
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ (void)v;(void)t;(void)drive;CHECK(!"installer VFS in desktop spawn replay");return -EIO; }
void vfs_table_destroy(struct vfs_table *t)
{ (void)t;CHECK(!"installer VFS in desktop spawn replay"); }
int vfs_stat(struct vfs_table *t,const char *p,struct fat_entry *entry)
{(void)t;(void)p;(void)entry;CHECK(!"installer stat in desktop replay");return -EIO;}
int vfs_open(struct vfs_table *t,const char *p,unsigned flags,enum vfs_share share,uint8_t attr)
{(void)t;(void)p;(void)flags;(void)share;(void)attr;CHECK(!"installer open in desktop replay");return -EIO;}
int vfs_read(struct vfs_table *t,int fd,void *p,size_t n,size_t *done)
{(void)t;(void)fd;(void)p;(void)n;(void)done;CHECK(!"installer read in desktop replay");return -EIO;}
int vfs_close(struct vfs_table *t,int fd)
{(void)t;(void)fd;CHECK(!"installer close in desktop replay");return -EIO;}
/* f5-15C installers-tier observer: its C: walk is never entered by these replays. */
int vfs_find_first(struct vfs_table *t,const char *p,uint8_t mask,struct vfs_find *f,struct fat_entry *e)
{(void)t;(void)p;(void)mask;(void)f;(void)e;CHECK(!"installers-tier walk in desktop replay");return -EIO;}
int vfs_find_next(struct vfs_table *t,struct vfs_find *f,struct fat_entry *e)
{(void)t;(void)f;(void)e;CHECK(!"installers-tier walk in desktop replay");return -EIO;}
int vfs_flush_drive(struct vfs_table *t,unsigned drive)
{(void)t;(void)drive;CHECK(!"installers-tier flush in desktop replay");return -EIO;}
#endif
static int report_fixture_spawn(const char *path, const char *const argv[], const char *cwd,
                                bool desktop, struct process **out)
{
    CHECK(libc_gate_calls == 1 && !libc_gate_error && !strcmp(path, "/Programs/libc_smoke"));
    CHECK(argv && !strcmp(argv[0], "libc_smoke") && !argv[1] && !cwd && !desktop && out);
    libc_spawn_calls++; return -ENOENT;
}
static int gate_fixture_spawn(struct process **out);
/* This harness calls controllers directly, without the kernel's linker-table
 * dispatcher. Keep the declarations, but let unrelated F5 controllers and
 * their device/filesystem dependencies be discarded from the host binary. */
#pragma push_macro("CIUKI_F5_PROBE")
#undef CIUKI_F5_PROBE
#define CIUKI_F5_PROBE(id, func) static const struct probe_def \
    __attribute__((unused)) f5probe_##func = { id, func }
#define supervisor_spawn_desktop_probe gate_fixture_spawn
#define supervisor_spawn_standin report_fixture_standin
#define supervisor_spawn report_fixture_spawn
#include "../../../src/kernel/probes/f2_probes_desktop.c"
#undef supervisor_spawn
#undef supervisor_spawn_standin
#undef supervisor_spawn_desktop_probe
#pragma pop_macro("CIUKI_F5_PROBE")

static struct process *poll_server;
static struct proc_ledger poll_baseline;
static void poll_ref(struct proc_object *o) { (void)o; }
void desktop_poll_host_init(void)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!proc_prepare(proc_supervisor(),&poll_server)); proc_publish(poll_server,0);
    const enum px_kind kinds[]={PX_NET,PX_MIXER,PX_MEDIA};
    for (unsigned i=0;i<3;i++) {
        poll_nodes[i].kind=kinds[i];
        poll_files[i].node=&poll_nodes[i];
        poll_files[i].object=(struct proc_object){poll_ref,poll_ref,true};
    }
    print_diagnostics=true;
}
void desktop_poll_host_open(unsigned kind)
{
    CHECK(kind<3 && !poll_server->fds[20+kind].object);
    poll_server->fds[20+kind]=(struct proc_fd){&poll_files[kind].object,FD_CLOEXEC};
}
void desktop_poll_host_close(unsigned kind)
{
    CHECK(kind<3 && poll_server->fds[20+kind].object);
    CHECK(!desktop_close(poll_server,20+(int)kind));
}
void desktop_poll_host_baseline(void)
{ proc_snapshot(&poll_baseline); native_handles_snapshot(); }
void desktop_poll_host_leak(int leak)
{
    if (leak) {
        CHECK(!poll_server->fds[23].object);
        poll_server->fds[23]=(struct proc_fd){&poll_files[2].object,FD_CLOEXEC};
    } else CHECK(!desktop_close(poll_server,23));
}
void desktop_poll_host_check(int equal)
{
    struct proc_ledger now; proc_snapshot(&now);
    CHECK(native_proc_equal(&poll_baseline,&now)==(equal!=0));
    if (!equal) {
        CHECK(now.handles==poll_baseline.handles+1);
        unsigned records=poll_handle_records;
        native_handles_record(1);
        CHECK(poll_handle_records==records+1);
    }
}
void desktop_poll_host_finish(void)
{
    struct proc_ledger now; proc_snapshot(&now); CHECK(!now.handles);
    proc_discard(poll_server); poll_server=0;
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1);
    print_diagnostics=false; free(ram);
}

#define BUFFER (CIUKI_IMAGE_BASE + PAGE_SIZE)
static void desktop_select(struct proc_thread *t) { g_current = t->task; g_current->state = T_RUNNING; }
static void desktop_message(struct process *p, const struct ciuki_message *m) { CHECK(!ua_write(p->memory, BUFFER, m, sizeof(*m))); }
static void desktop_clean(struct process *p)
{
    stop_collect(p);
    struct desktop_ledger l; desktop_snapshot(&l);
    CHECK(!l.descriptions && !l.surfaces && !l.pages && !l.channels && !l.messages && !l.grants);
}
static void test_desktop_surfaces(void)
{
    struct process *p;
    struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    CHECK(surface_create(p, 0, 1, 1) == -EINVAL);
    CHECK(surface_create(p, 1, 2049, 1) == -EINVAL);
    CHECK(surface_create(p, 1, 1, 0) == -EINVAL);
    int fd = surface_create(p, 65, 65, CIUKI_SURFACE_XRGB8888); CHECK(fd >= 0);
    struct ciuki_surface_info info;
    CHECK(!surface_info(p, fd, &info));
    CHECK(info.width == 65 && info.height == 65 && info.stride == 260 && info.allocation_bytes == 20480);
    struct surface *s = desktop_fd(p, fd)->u.surface;
    uint8_t bytes[200];
    for (unsigned off = 0; off < info.allocation_bytes; off += sizeof(bytes)) {
        unsigned n = info.allocation_bytes - off;
        if (n > sizeof(bytes)) n = sizeof(bytes);
        CHECK(!surface_read(s, off, bytes, n));
        for (unsigned i = 0; i < n; i++) CHECK(!bytes[i]);
    }
    CHECK(surface_map(p, fd, PROT_EXEC) == -EINVAL);
    struct trap_frame tf = { .eax = CIUKI_SYS_FSTAT, .ebx = (uint32_t)fd };
    CHECK(desktop_fd_syscall(&tf) && (int32_t)tf.eax == -EOPNOTSUPP);
    tf = (struct trap_frame){ .eax = CIUKI_SYS_READ, .ebx = (uint32_t)fd };
    CHECK(desktop_fd_syscall(&tf) && (int32_t)tf.eax == -EBADF);
    tf = (struct trap_frame){ .eax = CIUKI_SYS_CLOSE, .ebx = 127 };
    CHECK(desktop_fd_syscall(&tf) && (int32_t)tf.eax == -EBADF);
    struct desktop_description *ro;
    CHECK(!surface_attachment(p, fd, &ro));
    int received = desktop_fd_install(p, ro, FD_CLOEXEC); CHECK(received >= 0);
    CHECK(desktop_fcntl(p, received, F_GETFL, 0) == O_RDONLY);
    CHECK(surface_map(p, received, PROT_READ | PROT_WRITE) == -EACCES);
    CHECK(desktop_fcntl(p, fd, F_SETFL, O_APPEND) == -EINVAL);
    CHECK(!desktop_fcntl(p, fd, F_SETFL, O_NONBLOCK));
    int dup = desktop_dup(p, fd, 0, false, 0); CHECK(dup >= 0);
    CHECK(desktop_fd(p, dup) == desktop_fd(p, fd));
    CHECK(desktop_fcntl(p, dup, F_GETFL, 0) == (O_RDWR | O_NONBLOCK));
    CHECK(p->fds[fd].flags == FD_CLOEXEC && p->fds[dup].flags == 0);
    if (ua_map_shared) {
        uint32_t a = (uint32_t)surface_map(p, fd, PROT_READ | PROT_WRITE);
        uint32_t b = (uint32_t)surface_map(p, received, PROT_READ);
        CHECK(a >= CIUKI_MMAP_BASE && a < CIUKI_MMAP_LIMIT && b != a && b < CIUKI_MMAP_LIMIT);
        put_word(p->memory, a, 0x12345678); CHECK(get_word(p->memory, b) == 0x12345678);
        CHECK(!desktop_close(p, fd) && !desktop_close(p, dup) && !desktop_close(p, received));
        CHECK(desktop_objects.surfaces == 1 && get_word(p->memory, b) == 0x12345678);
        CHECK(ua_mprotect(p->memory, b, PAGE_SIZE, PROT_READ | PROT_WRITE) == -EACCES);
        CHECK(ua_mprotect(p->memory, a, PAGE_SIZE, PROT_READ | PROT_EXEC) == -EACCES);
        CHECK(!ua_mprotect(p->memory, a + PAGE_SIZE, PAGE_SIZE, PROT_NONE));
        CHECK(!ua_mprotect(p->memory, a + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE));
        CHECK(!ua_munmap(p->memory, a + PAGE_SIZE, PAGE_SIZE));
        CHECK(!ua_munmap(p->memory, a, PAGE_SIZE));
        CHECK(!ua_munmap(p->memory, a + 2*PAGE_SIZE, info.allocation_bytes - 2*PAGE_SIZE));
        CHECK(desktop_objects.surfaces == 1);
        CHECK(!ua_munmap(p->memory, b, info.allocation_bytes));
        CHECK(!desktop_objects.surfaces && !desktop_objects.pages);
        puts("desktop shared aliases, partial unmap/protect and last mapping reference: PASS");
    } else {
        CHECK(surface_map(p, fd, PROT_READ) == -ENOMEM);
        puts("desktop shared mapping lifetime: NOT RUN (uaddr integration hook absent)");
    }
    desktop_clean(p);
    p = 0; t = make_process(proc_supervisor(), &p); desktop_select(t);
    for (int fault = 0; fault < 12; fault++) {
        unsigned pages = pages_used, heap = heap_blocks;
        alloc_fail = fault;
        fd = surface_create(p, 64, 64, 1);
        alloc_fail = -1;
        if (fd >= 0) desktop_close(p, fd);
        CHECK(pages_used == pages && heap_blocks == heap);
    }
    if (ua_map_shared) {
        fd = surface_create(p, 2048, 2048, 1); CHECK(fd >= 0);
        unsigned pages = pages_used, heap = heap_blocks;
        for (int fault = 0; fault < 6; fault++) {
            alloc_fail = fault;
            int32_t mapped = surface_map(p, fd, PROT_READ);
            alloc_fail = -1;
            if ((uint32_t)mapped < (uint32_t)-CIUKI_SYSCALL_ERROR_MAX)
                CHECK(!ua_munmap(p->memory, (uint32_t)mapped, CIUKI_SURFACE_MAX));
            CHECK(pages_used == pages && heap_blocks == heap);
        }
        CHECK((uint32_t)surface_map(p, fd, PROT_READ) < (uint32_t)-CIUKI_SYSCALL_ERROR_MAX);
        CHECK(!desktop_close(p, fd)); /* death releases final mapping */
    }
    desktop_clean(p);
    puts("desktop geometry, zero-fill, description rights and allocation rollback: PASS");
}
static void test_desktop_channels(void)
{
    struct process *p;
    struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    int32_t pair[2]; CHECK(!channel_pair(p, pair));
    struct ciuki_message m = { .length = 3, .data = { 'a', 'b', 'c' } }, got;
    desktop_message(p, &m);
    CHECK(channel_recv(p, pair[1], BUFFER, DONTWAIT) == -EAGAIN);
    for (unsigned i = 0; i < CIUKI_CHANNEL_QUEUE_MAX; i++) CHECK(!channel_send(p, pair[0], BUFFER, DONTWAIT));
    CHECK(channel_send(p, pair[0], BUFFER, DONTWAIT) == -EAGAIN);
    CHECK(channel_recv(p, pair[1], 0, 0) == -EFAULT && desktop_objects.messages == 64);
    CHECK(!desktop_close(p, pair[0]));
    for (unsigned i = 0; i < CIUKI_CHANNEL_QUEUE_MAX; i++) {
        CHECK(channel_recv(p, pair[1], BUFFER, DONTWAIT) == 1);
        CHECK(!ua_read(p->memory, &got, BUFFER, sizeof(got)));
        CHECK(got.sender_pid == p->pid && got.length == 3 && !memcmp(got.data, "abc", 3));
    }
    CHECK(channel_recv(p, pair[1], BUFFER, 0) == 0);
    desktop_message(p, &m);
    CHECK(channel_send(p, pair[1], BUFFER, 0) == -EPIPE && pipe_calls == 1);
    CHECK(t->pending & CIUKI_SIGBIT(SIGPIPE));
    CHECK(!desktop_close(p, pair[1]));
    CHECK(!channel_pair(p, pair));
    m.length = 257; desktop_message(p, &m); CHECK(channel_send(p, pair[0], BUFFER, 0) == -EMSGSIZE);
    m.length = 3; m.fd_count = 5; desktop_message(p, &m); CHECK(channel_send(p, pair[0], BUFFER, 0) == -EMSGSIZE);
    m.fd_count = 0; m.data[3] = 1; desktop_message(p, &m); CHECK(channel_send(p, pair[0], BUFFER, 0) == -EINVAL);
    m.data[3] = 0; m.sender_pid = p->pid; desktop_message(p, &m); CHECK(channel_send(p, pair[0], BUFFER, 0) == -EINVAL);
    m.sender_pid = 0; m.fd_count = 1; m.fds[0] = pair[0]; desktop_message(p, &m);
    CHECK(channel_send(p, pair[0], BUFFER, 0) == -EBADF);
    int fd = surface_create(p, 1, 1, 1); CHECK(fd >= 0);
    m.fds[0] = fd; desktop_message(p, &m);
    CHECK(!channel_send(p, pair[0], BUFFER, 0));
    CHECK(!desktop_close(p, fd) && desktop_objects.surfaces == 1);
    /* Fill every fd with endpoint duplicates, without copying descriptions. */
    while (desktop_dup(p, pair[0], 0, false, 0) >= 0) { }
    CHECK(channel_recv(p, pair[1], BUFFER, 0) == -EMFILE && desktop_objects.messages == 1);
    CHECK(!desktop_close(p, 127));
    CHECK(channel_recv(p, pair[1], BUFFER, 0) == 1);
    CHECK(!ua_read(p->memory, &got, BUFFER, sizeof(got)));
    CHECK(got.fds[0] == 127 && desktop_fd(p, 127)->maximum == PROT_READ && p->fds[127].flags == FD_CLOEXEC);
    CHECK(!desktop_close(p, 127) && !desktop_objects.surfaces);
    desktop_clean(p);
    puts("desktop channels: queue cap, drain/EOF, SIGPIPE, atomic receive, attachment lifetime PASS");
}

static void test_desktop_scheduler_hints(void)
{
    struct process *p; struct proc_thread *t=make_process(proc_supervisor(),&p);desktop_select(t);
    int32_t pair[2],grants[2];CHECK(!channel_pair(p,pair));
    struct ciuki_message m={.length=1,.data={1}};desktop_message(p,&m);
    CHECK(channel_send(p,pair[0],BUFFER,CIUKI_CHANNEL_FOCUS)==-EPERM);
    CHECK(!grants_install(p,grants));
    CHECK(channel_send(p,pair[0],BUFFER,CIUKI_CHANNEL_FOCUS|CIUKI_CHANNEL_UNFOCUS)==-EINVAL);
    /* The window belongs to a different thread from the compositor. */
    struct proc_thread *owner;
    CHECK(!proc_thread_prepare(p,CIUKI_IMAGE_BASE,0,CIUKI_IMAGE_BASE,CIUKI_THREAD_STACK_MIN,0,0,false,&owner));
    task_start(owner->task);
    t->task->id=1001;owner->task->id=1002;t->task->generation=owner->task->generation=1;
    fixture_tasks=t->task;t->task->all_next=owner->task;
    desktop_select(owner);CHECK(!channel_send(p,pair[1],BUFFER,0));
    desktop_select(t);CHECK(channel_recv(p,pair[0],BUFFER,DONTWAIT)==1);
    desktop_message(p,&m);unsigned inputs=fixture_inputs;
    CHECK(!channel_send(p,pair[0],BUFFER,CIUKI_CHANNEL_FOCUS|CIUKI_CHANNEL_INPUT));
    CHECK(fixture_focus==owner->task && fixture_inputs==inputs+1);
    for(unsigned i=1;i<CIUKI_CHANNEL_QUEUE_MAX;i++)CHECK(!channel_send(p,pair[0],BUFFER,0));
    CHECK(channel_send(p,pair[0],BUFFER,DONTWAIT|CIUKI_CHANNEL_INPUT)==-EAGAIN && fixture_inputs==inputs+1);
    desktop_select(owner);CHECK(channel_recv(p,pair[1],BUFFER,DONTWAIT)==1);
    desktop_select(t);desktop_message(p,&m);
    CHECK(!channel_send(p,pair[0],BUFFER,CIUKI_CHANNEL_UNFOCUS));CHECK(fixture_focus==t->task);
    desktop_select(owner);CHECK(channel_recv(p,pair[1],BUFFER,DONTWAIT)==1);
    desktop_select(t);desktop_message(p,&m);
    CHECK(!channel_send(p,pair[0],BUFFER,CIUKI_CHANNEL_INPUT) && fixture_inputs==inputs+1);
    task_focus(0);fixture_tasks=0;
    desktop_clean(p);
    puts("desktop scheduler hints: input-grant authority, endpoint thread identity, successful delivery only, focus loss PASS");
}
static void test_desktop_channel_rollback(void)
{
    struct process *p; struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    for (int fault = 0; fault < 5; fault++) {
        unsigned heap = heap_blocks;
        int32_t pair[2] = { -99, -99 };
        alloc_fail = fault;
        int err = channel_pair(p, pair);
        alloc_fail = -1;
        if (!err) { CHECK(!desktop_close(p, pair[0])); CHECK(!desktop_close(p, pair[1])); }
        else CHECK(err == -ENOMEM && pair[0] == -99 && pair[1] == -99);
        CHECK(heap_blocks == heap && !desktop_objects.channels && !desktop_objects.descriptions);
    }
    int32_t pair[2]; CHECK(!channel_pair(p, pair));
    int fd = surface_create(p, 1, 1, 1); CHECK(fd >= 0);
    struct ciuki_message m = { .fd_count = 4, .fds = { fd, fd, fd, fd } }, got;
    for (int fault = 0; fault < 9; fault++) {
        unsigned heap = heap_blocks;
        desktop_message(p, &m);
        alloc_fail = fault;
        int err = channel_send(p, pair[0], BUFFER, 0);
        alloc_fail = -1;
        if (!err) {
            CHECK(channel_recv(p, pair[1], BUFFER, 0) == 1);
            CHECK(!ua_read(p->memory, &got, BUFFER, sizeof(got)));
            for (unsigned i = 0; i < 4; i++) CHECK(!desktop_close(p, got.fds[i]));
        } else CHECK(err == -ENOMEM);
        CHECK(heap_blocks == heap && desktop_objects.descriptions == 3 && !desktop_objects.messages);
    }
    desktop_message(p, &m); CHECK(!channel_send(p, pair[0], BUFFER, 0));
    while (desktop_dup(p, pair[0], 0, false, 0) >= 0) { }
    for (unsigned i = 125; i < 128; i++) CHECK(!desktop_close(p, (int32_t)i));
    CHECK(channel_recv(p, pair[1], BUFFER, 0) == -EMFILE);
    CHECK(!p->fds[125].object && !p->fds[126].object && !p->fds[127].object && desktop_objects.messages == 1);
    CHECK(!ua_read(p->memory, &got, BUFFER, sizeof(got)) && !memcmp(&got, &m, sizeof(m)));
    CHECK(!desktop_close(p, 124));
    CHECK(channel_recv(p, pair[1], BUFFER, 0) == 1);
    CHECK(!ua_read(p->memory, &got, BUFFER, sizeof(got)));
    for (unsigned i = 0; i < 4; i++) CHECK(got.fds[i] == 124+(int32_t)i && p->fds[124+i].flags == FD_CLOEXEC);
    desktop_clean(p);
    puts("desktop channels: allocation rollback and four-fd receive transaction PASS");
}

static int32_t waiting_surface, waiting_receiver;
static bool release_queue;
static void desktop_wait_interleave(void)
{
    on_schedule = 0;
    struct process *p = proc_current();
    struct ciuki_message corrupt = { .length = 257 };
    desktop_message(p, &corrupt); /* a send must already own its full snapshot */
    CHECK(!desktop_close(p, waiting_surface));
    if (release_queue) {
        struct channel_queue *q = &desktop_fd(p, waiting_receiver)->u.endpoint.channel->inbox[1];
        struct channel_message *m = q->head;
        q->head = m->next; q->count--; desktop_objects.messages--;
        channel_message_free(m);
    } else {
        proc_thread_for(g_current)->interrupted = true;
    }
}
static void desktop_interrupt_wait(void)
{
    on_schedule = 0;
    proc_thread_for(g_current)->interrupted = true;
}
static void desktop_keyboard_after_wait(void)
{
    on_schedule=0; usb_ready=true;
    input_count=1; input_at=0;
    input_events[0]=(struct input_event){.type=INPUT_KEY,.code=INPUT_KEY_A,.value=1,
        .source=INPUT_USB,.generation=23,.sequence=1};
}
static void test_desktop_waits(void)
{
    for (unsigned success = 0; success < 2; success++) {
        struct process *p; struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
        int32_t pair[2]; CHECK(!channel_pair(p, pair));
        struct ciuki_message m = { .length = 3, .data = { 'a', 'b', 'c' } };
        desktop_message(p, &m);
        for (unsigned i = 0; i < 64; i++) CHECK(!channel_send(p, pair[0], BUFFER, 0));
        int fd = surface_create(p, 1, 1, 1); CHECK(fd >= 0);
        m.fd_count = 1; m.fds[0] = fd; desktop_message(p, &m);
        waiting_surface = fd; waiting_receiver = pair[1]; release_queue = success != 0;
        on_schedule = desktop_wait_interleave;
        CHECK(channel_send(p, pair[0], BUFFER, 0) == (success ? 0 : -EINTR));
        CHECK(!t->cleanup && !t->operation);
        CHECK(desktop_objects.surfaces == success);
        t->interrupted = false;
        for (unsigned i = 0; i < 64; i++) CHECK(channel_recv(p, pair[1], BUFFER, 0) == 1);
        if (success) {
            struct ciuki_message got; CHECK(!ua_read(p->memory, &got, BUFFER, sizeof(got)));
            CHECK(got.length == 3 && !memcmp(got.data, "abc", 3) && got.fd_count == 1);
            CHECK(desktop_fd(p, got.fds[0])->maximum == PROT_READ);
            CHECK(!desktop_close(p, got.fds[0]));
        }
        struct ua_pin *pins = p->memory->pins;
        on_schedule = desktop_interrupt_wait;
        CHECK(channel_recv(p, pair[1], BUFFER, 0) == -EINTR);
        CHECK(p->memory->pins == pins && !t->cleanup && !t->operation);
        t->interrupted = false;
        int32_t grants[2]; CHECK(!grants_install(p, grants));
        input_state.active=false; input_state.quarantined=true; usb_ready=false;
        input_count=input_at=0;
        on_schedule = desktop_interrupt_wait;
        CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EINTR);
        CHECK(p->memory->pins == pins && !t->cleanup && !t->operation);
        t->interrupted=false;
        on_schedule=desktop_keyboard_after_wait;
        CHECK(grant_input_read(p,grants[1],BUFFER,1)==1);
        struct ciuki_input_event key; CHECK(!ua_read(p->memory,&key,BUFFER,sizeof(key)));
        CHECK(key.type==CIUKI_INPUT_KEY && key.source==INPUT_USB && key.code==INPUT_KEY_A && key.value==1);
        CHECK(p->memory->pins==pins && !t->cleanup && !t->operation);
        input_state.active=true; input_state.quarantined=false; usb_ready=false; input_count=input_at=0;
        desktop_clean(p);
    }
    puts("desktop waits: immutable send snapshot, close while waiting, no-device EINTR/late USB key and pin/reference cleanup PASS");
}

static void test_desktop_grants(void)
{
    struct process *p, *child;
    struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    unsigned owned_changes = console_owned_changes;
    int32_t grants[2]; CHECK(!console_owned && !grants_install(p, grants));
    CHECK(console_owned && console_owned_changes == owned_changes + 1);
    CHECK(grants_install(p, grants) == -EPERM && console_owned_changes == owned_changes + 1);
    struct ciuki_framebuffer_info info;
    CHECK(grant_display_info(p, 127, &info) == -EBADF);
    CHECK(grant_display_info(p, grants[1], &info) == -EBADF);
    CHECK(grant_display_info(p, grants[0], &info) == -ENODEV);
    struct trap_frame change = {.eax = CIUKI_SYS_READ, .ebx = (uint32_t)grants[0], .ecx = BUFFER, .edx = 4};
    CHECK(desktop_fd_syscall(&change) && (int32_t)change.eax == 4);
    uint32_t counter; CHECK(!ua_read(p->memory, &counter, BUFFER, 4) && counter == 42);
    change.eax = CIUKI_SYS_READ; change.edx = 3;
    CHECK(desktop_fd_syscall(&change) && (int32_t)change.eax == -EINVAL);
    CHECK(grants_display_owner(p) && !grants_display_busy());
    int dup = desktop_dup(p, grants[0], 0, false, 0); CHECK(dup >= 0);
    CHECK(!desktop_fcntl(p, grants[0], F_SETFD, 0));
    struct ciuki_spawn_fd inherited = { grants[0], 3 };
    CHECK(proc_fd_validate(p, &inherited, 1) == -EPERM);
    CHECK(!proc_prepare(p, &child));
    CHECK(!proc_inherit(child, p, 0, 0));
    CHECK(!child->fds[0].object && !child->fds[1].object && !child->fds[2].object);
    proc_discard(child);
    struct desktop_description *attachment;
    CHECK(surface_attachment(p, grants[0], &attachment) == -EPERM);
    CHECK(surface_attachment(p, grants[1], &attachment) == -EPERM);
    desktop_fd(p, grants[0])->maximum = 0;
    CHECK(grant_display_info(p, grants[0], &info) == -EACCES);
    desktop_fd(p, grants[0])->maximum = PROT_WRITE;
    CHECK(!desktop_close(p, grants[0]));
    CHECK(grant_display_info(p, dup, &info) == -ENODEV);
    CHECK(!desktop_fcntl(p, grants[1], F_SETFL, O_NONBLOCK));
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EAGAIN);
    CHECK(grant_input_read(p, grants[1], BUFFER, 0) == -EINVAL);
    input_state.quarantined = true;
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EAGAIN);
    input_state.quarantined = false; input_state.active = false;
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EAGAIN);
    usb_ready = true; input_state.quarantined = true;
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EAGAIN);
    usb_ready = false; input_state.quarantined = false; input_state.active = true;
    struct input_event wheel = {.type = INPUT_REL, .code = INPUT_WHEEL, .value = -1, .source = INPUT_USB, .generation = 17};
    struct ciuki_input_event wheel_user;
    desktop_input_event(&wheel, 0, 0, &wheel_user);
    CHECK(wheel_user.type == CIUKI_INPUT_WHEEL && wheel_user.value == -1 && !wheel_user.value2 && wheel_user.source == INPUT_USB && wheel_user.generation == 17);
    for (unsigned button = INPUT_SIDE; button <= INPUT_EXTRA; button++) {
        struct input_event extra = { .type = INPUT_BTN, .code = button, .value = 1,
            .source = INPUT_NATIVE, .generation = 23, .tick = 123 };
        desktop_input_event(&extra, 0, 0, &wheel_user);
        CHECK(wheel_user.type == CIUKI_INPUT_BUTTON && wheel_user.code == (int32_t)button + 1 &&
              wheel_user.value == 1 && wheel_user.source == INPUT_NATIVE && wheel_user.generation == 23);
    }
    struct input_event extra_resync = { .type = INPUT_RESYNC };
    desktop_input_event(&extra_resync, 256, 0xFF, &wheel_user);
    CHECK(wheel_user.type == CIUKI_INPUT_RESYNC && wheel_user.value == 31 && wheel_user.lost_count == 256);
    input_count = 6; input_at = 0;
    input_events[0] = (struct input_event){ .type = INPUT_KEY, .code = INPUT_KEY_A, .value = 2, .source = INPUT_NATIVE, .generation = 1, .tick = 123, .sequence = 12 };
    input_events[1] = (struct input_event){ .type = INPUT_TEXT, .code = 'a' };
    input_events[2] = (struct input_event){ .type = INPUT_REL, .code = INPUT_X, .value = -12 };
    input_events[3] = (struct input_event){ .type = INPUT_REL, .code = INPUT_Y, .value = 13 };
    input_events[4] = (struct input_event){ .type = INPUT_BTN, .code = INPUT_RIGHT, .value = 1 };
    input_events[5] = (struct input_event){ .type = INPUT_RESYNC, .value = 5, .lost_count = 256 };
    input_status.buttons = 0; input_status.overflow = 999; /* later producer state */
    CHECK(grant_input_read(p, grants[1], 0, 6) == -EFAULT && !input_at);
    CHECK(grant_input_read(p, grants[1], BUFFER, 6) == 6);
    struct ciuki_input_event events[6]; CHECK(!ua_read(p->memory, events, BUFFER, sizeof(events)));
    CHECK(events[0].code == 0x1e && events[0].value == 2 && events[0].monotonic_ns == UINT64_C(123000000));
    CHECK(events[0].type == CIUKI_INPUT_KEY && !events[0].lost_count && !events[0].value2);
    CHECK(events[1].type == CIUKI_INPUT_TEXT && events[1].code == 'a' && !events[1].value);
    CHECK(events[2].type == CIUKI_INPUT_MOTION && events[2].value == -12 && !events[2].value2);
    CHECK(events[3].value2 == 13 && !events[3].value && !events[3].code);
    CHECK(events[4].type == CIUKI_INPUT_BUTTON && events[4].code == 2 && events[4].value == 1);
    CHECK(events[5].type == CIUKI_INPUT_RESYNC && events[5].value == 5 && events[5].lost_count == 256);
    input_events[0] = (struct input_event){ .type = INPUT_RESYNC, .value = 2, .lost_count = 512,
        .sequence = 258, .tick = 456, .source = INPUT_FIRMWARE, .generation = 7 };
    input_events[1] = (struct input_event){ .type = INPUT_KEY, .code = INPUT_KEY_A, .value = 1, .sequence = 259 };
    input_events[2] = (struct input_event){ .type = INPUT_KEY, .code = INPUT_KEY_A, .value = 0, .sequence = 260 };
    input_count = 3; input_at = 0;
    CHECK(grant_input_read(p, grants[1], BUFFER, 2) == 2);
    CHECK(!ua_read(p->memory, events, BUFFER, 2*sizeof(events[0])));
    CHECK(events[0].type == CIUKI_INPUT_RESYNC && events[0].lost_count == 512 && events[0].value == 2);
    CHECK(events[0].sequence == 258 && events[0].source == INPUT_FIRMWARE && events[0].generation == 7 &&
          events[0].monotonic_ns == UINT64_C(456000000) && !events[0].code && !events[0].value2);
    CHECK(events[1].type == CIUKI_INPUT_KEY && events[1].value == 1 && events[1].sequence == 259 && !events[1].lost_count);
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == 1 && input_at == 3);
    CHECK(!ua_read(p->memory, events, BUFFER, sizeof(events[0])));
    CHECK(events[0].type == CIUKI_INPUT_KEY && !events[0].value && events[0].sequence == 260);
    /* f1-122: raw pad coordinates/pressure stay inside the kernel stream;
     * public grants deliver only the existing pointer/button projection. */
    input_count = 4; input_at = 0;
    input_events[0] = (struct input_event){ .type = INPUT_ABS, .code = 0, .value = 2000 };
    input_events[1] = (struct input_event){ .type = INPUT_REL, .code = INPUT_X, .value = 4, .sequence = 262 };
    input_events[2] = (struct input_event){ .type = INPUT_ABS, .code = 2, .value = 40 };
    input_events[3] = (struct input_event){ .type = INPUT_BTN, .code = INPUT_LEFT, .value = 1, .sequence = 264 };
    CHECK(grant_input_read(p, grants[1], BUFFER, 2) == 2 && input_at == 4);
    CHECK(!ua_read(p->memory, events, BUFFER, 2 * sizeof(events[0])));
    CHECK(events[0].type == CIUKI_INPUT_MOTION && events[0].value == 4 && events[0].sequence == 262);
    CHECK(events[1].type == CIUKI_INPUT_BUTTON && events[1].code == 1 && events[1].sequence == 264);
    input_count = 1; input_at = 0;
    input_events[0] = (struct input_event){ .type = INPUT_ABS, .code = 2, .value = 0 };
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EAGAIN && input_at == 1);
    CHECK(console_owned);   /* the display duplicate and the input grant are still open */
    desktop_clean(p);
    CHECK(!console_owned && !desktop_objects.grants);
    puts("desktop grants: inheritance/transfer forbidden, duplication, authority, input conversion/RESYNC, console dark while granted PASS");
}

/* Independent pixel oracle enumerates source coordinates, not clipped rows. */
static void present_mapping_change(void)
{
    on_preempt_point = 0;
    CHECK(!presenter.owner && !g_current->preempt_holds);
    device.generation++;
}
static void test_command_transport(struct process *p, int grant)
{
    struct {
        struct ciuki_batch_header h;
        struct ciuki_batch_record r[2];
        struct ciuki_batch_target target;
    } blob={.h={CIUKI_BATCH_MAGIC,1,64,352,2,64,320,1,352,0,0,1,{0}},
        .r={{.opcode=1,.record_bytes=128,.dst=1,.dw=1,.dh=1,.foreground=0x123456},
            {.opcode=10,.record_bytes=128}},.target={.kind=1,.generation=1}};
    struct ciuki_batch_submit envelope={32,1,0,BUFFER+512,sizeof(blob),BUFFER+PAGE_SIZE-32,64,{0}};
    CHECK(!ua_write(p->memory,BUFFER+512,&blob,sizeof(blob)));
    CHECK(!ua_write(p->memory,BUFFER,&envelope,sizeof(envelope)));
    struct ciuki_accel_request request={.size=308,.operation=CIUKI_ACCEL_BATCH};
    CHECK(!ua_write(p->memory,BUFFER+128,&request,sizeof(request)));
    struct trap_frame tf={.eax=CIUKI_SYS_DISPLAY_ACCEL,.ebx=(uint32_t)grant,.ecx=BUFFER+128,.edx=BUFFER};
    CHECK(!desktop_syscall(&tf));
    struct ciuki_batch_reply reply;
    CHECK(!ua_read(p->memory,&reply,envelope.reply_address,64));
    CHECK(reply.fence && reply.completed==2 && reply.first_uncompleted==UINT32_MAX && reply.state==2 && (reply.flags&2));
    uint64_t first=reply.fence;
    struct ciuki_fence_control control={32,1,CIUKI_FENCE_STATUS,first,0,BUFFER+PAGE_SIZE-32,64,0};
    CHECK(!ua_write(p->memory,BUFFER,&control,sizeof(control)));
    CHECK(!grant_batch_request(p,grant,BUFFER,true));
    CHECK(!ua_read(p->memory,&reply,control.reply_address,64) && reply.fence==first);
    /* Malformed suffix rejects the entire snapshot before the first pixel. */
    blob.r[1].reserved[2]=1;
    CHECK(!ua_write(p->memory,BUFFER+512,&blob,sizeof(blob)));
    CHECK(!ua_write(p->memory,BUFFER,&envelope,32));
    CHECK(grant_batch_request(p,grant,BUFFER,false)==-EINVAL);
    CHECK(!ua_read(p->memory,&reply,envelope.reply_address,64) && !reply.fence && !reply.completed);
    blob.r[1].reserved[2]=0;
    CHECK(!ua_write(p->memory,BUFFER+512,&blob,sizeof(blob)));
    for (unsigned i=1;i<64;i++) CHECK(!grant_batch_request(p,grant,BUFFER,false));
    CHECK(grant_batch_request(p,grant,BUFFER,false)==-ENOSPC);
    for (uint64_t id=first;id<first+64;id++) {
        control.fence=id; control.action=CIUKI_FENCE_CANCEL;
        CHECK(!ua_write(p->memory,BUFFER,&control,32)); CHECK(!grant_batch_request(p,grant,BUFFER,true));
        control.action=CIUKI_FENCE_RELEASE;
        CHECK(!ua_write(p->memory,BUFFER,&control,32)); CHECK(!grant_batch_request(p,grant,BUFFER,true));
        CHECK(grant_batch_request(p,grant,BUFFER,true)==-ENOENT);
    }
    CHECK(!desktop_fd(p,grant)->display_fences);
    /* B6: cached controls do not turn an unsafe fence into completion. */
    struct batch_fence *pending=desktop_buffer_alloc(sizeof(*pending),MM_IO_REQUEST); CHECK(pending);
    pending->batch.reply=(struct ciuki_batch_reply){.size=64,.version=1,.state=CIUKI_FENCE_RECOVERING,
        .fence=++desktop_fd(p,grant)->display_sequence,.error=-EIO,.flags=CIUKI_FENCE_PARTIAL_DIRTY,.admitted=1};
    desktop_fd(p,grant)->display_fences=pending;
    control.fence=pending->batch.reply.fence; control.action=CIUKI_FENCE_WAIT; control.timeout_ms=0;
    CHECK(!ua_write(p->memory,BUFFER,&control,32)); CHECK(grant_batch_request(p,grant,BUFFER,true)==-ETIMEDOUT);
    control.action=CIUKI_FENCE_CANCEL;
    CHECK(!ua_write(p->memory,BUFFER,&control,32)); CHECK(!grant_batch_request(p,grant,BUFFER,true));
    CHECK(pending->batch.cancel && (pending->batch.reply.flags&CIUKI_FENCE_CANCEL_REQUESTED));
    control.action=CIUKI_FENCE_RELEASE;
    CHECK(!ua_write(p->memory,BUFFER,&control,32)); CHECK(grant_batch_request(p,grant,BUFFER,true)==-EBUSY);
    CHECK(desktop_fd(p,grant)->display_fences==pending && !(pending->batch.reply.flags&CIUKI_FENCE_CPU_SAFE));
    bool was_blocked=accel_blocked;
    batch_owner=&pending->batch; drawing=PRESENT_BUSY; accel_inflight=true;
    pending->busy=true; grant_batch_busy(true); enqueue_recovery(pending);
    CHECK(recovery_queue==pending && pending->users==1 && grants_display_busy());
    reap_once();
    CHECK(!recovery_queue && !grants_display_busy() && !drawing && !accel_inflight);
    CHECK(pending->batch.reply.state==CIUKI_FENCE_STOPPED && (pending->batch.reply.flags&CIUKI_FENCE_CPU_SAFE));
    CHECK(!grant_batch_request(p,grant,BUFFER,true)); CHECK(!desktop_fd(p,grant)->display_fences);
    /* Grant closure transfers the still-pinned source and upload to recovery. */
    unsigned before_pages=pages_used, before_surfaces=desktop_objects.surfaces;
    pending=desktop_buffer_alloc(sizeof(*pending),MM_IO_REQUEST); CHECK(pending);
    pending->batch.reply=(struct ciuki_batch_reply){.size=64,.version=1,.state=CIUKI_FENCE_RECOVERING,
        .fence=++desktop_fd(p,grant)->display_sequence,.error=-EIO,.flags=1,.admitted=1};
    pending->blob_bytes=64; pending->batch.blob=desktop_buffer_alloc(64,MM_IO_REQUEST); CHECK(pending->batch.blob);
    int source_fd=surface_create(p,1,1,CIUKI_SURFACE_BATCH_XRGB8888); CHECK(source_fd>=0);
    struct desktop_description *source=desktop_fd(p,source_fd);
    source->object.retain(&source->object); pending->refs[pending->ref_count++]=source;
    CHECK(!surface_batch_reserve(source->u.surface)); pending->reserved[pending->reserved_count++]=source->u.surface;
    CHECK(!desktop_close(p,source_fd));
    desktop_fd(p,grant)->display_fences=pending;
    batch_owner=&pending->batch; drawing=PRESENT_BUSY; accel_inflight=true;
    pending->busy=true; grant_batch_busy(true); enqueue_recovery(pending);
    grant_batch_close(desktop_fd(p,grant));
    CHECK(!desktop_fd(p,grant)->display_fences && recovery_queue==pending && pending->released);
    CHECK(source->u.surface->reserved && desktop_objects.surfaces==before_surfaces+1 && pages_used>before_pages);
    reap_once();
    CHECK(!recovery_queue && !grants_display_busy() && !drawing && !accel_inflight);
    CHECK(desktop_objects.surfaces==before_surfaces && pages_used==before_pages);
    accel_blocked=was_blocked;
    /* Managed surfaces cannot be mapped or bypassed through a read-only fd. */
    for (unsigned format=2;format<=4;format++) {
        int fd=surface_create(p,3,2,format); CHECK(fd>=0);
        struct desktop_description *d=desktop_fd(p,fd); struct swdib_target target;
        CHECK(surface_map(p,fd,PROT_READ|PROT_WRITE)==-EACCES);
        CHECK(!surface_batch_target(d,0,&target) && target.device.bpp==format*8);
        uint8_t canary[48]; memset(canary,0xa5,sizeof(canary));
        CHECK(!ua_write(p->memory,BUFFER+1024,canary,sizeof(canary)));
        struct trap_frame legacy={.eax=CIUKI_SYS_SURFACE_INFO,.ebx=(uint32_t)fd,.ecx=BUFFER+1024,.edx=1,.esi=36};
        CHECK(!desktop_syscall(&legacy)); CHECK(!ua_read(p->memory,canary,BUFFER+1024,sizeof(canary)));
        for (unsigned i=24;i<sizeof(canary);i++) CHECK(canary[i]==0xa5);
        struct ciuki_accel_request query={.size=308,.operation=CIUKI_ACCEL_QUERY_SURFACE,.colour=1,.background=36,.rect={.src_x=fd}};
        CHECK(!ua_write(p->memory,BUFFER+128,&query,sizeof(query)));
        struct trap_frame versioned={.eax=CIUKI_SYS_DISPLAY_ACCEL,.ebx=(uint32_t)grant,.ecx=BUFFER+128,.edx=BUFFER+1024};
        CHECK(!desktop_syscall(&versioned)); struct ciuki_batch_surface_info negotiated;
        CHECK(!ua_read(p->memory,&negotiated,BUFFER+1024,sizeof(negotiated)) && negotiated.size==36 && negotiated.generation==target.device.generation);
        CHECK(surface_batch_target(d,target.device.generation+1,&target)==-EAGAIN);
        CHECK(!surface_batch_target(d,0,&target));
        blob.target=(struct ciuki_batch_target){.kind=3,.handle=(uint32_t)fd,.generation=target.device.generation};
        CHECK(!ua_write(p->memory,BUFFER+512,&blob,sizeof(blob))); CHECK(!ua_write(p->memory,BUFFER,&envelope,32));
        CHECK(!grant_batch_request(p,grant,BUFFER,false));
        CHECK(!ua_read(p->memory,&reply,envelope.reply_address,64) && reply.completed==2);
        uint8_t pixel[4]={0}; CHECK(!surface_read(d->u.surface,0,pixel,format));
        CHECK(pixel[0]==(format==2 ? 0xaa : 0x56) && pixel[1]==(format==2 ? 0x11 : 0x34));
        CHECK(!surface_batch_reserve(d->u.surface));
        CHECK(surface_read(d->u.surface,0,pixel,format)==-EBUSY && surface_write(d->u.surface,0,pixel,format)==-EBUSY);
        surface_batch_unreserve(d->u.surface);
        struct desktop_description *ro; CHECK(!surface_attachment(p,fd,&ro));
        CHECK(!surface_batch_target(ro,0,&target) && target.rights==PROT_READ);
        ro->object.release(&ro->object);
        CHECK(!desktop_close(p,fd));
    }
    if (reaper) { reaper->state=T_ZOMBIE; task_reap(reaper); reaper=NULL; }
    puts("display batch transport: production syscall, pinned cross-page replies, all-or-nothing preflight, 64 fences, status/cancel/release, managed RGB565/BGR888/XRGB surfaces and reservation protection PASS");
}

static void test_desktop_present(void)
{
    struct process *p;
    struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    int32_t grants[2]; CHECK(!grants_install(p, grants));
    int fd = surface_create(p, 7, 5, 1); CHECK(fd >= 0);
    struct surface *s = desktop_fd(p, fd)->u.surface;
    uint32_t pixels[35]; for (unsigned i = 0; i < 35; i++) pixels[i] = 0xa5000000u | (i * 71111u);
    memcpy(P2V(surface_page(s, 0)), pixels, sizeof(pixels));
    uint8_t target[9*8*4+32], expected[sizeof(target)];
    for (unsigned bytes = 3; bytes <= 4; bytes++) {
        device = (struct fb_device){ .present = true, .width = 9, .height = 8, .pitch = 9*bytes+1,
            .size = (9*bytes+1)*8, .mapped = target, .bpp = (uint8_t)(bytes*8), .red_size = 5, .red_pos = 3,
            .green_size = 8, .green_pos = 8, .blue_size = 6, .blue_pos = 16, .handle = 0, .generation = 1 };
        initialized = true; kmutex_init(&presenter);
        for (int sx = -3; sx <= 7; sx += 2) for (int dx = -3; dx <= 9; dx += 2) {
            struct ciuki_rect r = { sx, -1, dx, 2, 8, 7 };
            memset(target, 0x6b, sizeof(target)); memcpy(expected, target, sizeof(target));
            for (unsigned y = 0; y < r.height; y++) for (unsigned x = 0; x < r.width; x++) {
                int64_t a = (int64_t)r.src_x + x, b = (int64_t)r.src_y + y;
                int64_t c = (int64_t)r.dst_x + x, d = (int64_t)r.dst_y + y;
                if (a < 0 || a >= 7 || b < 0 || b >= 5 || c < 0 || c >= 9 || d < 0 || d >= 8) continue;
                uint32_t rgb = pixels[b*7+a];
                uint32_t v = ((rgb >> 19) & 31) * 8 + ((rgb >> 8) & 255) * 256 + ((rgb & 255) / 4) * 65536;
                for (unsigned j = 0; j < bytes; j++) expected[d*device.pitch+c*bytes+j] = (uint8_t)(v >> (8*j));
            }
            CHECK(!grant_present(p, grants[0], fd, &r));
            CHECK(!memcmp(target, expected, sizeof(target)));
        }
        struct ciuki_rect invalid = { INT32_MAX, 0, 0, 0, 1, 1 };
        CHECK(grant_present(p, grants[0], fd, &invalid) == -EINVAL);
        invalid = (struct ciuki_rect){ INT32_MIN, INT32_MIN, 0, 0, UINT32_MAX, UINT32_MAX };
        CHECK(grant_present(p, grants[0], fd, &invalid) == -EINVAL);
        struct ciuki_framebuffer_info info; CHECK(!grant_display_info(p, grants[0], &info) && info.generation == 1);
        CHECK(!grants_display_suspend(true));
        CHECK(grants_display_suspend(true)==-EBUSY);
        CHECK(grant_display_info(p,grants[0],&info)==-EBUSY);
        CHECK(grant_present(p,grants[0],fd,&invalid)==-EBUSY);
        CHECK(!grants_display_suspend(false));
        CHECK(!grant_display_info(p,grants[0],&info));
        desktop_resource.state = RS_QUARANTINED;
        CHECK(grant_present(p, grants[0], fd, &invalid) == -EIO);
        desktop_resource.state = RS_ACTIVE;
    }
    test_command_transport(p,grants[0]);
    int wide_fd = surface_create(p, 2048, 2, 1); CHECK(wide_fd >= 0);
    /* f1-26: real syscall marshalling, grant ownership and software backend. */
    struct ciuki_accel_request q={.size=sizeof(q),.operation=CIUKI_ACCEL_QUERY,.generation=1};
    CHECK(!ua_write(p->memory,BUFFER,&q,sizeof(q)));
    struct trap_frame accel_call={.eax=CIUKI_SYS_DISPLAY_ACCEL,.ebx=(uint32_t)grants[0],.ecx=BUFFER};
    unsigned references=desktop_fd(p,grants[0])->references;
    CHECK(desktop_syscall(&accel_call)==0 && !grants_display_busy() &&
        !t->cleanup && !t->operation && desktop_fd(p,grants[0])->references==references);
    /* f2-52 optional read-only QUERY output, legacy query unchanged. */
    accel_call.edx=BUFFER+512;
    CHECK(!desktop_syscall(&accel_call));
    struct ciuki_accel_info ai;
    CHECK(!ua_read(p->memory,&ai,BUFFER+512,sizeof(ai)) && ai.generation==1 &&
        ai.driver==CIUKI_ACCEL_DRIVER_NONE && !ai.operations && !ai.capabilities && !ai.quarantined);
    /* f2-101: distinct version/size negotiation, real user pin/copy and
     * grant authorization; neither an old nor an oversized buffer is overrun. */
    struct ciuki_accel_request generic=q;
    generic.operation=CIUKI_ACCEL_QUERY_ENGINE; generic.colour=1;
    generic.background=sizeof(struct ciuki_display_engine_info)+16;
    CHECK(!ua_write(p->memory,BUFFER,&generic,sizeof(generic)));
    uint32_t canary=0x5a1234a5, observed=0;
    CHECK(!ua_write(p->memory,BUFFER+512+sizeof(struct ciuki_display_engine_info),&canary,sizeof(canary)));
    CHECK(!desktop_syscall(&accel_call));
    struct ciuki_display_engine_info engine;
    CHECK(!ua_read(p->memory,&engine,BUFFER+512,sizeof(engine)) && engine.version==1 &&
        engine.size==sizeof(engine) && engine.generation==1 && engine.kind==CIUKI_ENGINE_SOFTWARE &&
        engine.capabilities==0 && engine.cpu_ready && engine.software_operations==15);
    CHECK(!ua_read(p->memory,&observed,BUFFER+512+sizeof(engine),sizeof(observed)) && observed==canary);
    generic.background=sizeof(engine)-1; CHECK(!ua_write(p->memory,BUFFER,&generic,sizeof(generic)));
    CHECK(desktop_syscall(&accel_call)==-EINVAL);
    generic.background=sizeof(engine); generic.colour=2; CHECK(!ua_write(p->memory,BUFFER,&generic,sizeof(generic)));
    CHECK(desktop_syscall(&accel_call)==-EINVAL);
    generic.colour=1; generic.generation=2; CHECK(!ua_write(p->memory,BUFFER,&generic,sizeof(generic)));
    CHECK(desktop_syscall(&accel_call)==-EAGAIN);
    generic.generation=1; CHECK(!ua_write(p->memory,BUFFER,&generic,sizeof(generic)));
    accel_call.edx=1; CHECK(desktop_syscall(&accel_call)==-EFAULT);
    accel_call.edx=0; CHECK(desktop_syscall(&accel_call)==-EINVAL);
    CHECK(!ua_write(p->memory,BUFFER,&q,sizeof(q)));
    accel_call.edx=BUFFER+512;
    accel_call.edx=1; CHECK(desktop_syscall(&accel_call)==-EFAULT);
    accel_call.edx=BUFFER+512; q.generation=2; CHECK(!ua_write(p->memory,BUFFER,&q,sizeof(q)));
    CHECK(desktop_syscall(&accel_call)==-EAGAIN);
    q.generation=1; q.size--; CHECK(!ua_write(p->memory,BUFFER,&q,sizeof(q)));
    CHECK(desktop_syscall(&accel_call)==-EINVAL);
    q.size++; CHECK(!ua_write(p->memory,BUFFER,&q,sizeof(q))); accel_call.edx=0;
    accel_call.ecx=0; CHECK(desktop_syscall(&accel_call)==-EFAULT);
    accel_call.ebx=grants[1]; CHECK(desktop_syscall(&accel_call)==-EBADF);
    accel_call.ebx=grants[0]; desktop_fd(p,grants[0])->maximum=0;
    CHECK(desktop_syscall(&accel_call)==-EACCES);
    desktop_fd(p,grants[0])->maximum=PROT_WRITE;
    unsigned owner=desktop_fd(p,grants[0])->owner;desktop_fd(p,grants[0])->owner=owner+1;
    CHECK(desktop_syscall(&accel_call)==-EACCES);desktop_fd(p,grants[0])->owner=owner;
    q.generation=2;CHECK(grant_accel(p,grants[0],&q)==-EAGAIN);q.generation=1;
    q.size--;CHECK(grant_accel(p,grants[0],&q)==-EINVAL);q.size++;
    q.operation=CIUKI_ACCEL_CURSOR_SET;CHECK(grant_accel(p,grants[0],&q)==-EOPNOTSUPP);
    q.operation=CIUKI_ACCEL_FILL;q.colour=0x654321;q.rect=(struct ciuki_rect){0,0,1,2,4,3};
    CHECK(!grant_accel(p,grants[0],&q));
    for (unsigned y=2;y<5;y++) for (unsigned x=1;x<5;x++) {
        uint32_t value;memcpy(&value,target+y*device.pitch+x*4,4);
        CHECK(value==(((q.colour>>19)&31)*8+((q.colour>>8)&255)*256+((q.colour&255)/4)*65536));
    }
    memcpy(expected,target,sizeof(target));
    q.operation=CIUKI_ACCEL_COPY;q.rect=(struct ciuki_rect){1,2,2,3,4,3};
    CHECK(!grant_accel(p,grants[0],&q));
    uint8_t copy_source[sizeof(expected)]; memcpy(copy_source,expected,sizeof(expected));
    for (unsigned y=0;y<3;y++) memcpy(expected+(3+y)*device.pitch+2*4,copy_source+(2+y)*device.pitch+1*4,4*4);
    CHECK(!memcmp(target,expected,sizeof(target)));
    /* The normal RAM-surface upload remains valid after a software copy. */
    struct ciuki_rect recovery={0,0,2,3,4,3};
    CHECK(!grant_present(p,grants[0],fd,&recovery));
    for (unsigned y=0;y<3;y++) for (unsigned x=0;x<4;x++) {
        uint32_t rgb=pixels[y*7+x];
        uint32_t value=((rgb>>19)&31)*8+((rgb>>8)&255)*256+((rgb&255)/4)*65536;
        for (unsigned j=0;j<4;j++) expected[(3+y)*device.pitch+(2+x)*4+j]=(uint8_t)(value>>(8*j));
    }
    CHECK(!memcmp(target,expected,sizeof(target)));
    CHECK(!grants_display_busy() && !t->cleanup && !t->operation);
    puts("display accel syscall: grant/pointer/error ordering, wire/generation, software fill/copy retry, RAM re-present and lifetime cleanup PASS");
    struct surface *wide = desktop_fd(p, wide_fd)->u.surface;
    uint32_t pitch = 2048*4+3, size = pitch*2;
    uint8_t *wide_target = malloc(size); CHECK(wide_target); memset(wide_target, 0x55, size);
    device = (struct fb_device){ .present = true, .width = 2048, .height = 2, .pitch = pitch,
        .size = size, .mapped = wide_target, .bpp = 32, .red_size = 8, .red_pos = 16,
        .green_size = 8, .green_pos = 8, .blue_size = 8, .blue_pos = 0, .handle = 0, .generation = 1 };
    for (unsigned i = 0; i < 4096; i++) {
        uint32_t value = 0xff000000u | (i*123);
        memcpy((uint8_t *)P2V(surface_page(wide, i*4/PAGE_SIZE)) + i*4%PAGE_SIZE, &value, 4);
    }
    struct ciuki_rect whole = { 0, 0, 0, 0, 2048, 2 };
    unsigned acknowledged = display_frames;
    unsigned released = pager_boot_releases;
    unsigned points = preempt_points;
    CHECK(!grant_present(p, grants[0], wide_fd, &whole) && display_frames == acknowledged + 1);
    CHECK(pager_boot_releases == released + 1); /* after a complete successful frame */
    CHECK(preempt_points - points == 4); /* 2 rows, two 4096-byte tiles/row */
    points = preempt_points; on_preempt_point = present_mapping_change;
    CHECK(grant_present(p, grants[0], wide_fd, &whole) == -EIO);
    CHECK(pager_boot_releases == released + 1);
    CHECK(!on_preempt_point && preempt_points == points + 1 && !presenting &&
          !t->cleanup && !t->operation && display_frames == acknowledged + 1);
    device.generation = 1;
    whole.height = 1;
    unsigned uploaded=display_uploads;
    CHECK(!grant_present(p, grants[0], wide_fd, &whole) && display_frames == acknowledged + 1);
    CHECK(display_uploads==uploaded+1 && !display_full_frame);
    CHECK(pager_boot_releases == released + 1); /* partial frame cannot release boot work */
    /* An old larger surface clipped onto a smaller new screen is not a
     * complete compositor frame for the mode watchdog. */
    device.width = 1024; whole.height = 2;
    CHECK(!grant_present(p, grants[0], wide_fd, &whole) && display_frames == acknowledged + 1);
    device.width = 2048;
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 2048; x++) {
            uint32_t value; memcpy(&value, wide_target+y*pitch+x*4, 4);
            CHECK(value == (y*2048+x)*123);
        }
        CHECK(wide_target[y*pitch+8192] == 0x55 && wide_target[y*pitch+8194] == 0x55);
    }
    free(wide_target);
    device = (struct fb_device){ 0 };
    desktop_clean(p);
    puts("desktop present: source/destination clipping, masks, padded pitch, bounds and quarantine PASS");
}
static unsigned setup_opens;
static int setup_open_error;
static int setup_open(void *cwd, const char *path, struct ciuki_file *file)
{
    (void)cwd;
    CHECK(!strcmp(path, "/Programs/setup"));
    setup_opens++;
    if (setup_open_error) return setup_open_error;
    *file = fixture_file();
    return 0;
}
static int setup_stdio(struct process *p)
{
    CHECK(!p->published && !p->setup_capability);
    return 0;
}
static void test_setup_bootstrap(void)
{
    const struct ciuki_file_ops *saved_files = proc_get_file_ops();
    const struct supervisor_io_ops *saved_io = supervisor_io;
    struct ciuki_boot_info saved_boot = g_boot;
    static const struct ciuki_file_ops files = { .open = setup_open };
    static const struct supervisor_io_ops io = { .stdio = setup_stdio };
    proc_set_file_ops(&files); supervisor_set_io_ops(&io);
    struct process *p = 0;
    int result = 123;
    g_boot.flags = 0; g_boot.options[0] = 0;
    CHECK(!setup_bootstrap(&p, &result) && result == 123 && !setup_opens);
    strcpy(g_boot.options, "safe=0 serial=1 setup=1\n");
    unsigned wakes = console_wakes, fallback = standin_attempts;
    CHECK(setup_bootstrap(&p, &result) && result == -ENOENT && !p && !setup_opens);
    CHECK(console_wakes == wakes + 1 && standin_attempts == fallback);
    setup_media_ready = true;
    g_boot.flags = CBI_F_TEST_REQUEST;
    supervisor_bootstrap();
    CHECK(!bootstrap_attempted && !setup_opens); /* diagnostics keep their path */
    g_boot.flags = 0;
    CHECK(setup_bootstrap(&p, &result) && result > 0 && p && p->published);
    CHECK(p->setup_capability && !p->share_group && desktop_objects.grants==2);
    CHECK(setup_opens == 1 && standin_attempts == fallback);
    struct process *child = 0;
    CHECK(!proc_prepare(p, &child) && !child->setup_capability);
    child->setup_capability = true;
    supervisor_dosvm_delegate(p, child, "/Programs/setup");
    CHECK(!child->setup_capability); /* path and parent confer no authority */
    proc_discard(child);
    desktop_clean(p); p = 0;
    const char *argv[] = { "setup", 0 };
    CHECK(supervisor_spawn("/Programs/setup", argv, 0, false, &p) > 0);
    CHECK(!p->setup_capability); /* the ordinary supervisor API cannot grant it */
    desktop_clean(p); p = 0;
    setup_open_error = -ENOENT;
    CHECK(setup_bootstrap(&p, &result) && result == -ENOENT && !p);
    CHECK(standin_attempts == fallback + 1);
    setup_open_error = -EIO;
    CHECK(setup_bootstrap(&p, &result) && result == -EIO && !p);
    CHECK(standin_attempts == fallback + 1); /* only ENOENT selects the stand-in */
    setup_media_ready = false; setup_open_error = 0;
    g_boot = saved_boot;
    proc_set_file_ops(saved_files); supervisor_set_io_ops(saved_io);
    puts("Setup bootstrap: missing media/diagnostics refuse spawn, capability before publication only, no inheritance/path grant, ENOENT fallback PASS");
}
static void test_pager_boot_fallback(void)
{
    const struct ciuki_file_ops *saved = proc_get_file_ops();
    uint32_t boot_flags = g_boot.flags;
    unsigned releases = pager_boot_releases;
    CHECK(!bootstrap_attempted && !desktop_pid);
    g_boot.flags = CBI_F_TEST_REQUEST;
    supervisor_bootstrap();
    CHECK(!bootstrap_attempted && pager_boot_releases == releases);
    g_boot.flags = 0; proc_set_file_ops(0);
    supervisor_bootstrap(); /* desktop and stand-in absent: console fallback */
    CHECK(bootstrap_attempted && !desktop_pid && pager_boot_releases == releases + 1);
    supervisor_bootstrap(); CHECK(pager_boot_releases == releases + 1);
    desktop_pid = UINT32_MAX;
    uint32_t deaths = desktop_deaths;
    supervisor_poll();
    CHECK(!desktop_pid && desktop_deaths == deaths + 1 && pager_boot_releases == releases + 2);
    desktop_deaths = deaths; bootstrap_attempted = false;
    g_boot.flags = boot_flags; proc_set_file_ops(saved);
    puts("desktop pager boot: diagnostics bypass bootstrap; console fallback and death release manager; duplicate bootstrap is inert PASS");
}
static unsigned desktop_opens;
static int desktop_open_error;
static int desktop_open(void *cwd, const char *path, struct ciuki_file *file)
{
    (void)cwd;
    /* Transitional fixture has no sealed security-service images. */
    if (!strncmp(path,"/System/Services/",sizeof("/System/Services/")-1)) return -ENOENT;
    CHECK(!strcmp(path, "/Programs/desktop"));
    desktop_opens++;
    if (desktop_open_error) return desktop_open_error;
    *file = fixture_file();
    return 0;
}
static int desktop_stdio(struct process *p) { (void)p; return 0; }
/* G13 (ER-3): the supervisor restarts a crashed desktop, as Windows 9x
 * restarts a crashed Explorer shell; a clean exit, a stand-in, a probe and
 * four deaths in a minute leave the console. */
static void test_desktop_restart(void)
{
    const struct ciuki_file_ops *saved_files = proc_get_file_ops();
    const struct supervisor_io_ops *saved_io = supervisor_io;
    struct ciuki_boot_info saved_boot = g_boot;
    static const struct ciuki_file_ops files = { .open = desktop_open };
    static const struct supervisor_io_ops io = { .stdio = desktop_stdio };
    proc_set_file_ops(&files); supervisor_set_io_ops(&io);
    g_boot.flags = 0; g_boot.options[0] = 0;
    bootstrap_attempted = false; desktop_pid = 0; desktop_restartable = desktop_restarting = false;
    desktop_restarts = 0; g_ticks = 100000;
    fixture_focus = 0;                   /* an earlier fixture's freed task */
    unsigned deaths = desktop_deaths, wakes = console_wakes;
    CHECK(!supervisor_desktop_expected());
    supervisor_bootstrap();
    CHECK(bootstrap_attempted && desktop_pid && desktop_restartable && desktop_opens == 1);
    CHECK(supervisor_desktop_expected());
    g_current = &controller;
    for (unsigned crash = 1; crash <= DESKTOP_RESTART_MAX; crash++) {
        uint32_t old = desktop_pid;
        struct process *p = proc_find(old);
        struct desktop_ledger held; desktop_snapshot(&held);
        CHECK(p && held.grants);
        proc_stop(p, 0x10e, SIGSEGV);                      /* a fault kills it */
        supervisor_poll();
        CHECK(!desktop_pid && desktop_restarting && desktop_deaths == deaths + crash && console_wakes == wakes);
        CHECK(supervisor_desktop_expected() && !supervisor_desktop_pid());
        supervisor_poll();                                  /* grants still held: waits */
        CHECK(!desktop_pid && desktop_restarting && desktop_opens == crash);
        proc_collect(); fixture_focus = 0;                                     /* resources closed, zombie reaped */
        supervisor_poll();
        CHECK(desktop_pid && desktop_pid != old && !desktop_restarting && desktop_opens == crash + 1);
        CHECK(desktop_restarts == crash && supervisor_desktop_restarts() == crash && console_wakes == wakes);
    }
    /* The fourth death inside the minute gives up: console, no restart. */
    uint32_t last = desktop_pid;
    proc_stop(proc_find(last), 0x10e, SIGSEGV);
    supervisor_poll();
    CHECK(!desktop_pid && !desktop_restarting && !supervisor_desktop_expected() && console_wakes == wakes + 1);
    proc_collect(); fixture_focus = 0;
    /* The minute passes: a new window, restarts allowed again. */
    desktop_restartable = true; desktop_pid = 0;
    g_ticks += DESKTOP_RESTART_WINDOW_MS;
    CHECK(supervisor_restart_allowed(g_ticks) && desktop_restarts == 1);
    desktop_restarts = 0;
    /* A restart that cannot start falls back to the console. */
    bootstrap_attempted = false; g_ticks += 1000;
    supervisor_bootstrap();
    CHECK(desktop_pid && desktop_restartable);
    proc_stop(proc_find(desktop_pid), 0x10e, SIGSEGV);
    supervisor_poll(); proc_collect(); fixture_focus = 0;
    desktop_open_error = -ENOENT; wakes = console_wakes;
    supervisor_poll();
    CHECK(!desktop_pid && !desktop_restarting && console_wakes == wakes + 1);
    desktop_open_error = 0;
    /* A clean exit (status 0) is not a crash. */
    desktop_restarts = 0; bootstrap_attempted = false;
    supervisor_bootstrap();
    CHECK(desktop_pid && desktop_restartable);
    wakes = console_wakes;
    proc_stop(proc_find(desktop_pid), 0, 0);
    supervisor_poll(); proc_collect(); fixture_focus = 0;
    CHECK(!desktop_pid && !desktop_restarting && console_wakes == wakes + 1);
    /* A probe or stand-in desktop is never restarted. */
    desktop_restartable = false; desktop_restarts = 0;
    struct process *probe = 0;
    CHECK(supervisor_spawn_desktop_probe(&probe) > 0 && desktop_pid == probe->pid);
    wakes = console_wakes;
    proc_stop(probe, 0x10e, SIGSEGV);
    supervisor_poll(); proc_collect(); fixture_focus = 0;
    CHECK(!desktop_pid && !desktop_restarting && console_wakes == wakes + 1);
    desktop_restartable = false; desktop_restarting = false; desktop_pid = 0;
    desktop_restarts = 0; desktop_deaths = deaths; bootstrap_attempted = false;
    g_boot = saved_boot;
    proc_set_file_ops(saved_files); supervisor_set_io_ops(saved_io);
    puts("desktop restart: crash restarts the shell once its grants are free, three per minute, clean exit/probe/failed start leave the console PASS");
}
static void test_desktop_capture(void)
{
    struct supervisor_capture c; supervisor_capture_init(&c);
    uint8_t bytes[8192], tail[SUPERVISOR_CAPTURE_BYTES], digest[32]; char hex[65], got[65];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i % 251);
    for (unsigned off = 0; off < sizeof(bytes);) {
        unsigned n = sizeof(bytes) - off; if (n > 73) n = 73;
        supervisor_capture_add(&c, bytes+off, n); off += n;
    }
    sha256(bytes, sizeof(bytes), digest); sha256_hex(digest, hex); supervisor_capture_digest(&c, got);
    CHECK(!strcmp(hex, got) && c.bytes == sizeof(bytes));
    CHECK(c.head_bytes == SUPERVISOR_CAPTURE_BYTES && !memcmp(c.head, bytes, sizeof(c.head)));
    supervisor_capture_tail(&c, tail); CHECK(!memcmp(tail, bytes+sizeof(bytes)-sizeof(tail), sizeof(tail)));
    supervisor_capture_init(&c);
    supervisor_capture_add(&c, bytes, 4096);
    supervisor_capture_add(&c, "fi", 2); supervisor_capture_add(&c, "nal OK", 6);
    supervisor_capture_add(&c, "asser", 5); supervisor_capture_add(&c, "tion failed", 11);
    supervisor_capture_add(&c, bytes, 4096);
    CHECK(c.final_ok == 1 && c.assertion_failures == 1);
    struct process *p; struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    const char text[] = "CIUKI_TEST v=1 run=12345678 seq=999999 probe=libc-smoke event=END status=PASS";
    supervisor_observe("libc-smoke", p->pid);
    CHECK(supervisor_report(t->task, text, sizeof(text)-1));
    CHECK(frame_count > 1 && !strcmp(frame_probe, "libc-smoke") && frame_length == sizeof(text)-1);
    CHECK(!memcmp(frame_bytes, text, sizeof(text)-1));
    CHECK(!supervisor_output(&controller, 1, bytes, sizeof(bytes)));
    CHECK(supervisor_output(t->task, 1, bytes, sizeof(bytes)));
    CHECK(supervisor_captured(1)->bytes == sizeof(bytes) && !supervisor_captured(2)->bytes);
    frame_length = 0; supervisor_observe_end();
    CHECK(frame_length == 2*SUPERVISOR_CAPTURE_BYTES);
    CHECK(!supervisor_report(t->task, text, sizeof(text)-1));
    desktop_clean(p);
    puts("desktop supervisor: forged record framing, PID association, bounded head/tail and streaming SHA-256 PASS");
}
static void test_libc_reports(void)
{
    struct process *p; struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    const char *summaries[] = {
        "case=libc-smoke expected=0 observed=0 checks=100 failures=0 order=2",
        "case=atexit expected=0 observed=0 checks=101 failures=0 order=3",
        "case=destructor expected=0 observed=0 checks=102 failures=0 order=4"
    };
    memset(&libc_reports, 0, sizeof(libc_reports)); libc_reports.pid = p->pid;
    probe_f2_libc_report(&controller, summaries[0], strlen(summaries[0]));
    CHECK(!libc_reports.stages);
    for (unsigned i = 0; i < ARRAY_SIZE(summaries); i++)
        probe_f2_libc_report(t->task, summaries[i], strlen(summaries[i]));
    CHECK(libc_reports.stages == 3 && libc_reports.checks == 102 && !libc_reports.failures && !libc_reports.invalid);
    const char failed[] = "case=uname expected=1 observed=0 line=120 errno=38";
    probe_f2_libc_report(t->task, failed, sizeof(failed)-1); CHECK(libc_reports.failures == 1);
    probe_f2_libc_report(t->task, summaries[2], strlen(summaries[2])); CHECK(libc_reports.invalid);
    const char *bad[] = {
        "case=libc-smoke checks=0 failures=0 order=2",
        "case=libc-smoke checks=4294967296 failures=0 order=2",
        "case=libc-smoke checks=100x failures=0 order=2",
        "case=libc-smoke checks=100 failures=0 order=4",
        "case=libc-smoke checks=100 order=2"
    };
    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        memset(&libc_reports, 0, sizeof(libc_reports)); libc_reports.pid = p->pid;
        probe_f2_libc_report(t->task, bad[i], strlen(bad[i])); CHECK(libc_reports.invalid);
    }
    libc_reports.pid = 0;
    desktop_clean(p);
    puts("libc controller: PID-bound ordered summaries, failures survive exit zero, invalid/missing fields PASS");
}
static void test_libc_write_gate(void)
{
    libc_gate_error = -EROFS; libc_gate_calls = libc_spawn_calls = 0;
    CHECK(probe_f2_libc_smoke() == 1 && libc_gate_calls == 1 && !libc_spawn_calls);
    libc_gate_error = 0; libc_gate_calls = libc_spawn_calls = 0;
    CHECK(probe_f2_libc_smoke() == 1 && libc_gate_calls == 1 && libc_spawn_calls == 1);
    puts("libc controller: qualified write gate before payload launch, refusal never spawns PASS");
}

static struct process *gate_server, *gate_survivor;
static uint32_t gate_control_address, gate_replies, gate_turns, gate_cycle;
static bool gate_bad_status;
static bool gate_input_absent;
static unsigned gate_failure;
static int gate_fixture_spawn(struct process **out)
{
    make_process(proc_supervisor(),out); gate_server=*out; gate_server->pgid=gate_server->pid;
    make_process(gate_server,&gate_survivor); gate_survivor->pgid=gate_survivor->pid;
    struct ciuki_mmap_args a={.size=sizeof(a),.length=PAGE_SIZE,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    gate_control_address=(uint32_t)ua_mmap(gate_server->memory,&a); CHECK(gate_control_address>=CIUKI_MMAP_BASE);
    return (int)gate_server->pid;
}
static void gate_server_summary(unsigned generation,unsigned victim)
{
    char line[241];
    int n=snprintf(line,sizeof(line),"case=native-desktop control=%u generation=%u survivor=%u victim=%u cycle=%u replies=%u keys=%u motion=%u buttons=%u",
        gate_control_address,generation,gate_survivor->pid,victim,gate_cycle,gate_replies,
        gate_cycle==100 && !gate_input_absent ? 2u : 0u,
        gate_cycle==100 && !gate_input_absent ? 1u : 0u,
        gate_cycle==100 && !gate_input_absent ? 2u : 0u);
    native_report(gate_server,line,n);
}
static void gate_demo_summary(struct process *p,unsigned stage,unsigned generation)
{
    char line[241];
    int n=snprintf(line,sizeof(line),"case=native-demo stage=%u turns=%u unauthorized=0 generation=%u",stage,
        p==gate_survivor ? gate_turns : 0,generation);
    native_report(p,line,n);
}
static void gate_schedule(void)
{
    if (!native_reports.active) return;
    CHECK(g_current==&controller);
    if (!native_reports.control) {
        if (gate_failure==4) {
            desktop_remove(gate_server,gate_survivor);
            proc_stop(gate_server,126,0); proc_collect(); return;
        }
        gate_server_summary(0,0);
        if (gate_failure==5) { proc_stop(gate_survivor,126,0); proc_collect(); return; }
        if (gate_failure!=2) gate_demo_summary(gate_survivor,2,0);
    }
    struct gate_control c; CHECK(!ua_read(gate_server->memory,&c,gate_control_address,sizeof(c)));
    if (c.command) {
        CHECK(!ua_write(gate_server->memory,gate_control_address,&(struct gate_control){0},sizeof(c)));
        unsigned victim=native_reports.victim;
        switch(c.command) {
        case GATE_SPAWN: {
            if (gate_failure==6) return;
            struct process *v; make_process(gate_server,&v); v->pgid=v->pid;
            gate_server_summary(c.generation,v->pid); gate_demo_summary(v,1,0); return;
        }
        case GATE_RELEASE: {
            struct process *v=proc_find(victim); CHECK(v);
            gate_demo_summary(v,3,0); proc_stop(v,0,gate_bad_status ? SIGILL : gate_cycle%5==1 ? SIGPIPE : SIGSEGV); proc_collect(); break;
        }
        case GATE_REAP: { struct process *v=proc_find(victim); CHECK(v && v->state==PROC_ZOMBIE); proc_reap(gate_server,v); victim=0; gate_cycle++; break; }
        case GATE_RUN: gate_turns+=100; gate_replies+=100; gate_demo_summary(gate_survivor,2,0); break;
        case GATE_SNAPSHOT: if (gate_failure!=3) gate_demo_summary(gate_survivor,2,c.generation); break;
        case GATE_INTERACT: break;
        default: CHECK(false);
        }
        gate_server_summary(c.generation,victim);
    }
    if (gate_cycle==100) {
        activity.presents++;
        if (!gate_input_absent) { activity.input_events+=5; device.mapped[0]++; }
    }
}
static void test_native_controller(void)
{
    uint8_t prompt_pixels[68]={0};
    struct fb_device saved=device;
    device.present=true;device.mapped=prompt_pixels;device.size=sizeof(prompt_pixels);device.pitch=4;
    probes_operator_mode=true;
    uint32_t baseline=native_digest();prompt_pixels[0]=1;
    CHECK(native_digest()==baseline);prompt_pixels[64]=1;
    CHECK(native_digest()!=baseline);
    probes_operator_mode=false;baseline=native_digest();prompt_pixels[0]=2;
    CHECK(native_digest()!=baseline);
    device=saved;
    uint8_t pixels[4]={0}; device.present=true;device.mapped=pixels;device.size=sizeof(pixels);
    for (unsigned bad=0;bad<2;bad++) {
        gate_bad_status=bad; gate_replies=gate_turns=gate_cycle=0;
        gate_progress=gate_victims=gate_restored=gate_interactions=0;
        gate_simulation=true; g_current=&controller; on_schedule=gate_schedule;
        CHECK(native_crash_isolation()==(int)bad);
        on_schedule=0;gate_simulation=false;
        CHECK(gate_victims==(bad ? 1u : 100u) && gate_progress==(bad ? 0u : 100u));
        CHECK(gate_restored==gate_progress && gate_interactions==(bad ? 0u : 2u));
        CHECK(!native_reports.active && !native_reports.invalid);
    }
    for (unsigned absent=0;absent<2;absent++) {
        probes_operator_mode=true; gate_input_absent=absent; gate_bad_status=false;
        gate_replies=gate_turns=gate_cycle=0;
        gate_progress=gate_victims=gate_restored=gate_interactions=0;
        gate_simulation=true; g_current=&controller; on_schedule=gate_schedule;
        unsigned prompts=operator_prompts;
        CHECK(native_crash_isolation()==(absent ? -ECANCELED : 0));
        CHECK(operator_prompts==prompts+1 && gate_progress==100 && gate_victims==100 && gate_restored==100);
        CHECK(gate_interactions==(absent ? 1u : 2u) && !native_reports.active);
        on_schedule=0;gate_simulation=false;
    }
    probes_operator_mode=false;gate_input_absent=false;
    device.present=false;device.mapped=0;device.size=0;
    puts("native controller: 100 cycles/five kinds, counters, ticks, PID/CR3/pgid, ledgers, interaction, wrong-signal refusal PASS (fake scheduling)");
}
static void test_native_diagnostics(void)
{
    const char *reasons[]={"survivor_timeout","snapshot_timeout","server_exit:32256","survivor_exit:32256","command_timeout"};
    for (unsigned i=0;i<ARRAY_SIZE(reasons);i++) {
        gate_failure=i+2; gate_bad_status=false; gate_replies=gate_turns=gate_cycle=0;
        last_launch[0]=last_step[0]=0;
        g_current=&controller; on_schedule=gate_schedule; print_diagnostics=true;
        CHECK(native_crash_isolation()==1);
        on_schedule=0; print_diagnostics=false;
        CHECK(strstr(gate_failure==6 ? last_step : last_launch,reasons[i]));
        CHECK(gate_failure==6 ? strstr(last_step,"command=1 generation=2 reached=1 live=1 survivor=1 victim=0")!=0 : !last_step[0]);
        CHECK(!native_reports.active && !native_reports.invalid && !pages_used && !desktop_objects.channels);
    }
    gate_failure=0;
    /* Use the production formatter and maximum-width IDs, sequence and tick
     * counts, including the longest rejection name: no silent truncation. */
    memset(&native_reports,0,sizeof(native_reports));
    native_reports.server=CIUKI_ID_MAX; native_reports.reported_survivor=UINT32_MAX;
    native_reports.reported_control=CIUKI_MMAP_LIMIT-PAGE_SIZE; native_reports.reported_stage=UINT32_MAX;
    native_reports.generation=UINT32_MAX; native_invalid("generation_order");
    uint64_t ticks=g_ticks; g_ticks=UINT64_MAX;
    native_launch_record(0,"survivor_timeout"); native_step_record(GATE_INTERACT,UINT32_MAX,"command_timeout");
    g_ticks=ticks;
    puts("native diagnostics: survivor/snapshot deadlines, server/survivor exit 126, command/generation/liveness, first rejection and maximum-width records <=240 bytes PASS");
}

#ifdef CIUKI_DESKTOP_PAYLOAD_BIN
static void standin_schedule(void)
{
    for (unsigned i=1;i<CIUKI_PROCESS_MAX;i++) {
        struct process *p=processes[i];
        if (!p || p->state!=PROC_LIVE || !p->memory || !ua_range(p->memory,DESKTOP_RESULT,sizeof(struct desktop_result),PROT_READ)) continue;
        struct desktop_result r;CHECK(desktop_result_read(p,&r));
        if (r.mode==0) {
            if (r.victim_endpoint!=-1) r.ack=1;
            r.turns++;
        } else if(r.mode==1) {
            r.stage=r.ack ? 2 : 1;
            if(!r.ack) r.turns++;
        } else {
            if(r.ack==1) r.stage=3;
            if(r.ack==2) {proc_stop(p,0,r.mode==3 ? SIGPIPE : SIGSEGV);proc_collect();continue;}
        }
        CHECK(!ua_write(p->memory,DESKTOP_RESULT,&r,sizeof(r)));
    }
}
static void test_standin_controller_records(void)
{
    const char *request="f2:crash-isolation run=12345678 server=standin";
    memcpy(g_boot.test_request,request,strlen(request));g_boot.test_request_len=strlen(request);g_boot.flags=CBI_F_SMBIOS_QEMU;
    g_current=&controller;on_schedule=standin_schedule;
    CHECK(!probe_f2_crash_isolation());on_schedule=0;
    memset(&g_boot,0,sizeof(g_boot));
    CHECK(!pages_used && !desktop_objects.channels && !desktop_objects.grants);
    puts("stand-in production record formatting/selector: 100 mode cycles/progress/restored/display/ledger PASS (fake scheduling)");
}
#endif

static void test_native_reports(void)
{
    struct process *p,*child;make_process(proc_supervisor(),&p);make_process(p,&child); child->pgid=child->pid;
    memset(&native_reports,0,sizeof(native_reports));native_reports.active=true;native_reports.server=p->pid;native_reports.survivor=child->pid;
    const char *good="case=native-demo stage=2 turns=100 unauthorized=0 generation=7";
    native_report(p,good,strlen(good));CHECK(native_reports.invalid);native_reports.invalid=false;
    native_report(child,good,strlen(good));CHECK(!native_reports.invalid && native_reports.survivor_turns==100 && native_reports.survivor_snapshot==7);
    const char *bad[]={"case=native-demo stage=2 turns=99 unauthorized=0 generation=7",
        "case=native-demo stage=2 turns=4294967296 unauthorized=0 generation=7",
        "case=native-demo stage=2 turns=100 unauthorized=1 generation=7",
        "case=native-demo stage=1 turns=100 unauthorized=0 generation=7",
        "case=native-demo stage=2 turns=100 unauthorized=0"};
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++) { native_reports.invalid=false;native_report(child,bad[i],strlen(bad[i]));CHECK(native_reports.invalid); }
    native_reports.active=false;desktop_remove(p,child);desktop_remove(proc_supervisor(),p);
    puts("native summaries: emitting PID, stage, monotonic turns, overflow, unauthorized/missing fields, barrier generation PASS");
}

static void test_native_v3_reports(void)
{
    struct process *server,*child,*other;make_process(proc_supervisor(),&server);make_process(server,&child);make_process(proc_supervisor(),&other);
    child->personality=PROC_WIN32;
    uint32_t flags=g_boot.flags,length=g_boot.test_request_len;char request[64];memcpy(request,g_boot.test_request,sizeof(request));
    const char *selected="f2:crash-isolation run=12345678 server=desktop-v3";
    memcpy(g_boot.test_request,selected,strlen(selected)+1);g_boot.test_request_len=(uint32_t)strlen(selected);g_boot.flags=CBI_F_SMBIOS_QEMU;
    memset(&native_reports,0,sizeof(native_reports));native_reports.active=true;native_reports.server=server->pid;
    memset(&v3_observation,0,sizeof(v3_observation));
    char line[192];unsigned n=(unsigned)snprintf(line,sizeof(line),"case=desktop-v3 phase=1 generation=1 windows=4 errors=0 child=%u",child->pid);
    CHECK(!native_report(other,line,n) && !v3_observation.phase);
    CHECK(native_report(server,line,n) && !native_reports.invalid && v3_observation.phase==1);
    native_report(server,line,n);CHECK(native_reports.invalid); /* duplicate */
    native_reports.invalid=false;child->ppid=other->pid;
    n=(unsigned)snprintf(line,sizeof(line),"case=desktop-v3 phase=2 generation=1 windows=4 errors=0 child=%u",child->pid);
    native_report(server,line,n);CHECK(native_reports.invalid && v3_observation.phase==1);
    child->ppid=server->pid;child->personality=PROC_NATIVE;native_reports.invalid=false;
    native_report(server,line,n);CHECK(native_reports.invalid);
    child->personality=PROC_WIN32;native_reports.invalid=false;
    native_report(server,line,n);CHECK(!native_reports.invalid && v3_observation.phase==2);
    native_reports.invalid=false;g_boot.test_request_len=0;
    native_report(server,line,n);CHECK(native_reports.invalid); /* fixture not selected */
    native_reports.active=false;g_boot.flags=flags;g_boot.test_request_len=length;memcpy(g_boot.test_request,request,sizeof(request));
    desktop_remove(server,child);desktop_remove(proc_supervisor(),other);desktop_remove(proc_supervisor(),server);
    memset(&v3_observation,0,sizeof(v3_observation));
    puts("v3 observer: selected desktop only, exact live Win32 child, ordered nonduplicate phases; stdout not evidence PASS");
}

/* f2-31/f2-32: the optional app-case fields of the server's report (files=1 or note=1). */
static void app_report(struct process *server, struct process *child, uint32_t control, const char *suffix)
{
    char line[241];
    int n=snprintf(line,sizeof(line),"case=native-desktop control=%u generation=1 survivor=%u victim=0 cycle=0 replies=0 keys=0 motion=0 buttons=0%s",
        control,child->pid,suffix);
    native_report(server,line,(unsigned)n);
}
static void pacing_report(struct process *server, struct process *child, uint32_t control,
                          uint32_t keys, uint32_t motion, uint32_t buttons, const char *suffix)
{
    char line[241];
    int n=snprintf(line,sizeof(line),"case=native-desktop control=%u generation=1 survivor=%u victim=0 cycle=0 replies=0 keys=%u motion=%u buttons=%u%s",
        control,child->pid,keys,motion,buttons,suffix);
    native_report(server,line,(unsigned)n);
}
static void test_native_input_records(void)
{
    struct process *server,*child;
    make_process(proc_supervisor(),&server); server->pgid=server->pid;
    make_process(server,&child); child->pgid=child->pid;
    struct ciuki_mmap_args a={.size=sizeof(a),.length=PAGE_SIZE,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    uint32_t control=(uint32_t)ua_mmap(server->memory,&a); CHECK(control>=CIUKI_MMAP_BASE);
    memset(&native_reports,0,sizeof(native_reports)); native_reports.server=server->pid;
    unsigned before=input_records;
    pacing_report(server,child,control,0,0,0,""); CHECK(input_records==before);
    native_reports.active=true;
    /* Unregistered processes cannot supply a baseline. */
    pacing_report(proc_supervisor(),child,control,0,0,0,""); CHECK(input_records==before);
    pacing_report(server,child,control,0,0,0,"");
    CHECK(input_records==++before && native_reports.input_recorded && !native_reports.invalid);
    CHECK(!strcmp(last_input_record,"case=gate server=desktop keys=0 motion=0 buttons=0"));
    pacing_report(server,child,control,0,0,0,""); CHECK(input_records==before);
    /* Both axes and separate button edges become fresh controller records. */
    pacing_report(server,child,control,0,1,0,""); CHECK(input_records==++before);
    pacing_report(server,child,control,0,2,0,""); CHECK(input_records==++before);
    pacing_report(server,child,control,0,2,1,""); CHECK(input_records==++before);
    pacing_report(server,child,control,0,2,2,""); CHECK(input_records==++before);
    pacing_report(server,child,control,2,2,2,""); CHECK(input_records==++before);
    /* F1-59: faster f1-57 delivery can expose stage 6/A while the runner
     * still owes its final pointer move and button down/up. */
    pacing_report(server,child,control,4,46,10," shell=1 shell_stage=6");
    CHECK(input_records==++before && native_reports.shell && !native_reports.invalid);
    CHECK(shell_probe_pending(native_reports.shell_stage,native_reports.keys,native_reports.buttons));
    pacing_report(server,child,control,4,50,11," shell=1 shell_stage=6");
    CHECK(input_records==++before && shell_probe_pending(6,4,native_reports.buttons));
    pacing_report(server,child,control,4,50,12," shell=1 shell_stage=6");
    CHECK(input_records==++before && !shell_probe_pending(6,4,native_reports.buttons));
    CHECK(shell_probe_pending(5,4,12) && shell_probe_pending(6,2,12));
    pacing_report(server,child,control,2,1,2,""); CHECK(native_reports.invalid && input_records==before);
    pacing_report(server,child,control,2,3,2,""); CHECK(input_records==before);
    /* Full validation includes app fields, even when counters are valid. */
    memset(&native_reports,0,sizeof(native_reports)); native_reports.active=true; native_reports.server=server->pid;
    pacing_report(server,child,control,0,0,0," files=1 checkpoint=2");
    CHECK(native_reports.invalid && !native_reports.input_recorded && input_records==before);
    memset(&native_reports,0,sizeof(native_reports)); native_reports.active=true; native_reports.server=server->pid;
    pacing_report(server,child,control,UINT32_MAX,UINT32_MAX,UINT32_MAX,"");
    CHECK(input_records==++before && !native_reports.invalid);
    CHECK(strstr(last_input_record,"keys=4294967295 motion=4294967295 buttons=4294967295"));
    native_reports.active=false; desktop_remove(server,child); desktop_remove(proc_supervisor(),server);
    puts("native input records: zero baseline, changed axes/buttons/keys, deduplication, authority, full validation and maximum-width envelope PASS");
}

static void test_native_app_reports(void)
{
    struct process *server,*child;
    make_process(proc_supervisor(),&server); server->pgid=server->pid;
    make_process(server,&child); child->pgid=child->pid;
    struct ciuki_mmap_args a={.size=sizeof(a),.length=PAGE_SIZE,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    uint32_t control=(uint32_t)ua_mmap(server->memory,&a); CHECK(control>=CIUKI_MMAP_BASE);
    uint8_t pixels[4]={0}; struct fb_device saved=device;
    device.present=true;device.mapped=pixels;device.size=sizeof(pixels);
    struct { const char *suffix; uint32_t app; const char *invalid; } cases[]={
        {"",0,0}, {" files=1 checkpoint=0",1,0}, {" note=1 checkpoint=0",2,0}, {" note=1 checkpoint=1",2,0},
        {" files=1 note=1 checkpoint=0",0,"app"}, {" note=2 checkpoint=0",0,"note"}, {" files=2 checkpoint=0",0,"files"},
        {" note=1",0,"checkpoint"}, {" note=1 checkpoint=2",0,"checkpoint_order"}, {" files=1 checkpoint=6",0,"checkpoint_order"}};
    for (unsigned i=0;i<ARRAY_SIZE(cases);i++) {
        memset(&native_reports,0,sizeof(native_reports));native_reports.active=true;
        native_reports.server=server->pid;native_reports.survivor=child->pid;
        if (i==3) app_report(server,child,control," note=1 checkpoint=0");
        app_report(server,child,control,cases[i].suffix);
        if (cases[i].invalid) CHECK(native_reports.invalid && !strcmp(native_reports.invalid_check,cases[i].invalid));
        else CHECK(!native_reports.invalid && native_reports.app==cases[i].app);
    }
    /* Checkpoints advance one at a time, hash the framebuffer once and keep the digest. */
    memset(&native_reports,0,sizeof(native_reports));native_reports.active=true;
    native_reports.server=server->pid;native_reports.survivor=child->pid;
    app_report(server,child,control," note=1 checkpoint=0"); pixels[0]=7;
    app_report(server,child,control," note=1 checkpoint=1");
    uint32_t first=native_reports.checkpoint_digest[1]; CHECK(native_reports.checkpoint==1 && first==native_digest());
    pixels[0]=9; app_report(server,child,control," note=1 checkpoint=1"); CHECK(native_reports.checkpoint_digest[1]==first);
    app_report(server,child,control," note=1 checkpoint=2"); CHECK(native_reports.checkpoint==2 && native_reports.checkpoint_digest[2]==native_digest());
    CHECK(!native_reports.invalid);
    /* One run never switches case, whatever the order. */
    app_report(server,child,control," files=1 checkpoint=2"); CHECK(native_reports.invalid && !strcmp(native_reports.invalid_check,"app_changed"));
    CHECK(!strcmp(native_app_case(),"note") && native_app_checkpoints()==3);
    native_reports.app=1; CHECK(!strcmp(native_app_case(),"files") && native_app_checkpoints()==5);
    device=saved; native_reports.active=false;
    desktop_remove(server,child);desktop_remove(proc_supervisor(),server);
    puts("native app reports: files=1/note=1 fields, checkpoint order/range, digests, no case switching, other desktop cases unaffected PASS");
}

static struct proc_thread *native_group(struct process *parent, struct process **out)
{
    struct ciuki_file file=fixture_file(); struct proc_strings *strings=proc_strings_new();
    CHECK(strings && !proc_strings_add(strings,"test",5,false));
    CHECK(proc_spawn_file(parent,&file,strings,0,0,CIUKI_SPAWN_NEW_GROUP,0,out)>0);
    proc_strings_free(strings);
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) {
        struct proc_thread *t=proc_thread_slot(i);
        if (t && t->process==*out) return t;
    }
    CHECK(false); return 0;
}
static FILE *captured_reports;
static struct proc_thread *captured_server, *captured_survivor;
static char captured_first[241];
static void captured_report(void)
{
    char line[242]; CHECK(fgets(line,sizeof(line),captured_reports));
    size_t length=strlen(line); CHECK(length && line[length-1]=='\n'); line[--length]=0;
    CHECK(length<=CIUKI_PROBE_REPORT_MAX);
    struct proc_thread *t=!strncmp(line,"case=native-demo ",17) ? captured_survivor : captured_server;
    if (!captured_first[0]) strcpy(captured_first,line);
    probe_f2_libc_report(t->task,line,(uint32_t)length);
}
static void captured_snapshot(void) { captured_report(); captured_report(); }
static void test_captured_handshake(const char *path, const char *setup_path)
{
    struct process *owner,*server,*survivor;
    next_pid=40; CHECK(!proc_prepare(proc_supervisor(),&owner)); proc_publish(owner,0);
    captured_server=native_group(proc_supervisor(),&server);
    server->ppid=owner->pid; /* the production controller re-parents its desktop */
    captured_survivor=native_group(server,&survivor);
    CHECK(server->pid==41 && survivor->pid==42 && survivor->ppid==server->pid && server->ppid==owner->pid);
    CHECK(server->pgid==server->pid && survivor->pgid==survivor->pid);
    int output=surface_create(server,320,320,CIUKI_SURFACE_XRGB8888); CHECK(output>=0);
    int32_t pixels=surface_map(server,output,PROT_READ|PROT_WRITE); CHECK(pixels>=0);
    CHECK((uint32_t)pixels==CIUKI_MMAP_BASE+CIUKI_TLS_SIZE);
    struct ciuki_mmap_args a={.size=sizeof(a),.length=PAGE_SIZE,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    int32_t control=ua_mmap(server->memory,&a); CHECK(control>=0);
    CHECK((uint32_t)control==(uint32_t)pixels+320*320*4 && (uint32_t)control>=CIUKI_MMAP_BASE);
    CHECK(ua_range(server->memory,(uint32_t)control,PAGE_SIZE,PROT_READ|PROT_WRITE));
    CHECK(ua_find(server->memory,(uint32_t)control)->kind==UA_ANON);
    struct gate_control initial; CHECK(!ua_read(server->memory,&initial,(uint32_t)control,sizeof(initial)) && !initial.command && !initial.generation);
    memset(&native_reports,0,sizeof(native_reports)); native_reports.active=true; native_reports.server=server->pid;
    captured_reports=fopen(path,"rb"); CHECK(captured_reports); captured_first[0]=0;
    captured_report(); captured_report();
    CHECK(!native_reports.invalid && native_reports.control==(uint32_t)control && native_reports.survivor==survivor->pid && native_reports.survivor_stage==2);
    CHECK(!native_reports.generation && !native_reports.cycles && !native_reports.survivor_turns);
    g_current=&controller; on_schedule=captured_snapshot; uint32_t generation=0;
    CHECK(native_command(server,GATE_SNAPSHOT,&generation)); on_schedule=0;
    CHECK(!native_reports.invalid && generation==1 && native_reports.survivor_snapshot==1 && fgetc(captured_reports)==EOF);
    CHECK(!fclose(captured_reports));
    print_diagnostics=true; native_launch_record(g_ticks,0);
    if (setup_path) {
        memset(&native_reports,0,sizeof(native_reports)); native_reports.active=true; native_reports.server=server->pid;
        captured_reports=fopen(setup_path,"rb"); CHECK(captured_reports); captured_report();
        CHECK(!native_reports.invalid && native_reports.setup_step==GATE_SETUP_INPUT && native_reports.setup_error==EIO);
        CHECK(fgetc(captured_reports)==EOF && !fclose(captured_reports));
        native_launch_record(g_ticks,"survivor_timeout");
    }
    /* Reject actual captured text against changed production process/mapping
     * state, rather than inventing a report the payload could never emit. */
    const char *rejections[]={"survivor_parent","survivor_group","control_range"};
    for (unsigned i=0;i<ARRAY_SIZE(rejections);i++) {
        memset(&native_reports,0,sizeof(native_reports)); native_reports.active=true; native_reports.server=server->pid;
        if (i==0) survivor->ppid=owner->pid;
        if (i==1) survivor->pgid=server->pgid;
        if (i==2) CHECK(!ua_mprotect(server->memory,(uint32_t)control,PAGE_SIZE,PROT_READ));
        probe_f2_libc_report(captured_server->task,captured_first,strlen(captured_first));
        CHECK(native_reports.invalid && !strcmp(native_reports.invalid_check,rejections[i]));
        native_launch_record(g_ticks,"survivor_timeout");
        probe_f2_libc_report(captured_server->task,"bad",3);
        CHECK(!strcmp(native_reports.invalid_check,rejections[i]));
        survivor->ppid=server->pid; survivor->pgid=survivor->pid;
    }
    native_reports.invalid=false; native_reports.invalid_check=0; native_reports.control=(uint32_t)control;
    native_reports.survivor=survivor->pid;
    CHECK(!ua_munmap(server->memory,(uint32_t)control,PAGE_SIZE));
    CHECK(!native_command(server,GATE_SNAPSHOT,&generation) && !strcmp(native_command_failure,"control_write"));
    native_step_record(GATE_SNAPSHOT,generation,native_command_failure);
    print_diagnostics=false; native_reports.active=false;
    desktop_remove(server,survivor); desktop_remove(owner,server); desktop_remove(proc_supervisor(),owner);
    puts("captured production desktop/demo -> probe_f2_libc_report -> native_report: reparenting, new spawn group, first generation/cycle, mmap after TLS/surface, CONFIGURE and snapshot PASS");
}

#ifdef CIUKI_DESKTOP_SPAWN_HOST
/* Native desktop syscall seam: use production channel_pair, descriptor flags,
 * supervisor preparation and proc_spawn, including the copied 32-bit args.
 * Physical RAM, scheduling and executable file I/O remain host fixtures. */
static struct process *spawn_host_owner, *spawn_host_server;
static struct proc_thread *spawn_host_thread;
static struct test_object spawn_host_stdio[3];
static int spawn_host_open(void *cwd, const char *path, struct ciuki_file *file)
{
    (void)cwd;
    CHECK(!strcmp(path,"/Programs/desktop") || !strcmp(path,"/Programs/demo"));
    *file=fixture_file(); return 0;
}
static int spawn_host_install_stdio(struct process *p)
{
    for (unsigned i=0;i<3;i++) {
        spawn_host_stdio[i]=(struct test_object){ .object={obj_retain,obj_release,true}, .id=i };
        object_refs[i]=1; p->fds[i].object=&spawn_host_stdio[i].object;
    }
    return 0;
}
static struct proc_thread *spawn_host_find_thread(struct process *p)
{
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) {
        struct proc_thread *t=proc_thread_slot(i);
        if (t && t->process==p) return t;
    }
    CHECK(false); return 0;
}
/* Grant numbers belong to this process table, not to the host fd table. */
static int spawn_host_kind_fd(enum desktop_kind kind)
{
    int found=-1;
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) {
        struct desktop_description *d=desktop_fd(spawn_host_server,(int32_t)i);
        if (d && d->kind==kind) { CHECK(found<0); found=(int)i; }
    }
    CHECK(found>=3); return found;
}
int desktop_spawn_host_display_fd(void) { return spawn_host_kind_fd(DESKTOP_DISPLAY); }
int desktop_spawn_host_input_fd(void) { return spawn_host_kind_fd(DESKTOP_INPUT); }
void desktop_spawn_host_shift_grants(void)
{
    /* Exercise explicit desktop arguments with holes below the grants. */
    const enum desktop_kind kinds[]={DESKTOP_DISPLAY,DESKTOP_INPUT};
    for (unsigned i=0;i<ARRAY_SIZE(kinds);i++) {
        int source=spawn_host_kind_fd(kinds[i]), target=11+(int)i*2;
        CHECK(!spawn_host_server->fds[target].object);
        CHECK(desktop_dup(spawn_host_server,source,target,true,FD_CLOEXEC)==target);
        CHECK(!desktop_close(spawn_host_server,source));
        CHECK(spawn_host_kind_fd(kinds[i])==target);
    }
}
/* Model the shell's optional device queries in the guest table. Linux opens
 * must never hand host fd numbers to the guest close/fcntl seams. devctl is
 * deliberately ENOSYS in the gate fixture; other files remain unavailable. */
static unsigned spawn_host_node_refs, spawn_host_node_opens;
static void spawn_host_node_retain(struct proc_object *object)
{ (void)object; CHECK(spawn_host_node_refs); spawn_host_node_refs++; }
static void spawn_host_node_release(struct proc_object *object)
{ (void)object; CHECK(spawn_host_node_refs); spawn_host_node_refs--; }
static struct proc_object spawn_host_node={spawn_host_node_retain,spawn_host_node_release,false};
int desktop_spawn_host_open_node(const char *path)
{
    if (strcmp(path,"/dev/net") && strcmp(path,"/dev/media") && strcmp(path,"/dev/mixer")) return -ENOENT;
    int fd=desktop_fd_slot(spawn_host_server,0);
    if (fd<0) return fd;
    spawn_host_node_refs++; spawn_host_node_opens++;
    spawn_host_server->fds[fd]=(struct proc_fd){&spawn_host_node,FD_CLOEXEC};
    return fd;
}
unsigned desktop_spawn_host_node_opens(void) { return spawn_host_node_opens; }
void desktop_spawn_host_init(int gate)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    next_pid=40; CHECK(!proc_prepare(proc_supervisor(),&spawn_host_owner)); proc_publish(spawn_host_owner,0);
    static const struct ciuki_file_ops files={ .open=spawn_host_open };
    static const struct supervisor_io_ops io={ .stdio=spawn_host_install_stdio };
    proc_set_file_ops(&files); supervisor_set_io_ops(&io);
    if (gate) CHECK(supervisor_spawn_desktop_probe(&spawn_host_server)==41);
    else {
        const char *argv[]={"desktop",0};
        CHECK(supervisor_spawn("/Programs/desktop",argv,0,true,&spawn_host_server)==41);
    }
    if (gate) spawn_host_server->ppid=spawn_host_owner->pid;
    CHECK(desktop_spawn_host_display_fd()!=desktop_spawn_host_input_fd());
    spawn_host_thread=spawn_host_find_thread(spawn_host_server);
    CHECK(spawn_host_thread->task->prio==P_INTERACTIVE);
    desktop_select(spawn_host_thread);
}
int desktop_spawn_host_output(void)
{
    bool authority=false;
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++)
        authority |= proc_authority_object(spawn_host_server->fds[i].object);
    CHECK(authority);
    int fd=surface_create(spawn_host_server,320,320,CIUKI_SURFACE_XRGB8888);
    CHECK(fd>=3 && desktop_fd(spawn_host_server,fd)->kind==DESKTOP_SURFACE);
    CHECK(surface_map(spawn_host_server,fd,PROT_READ|PROT_WRITE)==CIUKI_MMAP_BASE+CIUKI_TLS_SIZE);
    return fd;
}
uint32_t desktop_spawn_host_control(void)
{
    struct ciuki_mmap_args a={.size=sizeof(a),.length=PAGE_SIZE,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    int32_t address=ua_mmap(spawn_host_server->memory,&a);
    CHECK(address==CIUKI_MMAP_BASE+CIUKI_TLS_SIZE+320*320*4);
    return (uint32_t)address;
}
void desktop_spawn_host_input(const struct input_event *in, struct ciuki_input_event *out)
{
    desktop_input_event(in, (uint32_t)in->lost_count, (uint32_t)in->value, out);
}
void desktop_spawn_host_no_input(void)
{
    input_state.active=false; input_state.quarantined=true;
    usb_ready=false; input_count=input_at=0;
}
void desktop_spawn_host_attach_keyboard(void)
{
    usb_ready=true;
    input_count=3; input_at=0;
    input_events[0]=(struct input_event){.type=INPUT_KEY,.code=INPUT_KEY_A,.value=1,.source=INPUT_USB,.generation=23,.sequence=1};
    input_events[1]=(struct input_event){.type=INPUT_TEXT,.code='a',.source=INPUT_USB,.generation=23,.sequence=2};
    input_events[2]=(struct input_event){.type=INPUT_KEY,.code=INPUT_KEY_A,.value=0,.source=INPUT_USB,.generation=23,.sequence=3};
}
int desktop_spawn_host_read_input(int fd, struct ciuki_input_event *events, uint32_t capacity)
{
    CHECK(fd==desktop_spawn_host_input_fd());
    desktop_select(spawn_host_thread);
    int n=grant_input_read(spawn_host_server,fd,BUFFER,capacity);
    if (n>0) CHECK(!ua_read(spawn_host_server->memory,events,BUFFER,(uint32_t)n*sizeof(*events)));
    return n;
}
int desktop_spawn_host_pair(int32_t pair[2])
{
    int err=channel_pair(spawn_host_server,pair);
    if (!err) {
        CHECK(desktop_fcntl(spawn_host_server,pair[0],F_GETFD,0)==FD_CLOEXEC);
        CHECK(desktop_fcntl(spawn_host_server,pair[1],F_GETFD,0)==FD_CLOEXEC);
        struct ciuki_spawn_fd mapping={pair[1],3};
        CHECK(proc_fd_validate(spawn_host_server,&mapping,1)==-EBADF);
    }
    return err;
}
int desktop_spawn_host_fcntl(int fd,int cmd,uint32_t arg)
{ return desktop_fcntl(spawn_host_server,fd,(uint32_t)cmd,arg); }
int desktop_spawn_host_close(int fd)
{ return desktop_close(spawn_host_server,fd); }
static uint32_t spawn_host_text(const char *text,uint32_t *at)
{
    uint32_t address=*at, length=(uint32_t)strlen(text)+1;
    CHECK(address+length<=BUFFER+PAGE_SIZE);
    CHECK(!ua_write(spawn_host_server->memory,address,text,length)); *at+=length;
    return address;
}
static unsigned spawn_host_vector(char *const text[],uint32_t vector,uint32_t *at)
{
    unsigned count=0;
    while (text[count]) {
        CHECK(count<8);
        put_word(spawn_host_server->memory,vector+4*count,spawn_host_text(text[count],at)); count++;
    }
    put_word(spawn_host_server->memory,vector+4*count,0); return count;
}
int desktop_spawn_host_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *mapping,uint32_t count,uint32_t flags)
{
    desktop_select(spawn_host_thread);
    uint32_t text=BUFFER+256;
    struct ciuki_spawn_args args={.size=sizeof(args),.path=spawn_host_text(path,&text),
        .argv=BUFFER+64,.envp=BUFFER+104,.fd_list=BUFFER+144,.fd_count=count,.flags=flags};
    unsigned argc=spawn_host_vector(argv,args.argv,&text), envc=spawn_host_vector(env,args.envp,&text);
    CHECK(count==1 && !ua_write(spawn_host_server->memory,args.fd_list,mapping,sizeof(*mapping)));
    CHECK(!ua_write(spawn_host_server->memory,BUFFER,&args,sizeof(args)));
    int pid=proc_spawn(BUFFER);
    if (pid<0) return pid;
    struct process *child=proc_find((uint32_t)pid); CHECK(child);
    CHECK(child->ppid==spawn_host_server->pid && child->pgid==child->pid);
    CHECK(child->fds[3].object==spawn_host_server->fds[mapping->source].object && !child->fds[3].flags);
    CHECK(desktop_fd(child,3)->kind==DESKTOP_CHANNEL);
    struct desktop_description *source=desktop_fd(spawn_host_server,mapping->source);
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) {
        struct desktop_description *d=desktop_fd(spawn_host_server,(int32_t)i);
        if (d && d!=source && d->kind==DESKTOP_CHANNEL && d->u.endpoint.channel==source->u.endpoint.channel)
            CHECK(spawn_host_server->fds[i].flags==FD_CLOEXEC);
    }
    for (unsigned i=0;i<3;i++) CHECK(child->fds[i].object==spawn_host_server->fds[i].object);
    for (unsigned i=4;i<CIUKI_OPEN_MAX;i++) CHECK(!child->fds[i].object);
    /* The channel pair no longer lands on fds 3 and 4 (the shell opens its nodes before the first
     * spawn since f2-56). The mapped client end is deliberately inheritable with FD_CLOEXEC cleared
     * (desktop.c clears it on pair[1] and closes it afterwards); every peer of its channel stays
     * FD_CLOEXEC. Other objects are never inherited: the child table check above proves it. */
    CHECK(!spawn_host_server->fds[mapping->source].flags && source->object.inheritable);
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) {
        struct desktop_description *d=desktop_fd(spawn_host_server,(int32_t)i);
        if (d && d!=source && d->kind==DESKTOP_CHANNEL && d->u.endpoint.channel==source->u.endpoint.channel)
            CHECK(spawn_host_server->fds[i].flags==FD_CLOEXEC);
    }
    struct proc_thread *t=spawn_host_find_thread(child); uint32_t at=t->task->saved_esp;
    CHECK(get_word(child->memory,at)==argc); at+=4;
    for (unsigned group=0;group<2;group++) {
        char *const *expected=group ? env : argv; unsigned n=group ? envc : argc;
        for (unsigned i=0;i<n;i++,at+=4) {
            char actual[128]; size_t length=strlen(expected[i])+1; CHECK(length<=sizeof(actual));
            CHECK(!ua_read(child->memory,actual,get_word(child->memory,at),length) && !strcmp(actual,expected[i]));
        }
        CHECK(!get_word(child->memory,at)); at+=4;
    }
    return pid;
}
void desktop_spawn_host_reap(int pid)
{
    struct process *child=proc_find((uint32_t)pid); CHECK(child);
    desktop_remove(spawn_host_server,child); desktop_select(spawn_host_thread);
}
void desktop_spawn_host_finish(void)
{
    g_current=&controller;
    for (unsigned i=1;i<CIUKI_PROCESS_MAX;i++) {
        struct process *p=processes[i];
        if (p && p->ppid==spawn_host_server->pid) desktop_remove(spawn_host_server,p);
    }
    desktop_remove(spawn_host_server->ppid==spawn_host_owner->pid ? spawn_host_owner : proc_supervisor(),spawn_host_server);
    desktop_remove(proc_supervisor(),spawn_host_owner);
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1);
    CHECK(!desktop_objects.descriptions && !desktop_objects.channels && !desktop_objects.grants);
    CHECK(!spawn_host_node_refs);
    for (unsigned i=0;i<3;i++) CHECK(!object_refs[i]);
    free(ram);
    puts("native desktop -> production channel_pair/fcntl/proc_spawn: CLOEXEC rejection, explicit fd 3, argv/env, new group, grants excluded, zero resources PASS");
}
#define main desktop_kernel_fixture_main
#endif
#ifdef CIUKI_DESKTOP_REAL_INPUT
#define main desktop_input_fixture_main
#endif
#ifndef CIUKI_DESKTOP_HOST_MAIN
#define CIUKI_DESKTOP_HOST_MAIN main
#endif
int CIUKI_DESKTOP_HOST_MAIN(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller;
    proc_init();
    if (argc==2 && !strcmp(argv[1],"--isolation-records")) {
        test_native_v3_reports();
        uint8_t pixels[4]={0};device.present=true;device.mapped=pixels;device.size=sizeof(pixels);
        gate_simulation=true;on_schedule=gate_schedule;routing_records=true;
        rec_emit("crash-isolation","BEGIN","server=desktop");
        CHECK(!native_crash_isolation());
        on_schedule=0;gate_simulation=routing_records=false;
        CHECK(!pages_used && !g_user_mappings && heap_blocks==1);free(ram);return 0;
    }
    if ((argc==3 || argc==4) && !strcmp(argv[1],"--native-handshake")) {
        test_captured_handshake(argv[2],argc==4 ? argv[3] : 0);
        CHECK(!pages_used && !g_user_mappings && heap_blocks==1); free(ram); return 0;
    }
    test_desktop_surfaces(); test_desktop_channels(); test_desktop_scheduler_hints(); test_desktop_channel_rollback(); test_desktop_waits(); test_desktop_grants(); test_desktop_present(); test_setup_bootstrap(); test_pager_boot_fallback(); test_desktop_restart(); test_desktop_capture();
    test_native_reports(); test_native_v3_reports(); test_native_input_records(); test_native_app_reports(); test_native_controller(); test_native_diagnostics();
#ifdef CIUKI_DESKTOP_PAYLOAD_BIN
    test_standin_controller_records();
#endif
    test_libc_reports();
    test_libc_write_gate();
    if (argc == 2) {
        FILE *file = fopen(argv[1], "rb"); CHECK(file);
        CHECK(!fseek(file, 0, SEEK_END)); long size = ftell(file); CHECK(size == 8196);
        rewind(file); uint8_t *bytes = malloc((size_t)size); CHECK(bytes);
        CHECK(fread(bytes, 1, (size_t)size, file) == (size_t)size); fclose(file);
        struct ciuki_file f = { .cookie = bytes, .bytes = (uint32_t)size, .read = real_payload_read };
        struct elf_image im; CHECK(!elf_validate(&f, &im) && im.count == 2 && im.bytes == 2*PAGE_SIZE);
        struct uaddr u; CHECK(!ua_init(&u, &u) && !elf_load(&f, &im, &u));
        CHECK(!get_word(&u, CIUKI_IMAGE_BASE + PAGE_SIZE));
        ua_destroy(&u); free(bytes);
        puts("desktop NASM ELF: production validation/load PASS (execution not run)");
    }
    CHECK(!pages_used && !g_user_mappings && heap_blocks == 1);
    printf("desktop final: pages=0 objects=0 messages=0 grants=0 checks=%u PASS\n", checks);
    free(ram);
    return 0;
}

void display_frame_complete(uint32_t gen, bool full_frame)
{ CHECK(gen == 1); display_uploads++; display_full_frame=full_frame; if (full_frame) display_frames++; }
uint32_t display_change_counter(void) { return 42; }
