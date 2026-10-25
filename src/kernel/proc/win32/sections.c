/* Section-owned backing, demand population, coherent aliases and COW credits.
 * File pages use the VFS cache path. No disk I/O bypasses its barriers/budgets.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
#include <ciuki/files.h>
#include <ciuki/sha256.h>
/* Internal uaddr bind flag, never a user protection value. */
#define SECTION_COW_BIND 0x80000000u
#define COW_PROMISE (UINT32_MAX-1u)
extern int file_section_io_locked(struct file_description *,void *,size_t,bool,bool,uint64_t,size_t *);

struct section_data {
    struct w32_object *object, *file;
    uint32_t refs, kind, bytes, count, protect, image_base, writer, writable_views;
    uint32_t *pages, *protection;
    uint32_t committed;
    uint8_t digest[32];
    struct kmutex lock;
    bool syncing;
};
struct w32_view {
    struct w32_view *next;
    struct process *process;
    struct w32_object *object, *section;
    uint32_t base, bytes, offset, access;
    uint32_t *credits;
    uint32_t active;
    bool image, cow, writable, registered, busy;
};
static struct w32_view *view_at(struct uaddr *u, uint32_t va)
{
    struct process *p = w32_process_for_memory(u);
    if (!u->win32 || !p || !p->w32)
        return 0;
    for (struct w32_view *v = p->w32->views; v; v = v->next)
        if (va >= v->base && va - v->base < v->bytes)
            return v;
    return 0;
}
bool w32_view_contains(struct uaddr *u, uint32_t va) { return view_at(u, va) != 0; }
static uint32_t section_page(struct section_data *s, unsigned index)
{
    if (s->pages[index])
        return s->pages[index];
    uint32_t page = pmm_alloc();
    if (!page)
        return 0;
    mm_claim(page, 1, MM_USER_SHARED);
    struct mm_page_descriptor *descriptor=pmm_descriptor?pmm_descriptor(page):0;
    if (descriptor) {
        descriptor->backing_kind=s->file?MM_BACKING_FILE:MM_BACKING_SHARED;
        descriptor->backing_slot=index; descriptor->io_in_flight=1;
    }
    memset(P2V(page), 0, PAGE_SIZE);
    if (s->file) {
        struct file_description *d = s->file->u.file.description;
        size_t done = 0;
        uint32_t off = index * PAGE_SIZE, n = s->bytes - off;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        s->syncing = true;
        vfs_lock(d->node->space->vfs, 0);
        int err = file_section_io_locked(d, P2V(page), n, false, true, off, &done);
        vfs_unlock(d->node->space->vfs);
        s->syncing = false;
        if (err || done != n) {
            mm_free(page, MM_USER_SHARED);
            return 0;
        }
    }
    if (descriptor) descriptor->io_in_flight=0;
    s->pages[index] = page;
    w32_accounts.pages++;
    return page;
}
uint32_t w32_shared_frame;
void w32_section_destroy(struct w32_object *o)
{
    struct section_data *s = o->u.section;
    if (!s || --s->refs)
        return;
    /* The tick publisher (counter.c) reads the frame with interrupts off. */
    if (s->kind == 4 && s->pages[0] && s->pages[0] == w32_shared_frame) {
        uint32_t f = irq_save(); w32_shared_frame = 0; irq_restore(f);
    }
    for (unsigned i = 0; i < s->count; i++)
        if (s->pages[i]) {
            mm_free(s->pages[i], MM_USER_SHARED);
            w32_accounts.pages--;
        }
    if (s->committed && ledger_promise) ledger_promise(MM_USER_SHARED,0,-(int32_t)s->committed);
    w32_release(s->file);
    w32_free(s->pages, s->count * 4);
    w32_free(s->protection, s->count * 4);
    w32_free(s, sizeof(*s));
    w32_accounts.sections--;
}
static int section_initialize(struct w32_object *o, uint32_t kind, uint32_t bytes, uint32_t protect)
{
    if (!bytes || bytes > CIUKI_W32_APP_LIMIT - CIUKI_W32_LOW)
        return -EINVAL;
    struct section_data *s = w32_alloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->refs = 1;
    s->object = o;
    s->kind = kind;
    s->bytes = bytes;
    s->count = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    s->protect = protect;
    kmutex_init(&s->lock);
    s->pages = w32_alloc(s->count * 4);
    s->protection = w32_alloc(s->count * 4);
    if (!s->pages || !s->protection) {
        w32_free(s->pages, s->count * 4);
        w32_free(s->protection, s->count * 4);
        w32_free(s, sizeof(*s));
        return -ENOMEM;
    }
    if (ledger_commit && !ledger_commit(MM_USER_SHARED,s->count)) {
        w32_free(s->pages,s->count*4); w32_free(s->protection,s->count*4);
        w32_free(s,sizeof(*s)); return -ENOMEM;
    }
    if (!ledger_commit && ledger_promise) ledger_promise(MM_USER_SHARED,0,s->count);
    s->committed=s->count;
    for (unsigned i = 0; i < s->count; i++) {
        s->protection[i] = protect;
        if (!((i+1)&15u)) kpreempt_point();
    }
    o->u.section = s;
    o->section_limit = bytes;
    o->section_protect = protect;
    w32_accounts.sections++;
    return 0;
}
static int section_flush(struct section_data *s, bool locked)
{
    if (!s->file || s->syncing)
        return 0;
    struct file_description *d = s->file->u.file.description;
    if (!(s->protect & (W32_PAGE_READWRITE | W32_PAGE_EXECUTE_READWRITE)))
        return 0;
    d->object.retain(&d->object);
    s->syncing = true;
    int err = 0;
    if (!locked)
        vfs_lock(d->node->space->vfs, 0);
    /* Write through the cache, retaining all resident bytes on error. The VFS
     * owns dirty-age, barriers and persistent error state, including f1-82. */
    for (unsigned i = 0; !err && i < s->count; i++)
        if (s->pages[i]) {
            uint32_t off = i * PAGE_SIZE, n = s->bytes - off;
            if (n > PAGE_SIZE)
                n = PAGE_SIZE;
            size_t done = 0;
            err = file_section_io_locked(d, P2V(s->pages[i]), n, true, true, off, &done);
            if (!err && done != n)
                err = -EIO;
        }
    if (!locked)
        vfs_unlock(d->node->space->vfs);
    s->syncing = false;
    d->object.release(&d->object);
    return err;
}
bool w32_file_needs_sync(struct px_node *node)
{
    for (struct w32_object *o=w32_objects;o;o=o->next) {
        if (o->type!=W32_OBJ_SECTION || !o->u.section) continue;
        struct section_data *s=o->u.section;
        if (s->file && !s->syncing && s->file->u.file.description->node==node &&
            (s->protect&(W32_PAGE_READWRITE|W32_PAGE_EXECUTE_READWRITE))) return true;
    }
    return false;
}
int w32_file_sync(struct px_node *node)
{
    struct w32_object *o = w32_objects;
    w32_retain(o);
    while (o) {
        struct w32_object *next = o->next;
        w32_retain(next);
        int err = 0;
        if (o->type == W32_OBJ_SECTION && o->u.section) {
            struct section_data *s = o->u.section;
            if (s->file && s->file->u.file.description->node == node)
                err = section_flush(s, true);
        }
        w32_release(o);
        if (err) {
            w32_release(next);
            return err;
        }
        o = next;
    }
    return 0;
}
void w32_file_changed(struct px_node *node, uint64_t off, const void *buffer, uint32_t bytes)
{
    for (struct w32_object *o = w32_objects; o; o = o->next)
        if (o->type == W32_OBJ_SECTION && o->u.section) {
            struct section_data *s = o->u.section;
            if (!s->file || s->file->u.file.description->node != node || s->syncing)
                continue;
            const uint8_t *b = buffer;
            uint64_t pos = off;
            uint32_t left = bytes;
            while (left && pos < s->bytes) {
                unsigned index = (uint32_t)pos / PAGE_SIZE, within = (uint32_t)pos % PAGE_SIZE,
                         n = PAGE_SIZE - within;
                if (n > left)
                    n = left;
                if (n > s->bytes - pos)
                    n = s->bytes - (uint32_t)pos;
                if (s->pages[index])
                    memcpy((uint8_t *)P2V(s->pages[index]) + within, b, n);
                pos += n;
                b += n;
                left -= n;
            }
        }
}
int w32_view_access(struct uaddr *u, uint32_t va, bool write)
{
    struct w32_view *v = view_at(u, va);
    if (!v)
        return 0;
    if (v->busy)
        return -EBUSY;
    v->active++;
    struct ua_extent *e = ua_find(u, va);
    if (!e || !(e->prot & PROT_READ) || (write && !(e->prot & PROT_WRITE))) {
        v->active--;
        return -EFAULT;
    }
    uint32_t protection = e->prot;
    struct section_data *s = v->section->u.section;
    unsigned local = (va - v->base) / PAGE_SIZE, index = (v->offset / PAGE_SIZE) + local;
    kmutex_lock(&s->lock);
    uint32_t page = section_page(s, index);
    int err = page ? 0 : -ENOMEM;
    if (!err && v->cow && write && v->credits && v->credits[local] && v->credits[local] != UINT32_MAX) {
        uint32_t credit = pmm_alloc();
        if (!credit) { kmutex_unlock(&s->lock); v->active--; return -ENOMEM; }
        mm_claim(credit,1,MM_USER_PRIVATE);
        memcpy(P2V(credit), P2V(page), PAGE_SIZE);
        err = ua_w32_bind(u, va & ~4095u, credit, protection, false);
        if (!err) {
            v->credits[local] = UINT32_MAX;
            w32_accounts.cow_credits--;
        } else mm_free(credit,MM_USER_PRIVATE);
    } else if (!err) {
        /* Existing private COW PTEs are never rebound to shared bytes. */
        err = ua_w32_bind(u, va & ~4095u, page, protection | (v->cow ? SECTION_COW_BIND : 0), true);
    }
    kmutex_unlock(&s->lock);
    v->active--;
    return err;
}
static int view_flush(struct w32_view *v)
{
    if (v->cow)
        return 0;
    struct section_data *s = v->section->u.section;
    kmutex_lock(&s->lock);
    int err = section_flush(s, false);
    kmutex_unlock(&s->lock);
    return err;
}
static void view_free(struct w32_view *v)
{
    struct section_data *s = v->section->u.section;
    if (v->credits) {
        for (unsigned i = 0; i < v->bytes / PAGE_SIZE; i++)
            if (v->credits[i] && v->credits[i] != UINT32_MAX) {
                if (v->credits[i]!=COW_PROMISE) mm_free(v->credits[i], MM_USER_PRIVATE);
                if (ledger_promise)
                    ledger_promise(MM_USER_PRIVATE, 0, -1);
                w32_accounts.cow_credits--;
            }
        w32_free(v->credits, v->bytes / PAGE_SIZE * 4);
    }
    if (s->file && v->registered && v->writable && !v->cow && s->writable_views && !--s->writable_views)
        s->writer = 0;
    w32_release(v->section);
    w32_release(v->object);
    w32_accounts.views--;
    w32_free(v, sizeof(*v));
}
static int map_view(struct process *p, struct w32_object *o, uint32_t *address, uint32_t bytes,
    uint32_t offset, uint32_t access, bool image)
{
    struct section_data *s = o->u.section;
    if (!s)
        return -EINVAL;
    if ((offset & 65535) || offset >= o->section_limit || !bytes || bytes > o->section_limit - offset
        || (*address & 65535))
        return -EINVAL;
    if (!image && access != W32_FILE_MAP_READ && access != W32_FILE_MAP_WRITE && access != W32_FILE_MAP_COPY)
        return -EINVAL;
    bool cow = image || access == W32_FILE_MAP_COPY, write = image || access == W32_FILE_MAP_WRITE;
    uint32_t native;
    if (w32_protection(o->section_protect, &native))
        return -EINVAL;
    if (!image && write
        && (!(native & PROT_WRITE)
            || (o->section_protect & (W32_PAGE_WRITECOPY | W32_PAGE_EXECUTE_WRITECOPY))))
        return -EACCES;
    if (s->file && write && !cow && s->writer && s->writer != p->pid)
        return -EACCES;
    struct w32_view *v = w32_alloc(sizeof(*v));
    if (!v)
        return -ENOMEM;
    v->object = w32_object_new(W32_OBJ_VIEW);
    if (!v->object) {
        w32_free(v, sizeof(*v));
        return -ENOMEM;
    }
    v->process = p;
    v->section = o;
    w32_retain(o);
    v->bytes = PAGE_ALIGN_UP(bytes);
    v->offset = offset;
    v->access = access;
    v->image = image;
    v->cow = cow;
    v->writable = write;
    w32_accounts.views++;
    if (s->file && write && !cow) {
        s->writer = p->pid;
        s->writable_views++;
        v->registered = true;
    }
    int err = 0;
    if (cow) {
        v->credits = w32_alloc(v->bytes / PAGE_SIZE * 4);
        if (!v->credits) {
            view_free(v);
            return -ENOMEM;
        }
        for (unsigned i = 0; i < v->bytes / PAGE_SIZE; i++) {
            uint32_t prot;
            w32_protection(s->protection[offset / PAGE_SIZE + i], &prot);
            if (!image || prot & PROT_WRITE) {
                if (ledger_commit && !ledger_commit(MM_USER_PRIVATE,1)) {
                    view_free(v); return -ENOMEM;
                }
                if (!ledger_commit && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,1);
                v->credits[i] = COW_PROMISE;
                w32_accounts.cow_credits++;
            }
            if (!(i & 15))
                kpreempt_point();
        }
    }
    uint32_t base = *address;
    if (image && base >= CIUKI_W32_APP_LIMIT) {
        if (base >= CIUKI_W32_BUILTIN_LIMIT || v->bytes > CIUKI_W32_BUILTIN_LIMIT - base)
            err = -EINVAL;
        else
            err = ua_map_at(p->memory, base, v->bytes, 0, 7, UA_W32, 0);
        if (!err) {
            struct ua_extent *e = ua_find(p->memory, base);
            e->w32_allocation = base;
            e->w32_allocation_protect = W32_PAGE_READONLY;
            e->w32_state = W32_MEM_COMMIT;
            e->w32_type = W32_MEM_IMAGE;
        }
    } else {
        struct ciuki_w32_vm a = { .address = base,
            .bytes = v->bytes,
            .flags = W32_MEM_RESERVE,
            .protect = write || cow ? W32_PAGE_READWRITE : W32_PAGE_READONLY };
        uint32_t status = w32_vm_alloc(p->memory, &a);
        err = status ? status == W32_STATUS_NO_MEMORY ? -ENOMEM : -EEXIST : 0;
        base = a.address;
        if (!err) {
            struct ua_extent *e = ua_find(p->memory, base);
            e->w32_state = W32_MEM_COMMIT;
            e->w32_type = image ? W32_MEM_IMAGE : W32_MEM_MAPPED;
        }
    }
    if (err) {
        view_free(v);
        return err;
    }
    v->base = base;
    for (unsigned i = 0; !err && i < v->bytes / PAGE_SIZE; i++) {
        uint32_t protect = image ? s->protection[offset / PAGE_SIZE + i]
            : cow                ? W32_PAGE_WRITECOPY
            : write              ? W32_PAGE_READWRITE
                                 : W32_PAGE_READONLY,
                 prot;
        w32_protection(protect, &prot);
        err = ua_w32_change(p->memory, base + i * PAGE_SIZE, PAGE_SIZE, W32_VM_PROTECT, protect, prot, true);
    }
    if (err) {
        ua_w32_release(p->memory, base, true);
        view_free(v);
        return err;
    }
    struct vfs_file_identity *identity=s->file?s->file->u.file.description->identity:0;
    ua_backing_identity(p->memory,base,v->bytes,identity,identity?identity->generation:0,offset/PAGE_SIZE);
    v->registered = true;
    v->next = p->w32->views;
    p->w32->views = v;
    *address = base;
    return 0;
}
static int unmap_view(struct process *p, uint32_t base, bool exiting)
{
    struct w32_view **at = &p->w32->views;
    while (*at && (*at)->base != base)
        at = &(*at)->next;
    if (!*at)
        return -EINVAL;
    struct w32_view *v = *at;
    if (v->busy || v->active)
        return -EBUSY;
    v->busy = true;
    int err = view_flush(v);
    if (err && !exiting) {
        v->busy = false;
        return err;
    }
    if (err)
        klog("[win32] mapping flush pid=%u base=%08x error=%d", p->pid, base, err);
    int release = ua_w32_release(p->memory, base, true);
    if (release) {
        v->busy = false;
        return release;
    }
    at = &p->w32->views;
    while (*at && *at != v)
        at = &(*at)->next;
    if (*at)
        *at = v->next;
    view_free(v);
    return err;
}
void w32_views_cleanup(struct process *p)
{
    while (p->w32 && p->w32->views) {
        struct w32_view *v = p->w32->views;
        int err = unmap_view(p, v->base, true);
        if (err && p->w32->views == v) { /* Pins are cancelled by lifecycle hooks first. */
            klog("[win32] pinned view teardown pid=%u base=%08x error=%d", p->pid, v->base, err);
            break;
        }
    }
}
static uint32_t view_protect_inner(struct uaddr *u, struct ciuki_w32_vm *a)
{
    struct w32_view *v = view_at(u, a->address);
    if (!v)
        return W32_STATUS_CONFLICTING_ADDRESSES;
    uint32_t native, status = w32_protection(a->protect, &native);
    if (status)
        return status;
    if (a->flags || !a->bytes || a->bytes > v->bytes - (a->address - v->base))
        return W32_STATUS_INVALID_PARAMETER;
    struct section_data *s = v->section->u.section;
    uint32_t maximum;
    w32_protection(v->section->section_protect, &maximum);
    if (v->cow)
        maximum |= PROT_WRITE;
    if (!v->image && ((native & ~maximum) || ((native & PROT_WRITE) && v->access == W32_FILE_MAP_READ)))
        return W32_STATUS_ACCESS_DENIED;
    struct ua_extent *e = ua_find(u, a->address);
    uint32_t old = e->w32_protect;
    uint32_t start = a->address & ~4095u, end = PAGE_ALIGN_UP(a->address + a->bytes);
    uint32_t count = (end - start) / PAGE_SIZE, *fresh = 0;
    bool need = false;
    if (v->cow && (native & PROT_WRITE))
        for (unsigned i = 0; i < count; i++)
            if (!v->credits[(start - v->base) / PAGE_SIZE + i])
                need = true;
    if (need) {
        fresh = w32_alloc(count * 4);
        if (!fresh)
            return W32_STATUS_NO_MEMORY;
        for (unsigned i = 0; i < count; i++)
            if (!v->credits[(start - v->base) / PAGE_SIZE + i]) {
                if (ledger_commit && !ledger_commit(MM_USER_PRIVATE,1)) {
                    for (unsigned j=0;j<i;j++) if (fresh[j] && ledger_promise)
                        ledger_promise(MM_USER_PRIVATE,0,-1);
                    w32_free(fresh,count*4); return W32_STATUS_NO_MEMORY;
                }
                if (!ledger_commit && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,1);
                fresh[i]=COW_PROMISE;
            }
    }
    int err = ua_w32_change(u, start, end - start, W32_VM_PROTECT, a->protect, native, true);
    if (err) {
        if (fresh) {
            for (unsigned i = 0; i < count; i++)
                if (fresh[i] && ledger_promise)
                    ledger_promise(MM_USER_PRIVATE,0,-1);
            w32_free(fresh, count * 4);
        }
        return w32_status(err);
    }
    if (fresh) {
        for (unsigned i = 0; i < count; i++)
            if (fresh[i]) {
                v->credits[(start - v->base) / PAGE_SIZE + i] = fresh[i];
                w32_accounts.cow_credits++;
            }
        w32_free(fresh, count * 4);
    }
    for (uint32_t va = start; va < end; va += PAGE_SIZE) {
        /* Reapply shared COW protection without committing untouched pages. */
        unsigned i = (va - v->base) / PAGE_SIZE, index = v->offset / PAGE_SIZE + i;
        if (s->pages[index])
            ua_w32_bind(u, va, s->pages[index], native | (v->cow ? SECTION_COW_BIND : 0), true);
    }
    a->old_protect = old;
    a->address = start;
    a->bytes = end - start;
    return 0;
}
uint32_t w32_view_protect(struct uaddr *u, struct ciuki_w32_vm *a)
{
    struct w32_view *v = view_at(u, a->address);
    if (!v)
        return W32_STATUS_CONFLICTING_ADDRESSES;
    if (v->busy || v->active)
        return W32_STATUS_DEVICE_BUSY;
    v->active++;
    uint32_t status = view_protect_inner(u, a);
    v->active--;
    return status;
}
uint32_t w32_view_call(struct proc_thread *t, struct ciuki_w32_view_args *a)
{
    int err = 0;
    if (a->action == W32_VIEW_MAP) {
        struct w32_object *o = w32_lookup(t, a->section, W32_OBJ_SECTION);
        if (!o) {
            a->error = 6;
            return 0;
        }
        if (!o->u.section || !o->section_limit) {
            a->error = 170;
            return 0;
        }
        struct section_data *s = o->u.section;
        if (a->offset_hi || a->offset_lo >= o->section_limit) {
            a->error = 87;
            return 0;
        }
        uint32_t bytes = a->bytes ? a->bytes : o->section_limit - a->offset_lo;
        err = map_view(t->process, o, &a->address, bytes, a->offset_lo, a->access, s->kind == W32_SEC_IMAGE);
        a->result = a->address;
    } else if (a->action == W32_VIEW_UNMAP)
        err = unmap_view(t->process, a->address, false);
    else if (a->action == W32_VIEW_FLUSH) {
        struct w32_view *v = view_at(t->process->memory, a->address);
        if (!v || a->bytes > v->bytes - (a->address - v->base))
            err = -EINVAL;
        else if (v->busy)
            err = -EBUSY;
        else {
            v->active++;
            err = view_flush(v);
            v->active--;
        }
    } else
        err = -EINVAL;
    a->error = err ? w32_last_error(w32_status(err)) : 0;
    return 0;
}
uint32_t w32_section_call(struct proc_thread *t, struct ciuki_w32_section_args *a)
{
    uint32_t native;
    uint32_t declared = a->protect & ~W32_PAGE_NOCACHE;
    bool shared_write = declared == W32_PAGE_READWRITE || declared == W32_PAGE_EXECUTE_READWRITE;
    if ((declared != W32_PAGE_READONLY && declared != W32_PAGE_READWRITE && declared != W32_PAGE_WRITECOPY
            && declared != W32_PAGE_EXECUTE_READ && declared != W32_PAGE_EXECUTE_READWRITE
            && declared != W32_PAGE_EXECUTE_WRITECOPY)
        || w32_protection(a->protect, &native) || a->flags & ~1u || a->kind < W32_SEC_PAGEFILE
        || a->kind > W32_SEC_FILE) {
        a->error = 87;
        return 0;
    }
    struct w32_object *file = 0;
    if (a->kind == W32_SEC_FILE) {
        file = w32_lookup(t, a->file, W32_OBJ_FILE);
        if (!file) {
            a->error = 6;
            return 0;
        }
        if (!file->u.file.description->node || file->u.file.description->node->kind != PX_FILE) {
            a->error = 6;
            return 0;
        }
        uint32_t mode = file->u.file.description->flags & O_ACCMODE;
        if (mode == O_WRONLY || (shared_write && mode != O_RDWR)) {
            a->error = 5;
            return 0;
        }
        if (!a->bytes) {
            struct px_node *n = file->u.file.description->node;
            a->bytes = n->ro ? (uint32_t)n->ro_size : n->entry.size;
        }
    }
    if (!a->bytes || a->bytes > CIUKI_W32_APP_LIMIT - CIUKI_W32_LOW) {
        a->error = 87;
        return 0;
    }
    uint16_t name[261] = { 0 };
    if (a->name) {
        for (unsigned i = 0; i <= 260; i++) {
            if (a->name > UINT32_MAX - i * 2 || copy_from_user(&name[i], a->name + i * 2, 2)) {
                a->error = 998;
                return 0;
            }
            if (!name[i])
                break;
            if (i == 260) {
                a->error = 206;
                return 0;
            }
        }
    }
    struct w32_object *o;
    bool exists;
    a->error = w32_named_create(
        t->process, W32_OBJ_SECTION, a->name ? name : 0, UINT32_MAX, !!a->flags, &o, &a->result, &exists);
    if (a->error)
        return 0;
    if (exists) {
        a->error = 183;
        return 0;
    }
    int err = 0;
    if (file) {
        struct file_description *d = file->u.file.description;
        uint64_t size = d->node->ro ? d->node->ro_size : d->node->entry.size;
        if (a->bytes > size) {
            if (!shared_write)
                err = -EACCES;
            else {
                vfs_lock(d->node->space->vfs, 0);
                err = file_truncate_locked(d, a->bytes);
                vfs_unlock(d->node->space->vfs);
            }
        }
    }
    if (!err)
        err = section_initialize(o, a->kind, a->bytes, a->protect);
    if (!err)
        o->section_limit = 0;
    if (!err && file) {
        struct section_data *fresh = o->u.section, *shared = 0;
        for (struct w32_object *it = w32_objects; it; it = it->next)
            if (it != o && it->type == W32_OBJ_SECTION && it->u.section) {
                struct section_data *candidate = it->u.section;
                if (candidate->file
                    && candidate->file->u.file.description->node == file->u.file.description->node) {
                    shared = candidate;
                    break;
                }
            }
        if (shared) {
            shared->refs++;
            kmutex_lock(&shared->lock);
            /* Existing section aliases retain their own size/protection. Grow
             * the common backing transactionally before publishing an alias. */
            if (fresh->count > shared->count) {
                uint32_t extra=fresh->count-shared->count;
                shared->committed+=extra; fresh->committed-=extra;
                memcpy(fresh->pages, shared->pages, shared->count * 4);
                w32_free(shared->pages, shared->count * 4);
                w32_free(shared->protection, shared->count * 4);
                shared->pages = fresh->pages;
                shared->protection = fresh->protection;
                shared->count = fresh->count;
                shared->bytes = fresh->bytes;
                fresh->pages = 0;
                fresh->protection = 0;
                fresh->count = 0;
            }
            if (shared_write && !(shared->protect & (W32_PAGE_READWRITE | W32_PAGE_EXECUTE_READWRITE))) {
                shared->protect = a->protect;
                w32_release(shared->file);
                shared->file = file;
                w32_retain(file);
            }
            kmutex_unlock(&shared->lock);
            w32_section_destroy(o);
            o->u.section = shared;
        } else {
            fresh->file = file;
            w32_retain(file);
        }
    }
    if (!err)
        o->section_limit = a->bytes;
    if (err) {
        w32_handle_close(t->process, a->result);
        a->result = 0;
        a->error = w32_last_error(w32_status(err));
    }
    return 0;
}
static int image_seed(
    struct section_data *s, const struct ciuki_file *f, uint32_t va, uint32_t off, uint32_t bytes)
{
    while (bytes) {
        unsigned i = va / PAGE_SIZE, within = va % PAGE_SIZE, n = PAGE_SIZE - within;
        if (n > bytes)
            n = bytes;
        uint32_t page = section_page(s, i);
        if (!page)
            return -ENOMEM;
        uint32_t present=off<f->bytes?f->bytes-off:0;
        if(present>n)present=n;
        int err = present ? f->read(f->cookie, off, (uint8_t *)P2V(page) + within, present) : 0;
        if (err) return err;
        if(present<n)memset((uint8_t *)P2V(page)+within+present,0,n-present);
        va += n;
        off += n;
        bytes -= n;
        kpreempt_point();
    }
    return 0;
}
/* `known`: the image digest when the caller already hashed these exact,
 * immutable bytes (w32_loader_image); NULL hashes them here. */
