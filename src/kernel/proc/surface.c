/* Shared, zeroed surface backing; descriptions and mappings own separate refs.
 * See execution-abi.md, Desktop surfaces, and build/f2-05/research.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS (file_description_count ? file_description_count() : 0)
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>

struct surface {
    struct ciuki_surface_info info;
    uint32_t references;
    uint32_t page_lists[CIUKI_SURFACE_MAX / PAGE_SIZE / (PAGE_SIZE / sizeof(uint32_t))];
};
struct desktop_ledger desktop_objects;

void desktop_snapshot(struct desktop_ledger *out) { *out = desktop_objects; }

static void description_retain(struct proc_object *o)
{
    struct desktop_description *d = (struct desktop_description *)o;
    d->references++;
}

static void description_release(struct proc_object *o)
{
    struct desktop_description *d = (struct desktop_description *)o;
    if (--d->references)
        return;
    if (d->kind == DESKTOP_SURFACE) surface_release(d->u.surface);
    else if (d->kind == DESKTOP_CHANNEL) channel_release(d);
    else grant_release(d);
    desktop_objects.descriptions--;
    kfree(d);
}

struct desktop_description *desktop_description_new(enum desktop_kind kind, uint32_t flags)
{
    if (desktop_objects.descriptions + FILE_OBJECTS >= CIUKI_OPEN_DESCRIPTION_MAX)
        return 0;
    struct desktop_description *d = kzalloc(sizeof(*d));
    if (!d)
        return 0;
    d->object = (struct proc_object){ description_retain, description_release,
        kind == DESKTOP_SURFACE || kind == DESKTOP_CHANNEL };
    d->references = 1;
    d->kind = kind;
    d->flags = flags;
    desktop_objects.descriptions++;
    return d;
}

struct desktop_description *desktop_fd(const struct process *p, int32_t fd)
{
    if (!p || !p->fds || fd < 0 || fd >= CIUKI_OPEN_MAX)
        return 0;
    struct proc_object *o = p->fds[fd].object;
    return o && o->release == description_release ? (struct desktop_description *)o : 0;
}

int desktop_fd_slot(const struct process *p, unsigned start)
{
    if (!p || !p->fds)
        return -EMFILE;
    for (unsigned i = start; i < CIUKI_OPEN_MAX; i++)
        if (!p->fds[i].object)
            return (int)i;
    return -EMFILE;
}

int desktop_fd_install(struct process *p, struct desktop_description *d, uint32_t flags)
{
    int fd = desktop_fd_slot(p, 0);
    if (fd >= 0)
        p->fds[fd] = (struct proc_fd){ &d->object, flags };
    return fd;
}

int desktop_close(struct process *p, int32_t fd)
{
    if (!p || !p->fds || fd < 0 || fd >= CIUKI_OPEN_MAX || !p->fds[fd].object)
        return -EBADF;
    struct proc_object *o = p->fds[fd].object;
    p->fds[fd] = (struct proc_fd){ 0 };
    o->release(o);
    return 0;
}

int desktop_dup(struct process *p, int32_t fd, int32_t target, bool exact, uint32_t flags)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d)
        return -EBADF;
    if (target < 0 || target >= CIUKI_OPEN_MAX)
        return exact ? -EBADF : -EINVAL;
    if (exact && target == fd)
        return fd;
    int slot = exact ? target : desktop_fd_slot(p, (unsigned)target);
    if (slot < 0)
        return slot;
    d->object.retain(&d->object);
    if (p->fds[slot].object)
        desktop_close(p, slot);
    p->fds[slot] = (struct proc_fd){ &d->object, flags };
    return slot;
}

int desktop_fcntl(struct process *p, int32_t fd, uint32_t cmd, uint32_t arg)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d)
        return -EBADF;
    switch (cmd) {
    case F_GETFD: return (int)p->fds[fd].flags;
    case F_SETFD:
        if (arg & ~FD_CLOEXEC) return -EINVAL;
        p->fds[fd].flags = arg;
        return 0;
    case F_GETFL: return (int)d->flags;
    case F_SETFL:
        if (arg & ~(O_ACCMODE | O_NONBLOCK)) return -EINVAL;
        d->flags = (d->flags & O_ACCMODE) | (arg & O_NONBLOCK);
        return 0;
    case F_DUPFD: return desktop_dup(p, fd, (int32_t)arg, false, 0);
    case F_DUPFD_CLOEXEC: return desktop_dup(p, fd, (int32_t)arg, false, FD_CLOEXEC);
    default: return -EINVAL;
    }
}

static uint32_t surface_page(void *object, uint32_t index)
{
    struct surface *s = object;
    uint32_t list = s->page_lists[index / (PAGE_SIZE / sizeof(uint32_t))];
    return list ? ((uint32_t *)P2V(list))[index % (PAGE_SIZE / sizeof(uint32_t))] : 0;
}

void surface_retain(struct surface *s) { s->references++; }
void surface_release(struct surface *s)
{
    if (!s || --s->references)
        return;
    for (unsigned i = 0; i < s->info.allocation_bytes / PAGE_SIZE; i++)
        if (surface_page(s, i)) { pmm_free(surface_page(s, i)); desktop_objects.pages--; }
    for (unsigned i = 0; i < ARRAY_SIZE(s->page_lists); i++)
        if (s->page_lists[i]) pmm_free(s->page_lists[i]);
    desktop_objects.surfaces--;
    kfree(s);
}
const struct ciuki_surface_info *surface_geometry(const struct surface *s) { return &s->info; }

int surface_create(struct process *p, uint32_t width, uint32_t height, uint32_t format)
{
    if (!width || !height || width > CIUKI_SURFACE_DIMENSION_MAX ||
        height > CIUKI_SURFACE_DIMENSION_MAX || format != CIUKI_SURFACE_XRGB8888)
        return -EINVAL;
    uint64_t bytes = (uint64_t)width * CIUKI_SURFACE_PIXEL_BYTES * height;
    if (bytes > CIUKI_SURFACE_MAX || bytes > UINT32_MAX - (PAGE_SIZE - 1))
        return -EOVERFLOW;
    int fd = desktop_fd_slot(p, 0);
    if (fd < 0) return fd;
    if (desktop_objects.descriptions + FILE_OBJECTS >= CIUKI_OPEN_DESCRIPTION_MAX) return -ENFILE;
    struct surface *s = kzalloc(sizeof(*s));
    if (!s) return -ENOMEM;
    s->info = (struct ciuki_surface_info){ sizeof(s->info), width, height,
        width * CIUKI_SURFACE_PIXEL_BYTES, format, PAGE_ALIGN_UP((uint32_t)bytes) };
    s->references = 1;
    desktop_objects.surfaces++;
    for (unsigned i = 0; i < s->info.allocation_bytes / PAGE_SIZE; i++) {
        unsigned table = i / (PAGE_SIZE / sizeof(uint32_t)), slot = i % (PAGE_SIZE / sizeof(uint32_t));
        if (!slot) {
            s->page_lists[table] = pmm_alloc();
            if (!s->page_lists[table]) { surface_release(s); return -ENOMEM; }
            memset(P2V(s->page_lists[table]), 0, PAGE_SIZE);
        }
        uint32_t phys = pmm_alloc();
        if (!phys) { surface_release(s); return -ENOMEM; }
        ((uint32_t *)P2V(s->page_lists[table]))[slot] = phys;
        desktop_objects.pages++;
        memset(P2V(phys), 0, PAGE_SIZE);
    }
    struct desktop_description *d = desktop_description_new(DESKTOP_SURFACE, O_RDWR);
    if (!d) { surface_release(s); return -ENOMEM; }
    d->u.surface = s;
    d->maximum = PROT_READ | PROT_WRITE;
    p->fds[fd] = (struct proc_fd){ &d->object, FD_CLOEXEC };
    return fd;
}

int surface_info(struct process *p, int32_t fd, struct ciuki_surface_info *out)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_SURFACE) return -EBADF;
    *out = d->u.surface->info;
    return 0;
}

int surface_attachment(struct process *p, int32_t fd, struct desktop_description **out)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d) return -EBADF;
    if (d->kind == DESKTOP_DISPLAY || d->kind == DESKTOP_INPUT) return -EPERM;
    if (d->kind != DESKTOP_SURFACE) return -EBADF;
    /* Reserve the receiving description before waiting. A queued message
     * owns it until an atomic receive moves it into the receiver's table. */
    struct desktop_description *ro = desktop_description_new(DESKTOP_SURFACE, O_RDONLY);
    if (!ro) return -ENOMEM;
    ro->u.surface = d->u.surface;
    ro->maximum = PROT_READ;
    surface_retain(ro->u.surface);
    *out = ro;
    return 0;
}

