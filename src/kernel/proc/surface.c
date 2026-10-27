/* Shared, zeroed surface backing; descriptions and mappings own separate refs.
 * See execution-abi.md, Desktop surfaces, and build/f2-05/research.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS ((file_description_count ? file_description_count() : 0) + (net_socket_count ? net_socket_count() : 0))
extern uint32_t net_socket_count(void) __attribute__((weak));
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>
#include <ciuki/security.h>
#include <ciuki/swdib.h>

struct surface {
    struct ciuki_surface_info info;
    uint32_t references;
    uint8_t *managed;
    uint32_t generation;
    bool reserved;
    uint32_t page_lists[CIUKI_SURFACE_MAX / PAGE_SIZE / (PAGE_SIZE / sizeof(uint32_t))];
};
struct desktop_ledger desktop_objects;
/* Large immutable command/surface storage cannot use the 2040-byte heap.
 * Ordinary resident direct-map pages; allocation failure is pre-admission. */
void *desktop_buffer_alloc(uint32_t bytes, enum mm_owner owner)
{
    if (!bytes || bytes>CIUKI_BATCH_MAX_EXTENT) return 0;
    unsigned pages=(bytes+PAGE_SIZE-1)/PAGE_SIZE;
    uint32_t phys=pmm_alloc_contig(pages);
    if (!phys) return 0;
    mm_claim(phys,pages,owner);
    memset(P2V(phys),0,pages*PAGE_SIZE);
    return P2V(phys);
}
void desktop_buffer_free(void *p, uint32_t bytes, enum mm_owner owner)
{
    if (!p) return;
    unsigned pages=(bytes+PAGE_SIZE-1)/PAGE_SIZE;
    for (unsigned i=0;i<pages;i++) mm_free(V2P(p)+i*PAGE_SIZE,owner);
}

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

bool desktop_object_is_channel(const struct proc_object *o)
{
    return o && o->release == description_release &&
        ((const struct desktop_description *)o)->kind == DESKTOP_CHANNEL;
}

int desktop_close(struct process *p, int32_t fd)
{
    if (!p || !p->fds || fd < 0 || fd >= CIUKI_OPEN_MAX || !p->fds[fd].object)
        return -EBADF;
    struct proc_object *o = p->fds[fd].object;
    struct desktop_description *d = desktop_fd(p, fd);
    p->fds[fd] = (struct proc_fd){ 0 };
    if (d && d->kind == DESKTOP_CHANNEL) channel_fd_closed(p, o);
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
    if (s->managed) {
        desktop_buffer_free(s->managed,s->info.allocation_bytes,MM_USER_SHARED);
        if (ledger_promise) ledger_promise(MM_USER_SHARED,0,-(int32_t)(s->info.allocation_bytes/PAGE_SIZE));
        desktop_objects.pages-=s->info.allocation_bytes/PAGE_SIZE;
        desktop_objects.surfaces--; kfree(s); return;
    }
    for (unsigned i = 0; i < s->info.allocation_bytes / PAGE_SIZE; i++)
        if (surface_page(s, i)) { mm_free(surface_page(s, i),MM_USER_SHARED); if (ledger_promise) ledger_promise(MM_USER_SHARED,0,-1); desktop_objects.pages--; }
    for (unsigned i = 0; i < ARRAY_SIZE(s->page_lists); i++)
        if (s->page_lists[i]) pmm_free(s->page_lists[i]);
    desktop_objects.surfaces--;
    kfree(s);
}
const struct ciuki_surface_info *surface_geometry(const struct surface *s) { return &s->info; }