static int image_section_map_inner(
    struct process *p, const struct ciuki_file *f, const struct w32_image *im, bool builtin, uint32_t *mapped,
    const uint8_t *known)
{
    uint32_t limit = builtin ? CIUKI_W32_BUILTIN_LIMIT : CIUKI_W32_APP_LIMIT;
    if ((!im->base && (!mapped || builtin)) || (im->base && im->base < (builtin ? CIUKI_W32_APP_LIMIT : CIUKI_W32_LOW)) || im->base >= limit || (im->base & 65535)
        || im->bytes > limit - im->base)
        return -ENOEXEC;
    if (w32_process_init(p))
        return -ENOMEM;
    uint8_t digest[32];
    int err = 0;
    if (known)
        memcpy(digest, known, sizeof(digest));
    else {
        uint32_t scratch = pmm_alloc();
        if (!scratch)
            return -ENOMEM;
        struct sha256_ctx hash;
        sha256_init(&hash);
        for (uint32_t off = 0; off < f->bytes;) {
            uint32_t n = f->bytes - off;
            if (n > PAGE_SIZE)
                n = PAGE_SIZE;
            err = f->read(f->cookie, off, P2V(scratch), n);
            if (err)
                break;
            sha256_update(&hash, P2V(scratch), n);
            off += n;
            kpreempt_point();
        }
        sha256_final(&hash, digest);
        pmm_free(scratch);
        if (err)
            return err;
    }
    struct w32_object *o = 0;
    for (struct w32_object *it = w32_objects; it; it = it->next)
        if (it->type == W32_OBJ_SECTION && it->u.section) {
            struct section_data *s = it->u.section;
            if (it->section_limit && s->kind == W32_SEC_IMAGE && s->image_base == im->base
                && s->bytes == im->bytes && !memcmp(s->digest, digest, 32)) {
                o = it;
                w32_retain(o);
                break;
            }
        }
    if (!o) {
        o = w32_object_new(W32_OBJ_SECTION);
        if (!o)
            return -ENOMEM;
        err = section_initialize(o, W32_SEC_IMAGE, im->bytes, W32_PAGE_EXECUTE_READWRITE);
        if (err) {
            w32_release(o);
            return err;
        }
        o->section_limit = 0;
        struct section_data *s = o->u.section;
        s->image_base = im->base;
        memcpy(s->digest, digest, 32);
        err = image_seed(s, f, 0, 0, im->headers);
        for (unsigned i = 0; !err && i < im->count; i++)
            err = image_seed(s, f, im->section[i].rva, im->section[i].offset, im->section[i].raw);
        for (uint32_t page = 0; !err && page < PAGE_ALIGN_UP(im->bytes); page += PAGE_SIZE) {
            uint32_t prot = page < im->headers ? PROT_READ : 0;
            for (unsigned i = 0; i < im->count; i++) {
                const struct w32_section *sec = &im->section[i];
                if (!sec->bytes || page >= sec->rva + sec->bytes || page + PAGE_SIZE <= sec->rva)
                    continue;
                if (sec->flags & 0x40000000)
                    prot |= PROT_READ;
                if (sec->flags & 0x80000000)
                    prot |= PROT_READ | PROT_WRITE;
                if (sec->flags & 0x20000000)
                    prot |= PROT_READ | PROT_EXEC;
            }
            s->protection[page / PAGE_SIZE] = !prot ? W32_PAGE_NOACCESS
                : prot & PROT_EXEC ? (prot & PROT_WRITE ? W32_PAGE_EXECUTE_WRITECOPY : W32_PAGE_EXECUTE_READ)
                                   : (prot & PROT_WRITE ? W32_PAGE_WRITECOPY : W32_PAGE_READONLY);
        }
    }
    if (!err)
        o->section_limit = im->bytes;
    uint32_t base = im->base;
    if (!err)
        err = map_view(p, o, &base, im->bytes, 0, W32_FILE_MAP_COPY, true);
    if(!err && mapped)*mapped=base;
    w32_release(o);
    return err;
}