int surface_read(struct surface *s, uint32_t offset, void *out, uint32_t bytes)
{
    if (offset > s->info.allocation_bytes || bytes > s->info.allocation_bytes - offset)
        return -EINVAL;
    uint8_t *dst = out;
    while (bytes) {
        uint32_t n = PAGE_SIZE - offset % PAGE_SIZE;
        if (n > bytes) n = bytes;
        memcpy(dst, (uint8_t *)P2V(surface_page(s, offset / PAGE_SIZE)) + offset % PAGE_SIZE, n);
        dst += n; offset += n; bytes -= n;
    }
    return 0;
}

static void surface_mapping_retain(void *object) { surface_retain(object); }
static void surface_mapping_release(void *object) { surface_release(object); }

int32_t surface_map(struct process *p, int32_t fd, uint32_t prot)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_SURFACE) return -EBADF;
    if (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE)) return -EINVAL;
    if (prot & ~d->maximum) return -EACCES;
    if (!ua_map_shared) return -ENOMEM; /* no private copy/unsafe lifetime fallback */
    struct surface *s = d->u.surface;
    const struct ua_shared shared = { s, surface_mapping_retain, surface_mapping_release, surface_page };
    return ua_map_shared(p->memory, s->info.allocation_bytes, prot, d->maximum, &shared);
}