int surface_create(struct process *p, uint32_t width, uint32_t height, uint32_t format)
{
    bool managed=format>=CIUKI_SURFACE_BATCH_RGB565 && format<=CIUKI_SURFACE_BATCH_XRGB8888;
    if (!width || !height || width > CIUKI_SURFACE_DIMENSION_MAX ||
        height > CIUKI_SURFACE_DIMENSION_MAX || (format != CIUKI_SURFACE_XRGB8888 && !managed))
        return -EINVAL;
    unsigned pixel_bytes=managed ? format : CIUKI_SURFACE_PIXEL_BYTES;
    uint64_t bytes = (uint64_t)width * pixel_bytes * height;
    if (bytes > CIUKI_SURFACE_MAX || bytes > UINT32_MAX - (PAGE_SIZE - 1))
        return -EOVERFLOW;
    int fd = desktop_fd_slot(p, 0);
    if (fd < 0) return fd;
    if (desktop_objects.descriptions + FILE_OBJECTS >= CIUKI_OPEN_DESCRIPTION_MAX) return -ENFILE;
    struct surface *s = kzalloc(sizeof(*s));
    if (!s) return -ENOMEM;
    s->info = (struct ciuki_surface_info){ sizeof(s->info), width, height,
        width * pixel_bytes, format, PAGE_ALIGN_UP((uint32_t)bytes) };
    s->references = 1;
    desktop_objects.surfaces++;
    if (managed) {
        static uint32_t sequence;
        if (sequence==UINT32_MAX) { surface_release(s); return -ENOSPC; }
        s->generation=++sequence;
        s->managed=desktop_buffer_alloc(s->info.allocation_bytes,MM_USER_SHARED);
        if (!s->managed) { surface_release(s); return -ENOMEM; }
        if (ledger_promise) ledger_promise(MM_USER_SHARED,0,(int32_t)(s->info.allocation_bytes/PAGE_SIZE));
        desktop_objects.pages+=s->info.allocation_bytes/PAGE_SIZE;
    }
    for (unsigned i = 0; !managed && i < s->info.allocation_bytes / PAGE_SIZE; i++) {
        unsigned table = i / (PAGE_SIZE / sizeof(uint32_t)), slot = i % (PAGE_SIZE / sizeof(uint32_t));
        if (!slot) {
            s->page_lists[table] = pmm_alloc();
            if (!s->page_lists[table]) { surface_release(s); return -ENOMEM; }
            memset(P2V(s->page_lists[table]), 0, PAGE_SIZE);
        }
        uint32_t phys = pmm_alloc();
        if (!phys) { surface_release(s); return -ENOMEM; }
        mm_claim(phys,1,MM_USER_SHARED);
        if (ledger_promise) ledger_promise(MM_USER_SHARED,0,1);
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
    if (s->reserved) return -EBUSY;
    if (offset > s->info.allocation_bytes || bytes > s->info.allocation_bytes - offset)
        return -EINVAL;
    if (s->managed) { memcpy(out,s->managed+offset,bytes); return 0; }
    uint8_t *dst = out;
    while (bytes) {
        uint32_t n = PAGE_SIZE - offset % PAGE_SIZE;
        if (n > bytes) n = bytes;
        memcpy(dst, (uint8_t *)P2V(surface_page(s, offset / PAGE_SIZE)) + offset % PAGE_SIZE, n);
        dst += n; offset += n; bytes -= n;
    }
    return 0;
}

int surface_write(struct surface *s, uint32_t offset, const void *data, uint32_t bytes)
{
    if (s->reserved) return -EBUSY;
    if (offset > s->info.allocation_bytes || bytes > s->info.allocation_bytes - offset) return -EINVAL;
    if (s->managed) { memcpy(s->managed+offset,data,bytes); return 0; }
    const uint8_t *src = data;
    while (bytes) {
        unsigned n = PAGE_SIZE - offset % PAGE_SIZE;
        if (n > bytes) n = bytes;
        memcpy((uint8_t *)P2V(surface_page(s, offset / PAGE_SIZE)) + offset % PAGE_SIZE, src, n);
        src += n; offset += n; bytes -= n;
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
    if (s->managed) return -EACCES;
    const struct ua_shared shared = { s, surface_mapping_retain, surface_mapping_release, surface_page };
    return ua_map_shared(p->memory, s->info.allocation_bytes, prot, d->maximum, &shared);
}

int surface_batch_target(struct desktop_description *d, uint32_t generation, struct swdib_target *out)
{
    if (!d || d->kind!=DESKTOP_SURFACE) return -EBADF;
    struct surface *s=d->u.surface;
    if (!s->managed) return -EACCES; /* Existing mapped writers cannot be excluded. */
    /* Internal metadata queries pass zero. Batch descriptors require the
     * explicit immutable generation returned by QUERY_SURFACE. */
    if (generation && generation!=s->generation) return -EAGAIN;
    unsigned bpp=s->info.format*8;
    *out=(struct swdib_target){.identity=s,.rights=d->maximum,.kind=CIUKI_TARGET_SYSTEM,
        .device={.width=s->info.width,.height=s->info.height,.pitch=s->info.stride,.size=s->info.allocation_bytes,
        .mapped=s->managed,.bpp=bpp,.red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8,
        .generation=s->generation,.present=true}};
    return 0;
}
int surface_batch_reserve(struct surface *s)
{ if (s->reserved) return -EBUSY; s->reserved=true; return 0; }
void surface_batch_unreserve(struct surface *s) { s->reserved=false; }
