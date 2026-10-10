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
static char last_launch[241], last_step[241];
static unsigned gate_progress, gate_victims, gate_restored, gate_interactions;

static struct resource desktop_resource = { .generation = 1, .state = RS_ACTIVE };
static struct i8042_stats input_state = { .active = true, .generation = 1 };
static struct input_stats input_status;
static struct input_event input_events[CIUKI_INPUT_QUEUE_MAX];
static unsigned input_count, input_at, pipe_calls, frame_count;
static uint8_t frame_bytes[8192];
static unsigned frame_length;
static char frame_probe[24];
void klog(const char *fmt, ...) { (void)fmt; }
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[241] = { 0 };
    va_list ap; va_start(ap, fmt);
    int n = fmt ? kvsnprintf(extra, sizeof(extra), fmt, ap) : 0;
    va_end(ap);
    CHECK(n >= 0 && n < (int)sizeof(extra));
    char record[256];
    CHECK(ksnprintf(record,sizeof(record),"CIUKI_TEST v=1 run=00000000 seq=4294967295 probe=%s event=%s%s%s",probe,event,n ? " " : "",extra)<=240);
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
        if (strstr(extra,"case=interaction ")) gate_interactions++;
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
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached) { (void)phys; (void)bytes; (void)uncached; return 0; }
void i8042_snapshot(struct i8042_stats *out) { *out = input_state; }
void input_snapshot(struct input_stats *out) { *out = input_status; out->pending = input_count - input_at; }
bool input_read(struct input_event *out)
{
    if (input_at == input_count) return false;
    *out = input_events[input_at++];
    return true;
}
int supervisor_spawn_standin(struct process *parent, struct process **out) { (void)parent; (void)out; return -ENOENT; }

#include "../../../src/kernel/drivers/fbdev.c"
#include "../../../src/kernel/proc/surface.c"
#include "../../../src/kernel/proc/channel.c"
#include "../../../src/kernel/proc/grants.c"
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
static int report_fixture_spawn(const char *path, const char *const argv[], const char *cwd,
                                bool desktop, struct process **out)
{
    CHECK(libc_gate_calls == 1 && !libc_gate_error && !strcmp(path, "/bin/libc_smoke"));
    CHECK(argv && !strcmp(argv[0], "libc_smoke") && !argv[1] && !cwd && !desktop && out);
    libc_spawn_calls++; return -ENOENT;
}
static int gate_fixture_spawn(struct process **out);
#define supervisor_spawn_desktop_probe gate_fixture_spawn
#define supervisor_spawn_standin report_fixture_standin
#define supervisor_spawn report_fixture_spawn
#include "../../../src/kernel/probes/f2_probes_desktop.c"
#undef supervisor_spawn
#undef supervisor_spawn_standin
#undef supervisor_spawn_desktop_probe

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
        on_schedule = desktop_interrupt_wait;
        CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EINTR);
        CHECK(p->memory->pins == pins && !t->cleanup && !t->operation);
        desktop_clean(p);
    }
    puts("desktop waits: immutable send snapshot, close while waiting, EINTR and pin/reference cleanup PASS");
}

static void test_desktop_grants(void)
{
    struct process *p, *child;
    struct proc_thread *t = make_process(proc_supervisor(), &p); desktop_select(t);
    int32_t grants[2]; CHECK(!grants_install(p, grants));
    CHECK(grants_install(p, grants) == -EPERM);
    struct ciuki_display_info info;
    CHECK(grant_display_info(p, 127, &info) == -EBADF);
    CHECK(grant_display_info(p, grants[1], &info) == -EBADF);
    CHECK(grant_display_info(p, grants[0], &info) == -ENODEV);
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
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -EIO);
    input_state.quarantined = false; input_state.active = false;
    CHECK(grant_input_read(p, grants[1], BUFFER, 1) == -ENODEV); input_state.active = true;
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
    desktop_clean(p);
    puts("desktop grants: inheritance/transfer forbidden, duplication, authority, input conversion/RESYNC PASS");
}

/* Independent pixel oracle enumerates source coordinates, not clipped rows. */
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
        struct ciuki_display_info info; CHECK(!grant_display_info(p, grants[0], &info) && info.generation == 1);
        desktop_resource.state = RS_QUARANTINED;
        CHECK(grant_present(p, grants[0], fd, &invalid) == -EIO);
        desktop_resource.state = RS_ACTIVE;
    }
    int wide_fd = surface_create(p, 2048, 2, 1); CHECK(wide_fd >= 0);
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
    CHECK(!grant_present(p, grants[0], wide_fd, &whole));
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
        gate_control_address,generation,gate_survivor->pid,victim,gate_cycle,gate_replies,gate_cycle==100 ? 2u : 0u,gate_cycle==100 ? 1u : 0u,gate_cycle==100 ? 2u : 0u);
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
    if (gate_cycle==100) { activity.presents++; activity.input_events+=5; device.mapped[0]++; }
}
static void test_native_controller(void)
{
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

int main(int argc, char **argv)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller;
    proc_init();
    if ((argc==3 || argc==4) && !strcmp(argv[1],"--native-handshake")) {
        test_captured_handshake(argv[2],argc==4 ? argv[3] : 0);
        CHECK(!pages_used && !g_user_mappings && heap_blocks==1); free(ram); return 0;
    }
    test_desktop_surfaces(); test_desktop_channels(); test_desktop_channel_rollback(); test_desktop_waits(); test_desktop_grants(); test_desktop_present(); test_desktop_capture();
    test_native_reports(); test_native_controller(); test_native_diagnostics();
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
