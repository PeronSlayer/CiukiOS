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

struct ciuki_boot_info g_boot;
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
    char extra[241];
    va_list ap; va_start(ap, fmt);
    int n = fmt ? vsnprintf(extra, sizeof(extra), fmt, ap) : 0;
    va_end(ap);
    CHECK(n >= 0 && n < 155); /* conservative allowance for fixed record prefix */
    frame_count++;
    CHECK(!strcmp(event, "DATA"));
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
int main(int argc, char **argv)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller;
    proc_init();
    test_desktop_surfaces(); test_desktop_channels(); test_desktop_channel_rollback(); test_desktop_waits(); test_desktop_grants(); test_desktop_present(); test_desktop_capture();
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