/* Serialize cold image publication without holding a kernel mutex over the
 * hash/seed/map loops: their existing voluntary preemption points stay live. */
static bool image_section_building;
static struct kwait image_section_changed;
static bool image_section_available(void *unused)
{
    (void)unused;
    return !image_section_building;
}
static int image_map_serialized(
    struct process *p, const struct ciuki_file *f, const struct w32_image *im, bool builtin, uint32_t *mapped,
    const uint8_t *known)
{
    while (image_section_building)
        kwait_wait_until(&image_section_changed, image_section_available, 0, w32_now() + 1);
    image_section_building = true;
    int err = image_section_map_inner(p, f, im, builtin, mapped, known);
    image_section_building = false;
    kwait_wake_all(&image_section_changed);
    return err;
}
int w32_image_section_map(struct process *p,const struct ciuki_file *f,const struct w32_image *im,bool builtin)
{
    return image_map_serialized(p,f,im,builtin,0,0);
}
static void shared_section_retain(void *o) { w32_retain(o); }
static void shared_section_release(void *o) { w32_release(o); }
int w32_shared_map(struct process *p)
{
    struct w32_object *o = 0;
    for (struct w32_object *it = w32_objects; it; it = it->next)
        if (it->type == W32_OBJ_SECTION && it->u.section
            && ((struct section_data *)it->u.section)->kind == 4) {
            o = it;
            w32_retain(o);
            break;
        }
    if (!o) {
        o = w32_object_new(W32_OBJ_SECTION);
        if (!o)
            return -ENOMEM;
        int err = section_initialize(o, 4, PAGE_SIZE, W32_PAGE_READONLY);
        if (err) {
            w32_release(o);
            return err;
        }
    }
    struct section_data *s = o->u.section;
    uint32_t page = section_page(s, 0);
    if (!page) {
        w32_release(o);
        return -ENOMEM;
    }
    const uint8_t gate[] = { 0xcd, 0x80, 0xc3 };
    memcpy((uint8_t *)P2V(page) + 0x100, gate, sizeof(gate));
    w32_shared_frame = page;
    int err = ua_map_at(p->memory, CIUKI_W32_SHARED, PAGE_SIZE, 0, PROT_READ, UA_W32, 0);
    if (!err) {
        struct ua_extent *e = ua_find(p->memory, CIUKI_W32_SHARED);
        e->w32_allocation = CIUKI_W32_SHARED;
        e->w32_allocation_protect = W32_PAGE_READONLY;
        e->w32_protect = W32_PAGE_READONLY;
        e->prot = PROT_READ;
        e->w32_state = W32_MEM_COMMIT;
        e->w32_type = W32_MEM_MAPPED;
        e->w32_locked = true;
        e->shared = (struct ua_shared) {
            .object = o, .retain = shared_section_retain, .release = shared_section_release
        };
        w32_retain(o);
        err = ua_w32_bind(p->memory, CIUKI_W32_SHARED, page, PROT_READ, true);
    }
    w32_release(o);
    return err;
}

