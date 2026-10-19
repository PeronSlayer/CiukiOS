/* User-mode loader transport over immutable snapshots and f5-04 sections.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_objects.h>
#include <ciuki/files.h>
/* Translate only the loader's Windows namespace; native open stays unchanged.
 * /mnt/<letter> is the existing VFS view, never a hardcoded disk identity. */
int w32_loader_open(struct process *p, const char *name, struct ciuki_file *out) {
    const struct ciuki_file_ops *ops = proc_get_file_ops();
    if (!ops)
        return -ENOENT;
    char path[CIUKI_PATH_MAX], cwd[CIUKI_PATH_MAX], normal[FS_PATH_BYTES];
    unsigned drive = p->cwd ? ((struct px_node *)p->cwd)->drive : 2;
    if (drive >= 26)
        drive = 2;
    if (!name[0] || ((name[0] == '\\' || name[0] == '/') && (name[1] == '\\' || name[1] == '/')))
        return -ENOENT;
    if (p->cwd) {
        struct px_node *node = p->cwd;
        vfs_lock(node->space->vfs, 0);
        int e = px_getcwd_locked(node, cwd);
        vfs_unlock(node->space->vfs);
        if (e)
            return e;
    } else
        memcpy(cwd, "/", 2);
    const char *relative = cwd;
    if (cwd[0] == '/' && !strncmp(cwd, "/mnt/", 5) && cwd[5] && cwd[6] == '/')
        relative = cwd + 6;
    int err = path_normalize(name, drive, relative, &drive, normal);
    if (err)
        return -ENOENT;
    /* Trim Win32 trailing spaces/dots, excluding the root. */
    unsigned at = 0;
    if (drive != 2) {
        memcpy(path, "/mnt/", 5);
        path[5] = (char)('A' + drive);
        at = 6;
    }
    for (const char *q = normal; *q;) {
        if (at + 1 >= sizeof(path))
            return -ENAMETOOLONG;
        path[at++] = '/';
        while (*q == '/')
            q++;
        const char *start = q;
        while (*q && *q != '/')
            q++;
        unsigned n = (unsigned)(q - start);
        while (n && (start[n - 1] == ' ' || start[n - 1] == '.'))
            n--;
        if (at + n >= sizeof(path))
            return -ENAMETOOLONG;
        memcpy(path + at, start, n);
        at += n;
    }
    path[at] = 0;
    return ops->open(p->cwd, path, out);
}
struct loader_snapshot {
    struct uaddr *u;
    uint32_t base;
};
static int loader_read(void *cookie, uint32_t offset, void *dst, uint32_t bytes) {
    struct loader_snapshot *s = cookie;
    return ua_read(s->u, dst, s->base + offset, bytes);
}
uint32_t w32_loader_call(struct proc_thread *t, struct ciuki_w32_loader_args *a) {
    struct process *p = t->process;
    if (a->action == W32_LDR_UNMAP) {
        struct ciuki_w32_view_args v = {.action = W32_VIEW_UNMAP, .address = a->address};
        uint32_t status = w32_view_call(t, &v);
        return status ? status : v.error ? W32_STATUS_INVALID_PARAMETER : 0;
    }
    if (a->action == W32_LDR_IMAGE) {
        if (!a->bytes || a->bytes > 512u * 1024 * 1024 ||
            !ua_range(p->memory, a->source, a->bytes, PROT_READ))
            return W32_STATUS_ACCESS_VIOLATION;
        /* Pins freeze the snapshot against release/protect while section hashing
         * and seeding yield. Copying into another image is never kernel code. */
        for (struct ua_extent *e = ua_find(p->memory, a->source);
             e && e->base < a->source + a->bytes; e = e->next)
            if (e->prot & PROT_WRITE)
                return W32_STATUS_ACCESS_DENIED;
        struct ua_pin pin;
        int err = ua_pin(p->memory, &pin, a->source, a->bytes, false);
        if (err)
            return w32_status(err);
        struct loader_snapshot snapshot = {p->memory, a->source};
        struct ciuki_file file = {&snapshot, a->bytes, loader_read, 0};
        struct w32_image *im = w32_alloc(sizeof(*im));
        if (!im)
            err = -ENOMEM;
        else {
            err = w32_pe_validate(&file, im);
            if (!err)
                err = w32_loader_image(p, &file, im, &a->address, a->flags);
            if (err)
                klog("[win32] load pid=%u check=%s error=193", p->pid,
                     im->failure ? im->failure : "relocation-base-collision");
            w32_free(im, sizeof(*im));
        }
        ua_unpin(p->memory, &pin);
        return w32_status(err);
    }
    char path[FS_PATH_BYTES];
    unsigned n = 0;
    int err = 0;
    do {
        err = copy_from_user(&path[n], a->path + n, 1);
        if (err)
            return w32_status(err);
    } while (path[n++] && n < sizeof(path));
    if (path[n - 1])
        return W32_STATUS_INVALID_PARAMETER;
    if (p->w32_source && !strncmp(path, p->w32_source_name, sizeof(p->w32_source_name))) {
        a->address = p->w32_source;
        a->bytes = p->w32_source_bytes;
        p->w32_source = 0;
        return 0;
    }
    struct ciuki_file file = {0};
    err = w32_loader_open(p, path, &file);
    if (err)
        return w32_status(err);
    uint32_t address = 0;
    err = w32_loader_snapshot(p, &file, &address);
    if (file.close)
        file.close(file.cookie);
    if (err)
        return w32_status(err);
    a->address = address;
    a->bytes = file.bytes;
    return 0;
}

int w32_loader_snapshot(struct process *p, const struct ciuki_file *file, uint32_t *address) {
    int err = 0;
    struct ciuki_w32_vm vm = {.bytes = file->bytes ? file->bytes : 1,
                              .flags = W32_MEM_RESERVE | W32_MEM_COMMIT,
                              .protect = W32_PAGE_READWRITE};
    uint32_t status = file->bytes > 512u * 1024 * 1024 ? W32_STATUS_INVALID_IMAGE_FORMAT
                                                       : w32_vm_alloc(p->memory, &vm);
    uint32_t scratch = status ? 0 : pmm_alloc();
    if (!status && !scratch)
        status = W32_STATUS_NO_MEMORY;
    for (uint32_t offset = 0; !status && offset < file->bytes;) {
        uint32_t take = file->bytes - offset;
        if (take > PAGE_SIZE)
            take = PAGE_SIZE;
        err = file->read(file->cookie, offset, P2V(scratch), take);
        if (!err)
            err = ua_write(p->memory, vm.address + offset, P2V(scratch), take);
        if (err)
            status = w32_status(err);
        offset += take;
        kpreempt_point();
    }
    if (scratch)
        pmm_free(scratch);
    if (!status) {
        struct ciuki_w32_vm ro = {
            .address = vm.address, .bytes = vm.bytes, .protect = W32_PAGE_READONLY};
        status = w32_vm_protect(p->memory, &ro);
    }
    if (status) {
        if (vm.address)
            ua_w32_release(p->memory, vm.address, true);
        return status == W32_STATUS_NO_MEMORY ? -ENOMEM : -EIO;
    }
    *address = vm.address;
    return 0;
}