/* Reuse the seed views at startup; all subsequent DLLs use the same immutable
 * section cache. A relocated view retains unrelocated shared backing. */
int w32_loader_image(struct process *p,const struct ciuki_file *f,struct w32_image *im,
                     uint32_t *address,uint32_t flags)
{
    uint32_t preferred=im->base;
    bool datafile=!!(flags&W32_LDR_DATAFILE);
    if(*address!=preferred && *address)return -EINVAL;
    if(datafile)preferred=im->base=0;
    uint32_t scratch=pmm_alloc();if(!scratch)return -ENOMEM;
    struct sha256_ctx hash;sha256_init(&hash);int err=0;
    for(uint32_t off=0;off<f->bytes;){uint32_t n=f->bytes-off;if(n>PAGE_SIZE)n=PAGE_SIZE;
        err=f->read(f->cookie,off,P2V(scratch),n);if(err)break;
        sha256_update(&hash,P2V(scratch),n);off+=n;kpreempt_point();}
    uint8_t digest[32];sha256_final(&hash,digest);pmm_free(scratch);if(err)return err;
    bool reused=false;
    for(struct w32_view *v=p->w32?p->w32->views:0;v;v=v->next){
        struct section_data *data=v->section->u.section;
        if(v->image && v->base==preferred && data->bytes==im->bytes && !memcmp(data->digest,digest,32)){
            reused=true;break;
        }
    }
    if(!reused){
        bool builtin=!!(flags&W32_LDR_BUILTIN)&&!datafile;
        uint32_t mapped=im->base;
        /* W32_LDR_IMAGE pins and checks private read-only backing, so the
         * digest remains valid across the serialization wait. Hashing twice in
         * the section lookup doubled the SHA-256 work of each Win32 start. */
        err=image_map_serialized(p,f,im,builtin,&mapped,digest);
        if(err && !(flags&W32_LDR_EXACT) && !builtin && (im->directories[11]) && !(im->flags&1)){
            im->base=0;
            err=image_map_serialized(p,f,im,false,&mapped,digest);
        }
        if(err)return err;
        im->base=mapped;
    }
    *address=im->base;
    /* Structural walkers may read a section lacking IMAGE_SCN_MEM_READ.
     * Restore the original characteristics after binding. No private copy is
     * allocated until an actual relocation/IAT/TLS write. */
    for(uint32_t off=0;off<PAGE_ALIGN_UP(im->bytes);off+=PAGE_SIZE){
        struct ua_extent *e=ua_find(p->memory,*address+off);
        if(e && !(e->prot&PROT_READ)){
            struct ciuki_w32_vm a={.address=*address+off,.bytes=PAGE_SIZE,.protect=W32_PAGE_READONLY};
            uint32_t status=w32_vm_protect(p->memory,&a);
            if(status){if(!reused)unmap_view(p,*address,true);return -ENOMEM;}
        }
    }
    return 0;
}
