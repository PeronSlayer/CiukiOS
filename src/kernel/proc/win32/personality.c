/* PE spawn handoff, kernel-owned TEBs and private emergency stacks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/win32.h>
#include <ciuki/files.h>

static int copy_string(struct uaddr *u, uint32_t dst, const struct proc_strings *s,
                       uint32_t offset, uint32_t *bytes)
{
    *bytes = 0;
    while (offset < s->used) {
        char c = ((const char *)P2V(s->pages[offset / PAGE_SIZE]))[offset % PAGE_SIZE];
        int err = ua_write(u, dst + *bytes, &c, 1);
        if (err) return err;
        offset++; (*bytes)++;
        if (!c) return 0;
    }
    return -EINVAL;
}

int w32_thread_init(struct proc_thread *t, bool main)
{
    struct process *p = t->process;
    struct uaddr *u = p->memory;
    uint32_t reserve = t->w32_reserve ? t->w32_reserve : p->w32_stack_reserve ? p->w32_stack_reserve : 1024 * 1024;
    uint32_t commit = main || !t->stack_bytes ? p->w32_stack_commit : t->stack_bytes;
    if (!commit) commit = PAGE_SIZE;
    uint32_t available = CIUKI_W32_APP_LIMIT - CIUKI_W32_LOW;
    if (reserve > available || commit > available - 2 * PAGE_SIZE) return -EINVAL;
    commit = PAGE_ALIGN_UP(commit);
    if (reserve < commit + 2 * PAGE_SIZE) reserve = commit + 2 * PAGE_SIZE;
    reserve = (reserve + 65535) & ~65535u;
    struct ciuki_w32_vm a = { .bytes = reserve, .flags = W32_MEM_RESERVE, .protect = W32_PAGE_READWRITE };
    uint32_t status = w32_vm_alloc(u, &a);
    if (status) return status == W32_STATUS_NO_MEMORY ? -ENOMEM : -EINVAL;
    t->stack_base = a.address; t->stack_bytes = a.bytes;
    uint32_t top = a.address + a.bytes;
    t->stack_limit = top - commit; t->stack_guard = t->stack_limit - PAGE_SIZE;
    for (struct ua_extent *e = ua_find(u, a.address); e && e->w32_allocation == a.address; e = e->next) e->owner = t->tid;
    int err = ua_w32_change(u, t->stack_limit, commit, W32_VM_ALLOC, W32_PAGE_READWRITE, PROT_READ | PROT_WRITE, true);
    if (!err) err = ua_w32_change(u, t->stack_guard, PAGE_SIZE, W32_VM_ALLOC, W32_PAGE_READWRITE | W32_PAGE_GUARD, 0, true);
    if (!err) err = w32_map_system(u, CIUKI_W32_TEB_SIZE, t->tid, &t->teb);
    if (!err) err = w32_map_system(u, 2 * PAGE_SIZE, t->tid, &t->emergency);
    if (err) return err;
    uint32_t prefix[14] = { UINT32_MAX, top, t->stack_limit, 0, 0x1e00, 0,
        t->teb, 0, p->pid, t->tid, 0, 0, p->w32_peb, 0 };
    err = ua_write(u, t->teb, prefix, sizeof(prefix));
    return err;
}

void w32_thread_cleanup(struct proc_thread *t)
{
    while (t->w32_delivery) {
        struct w32_delivery *d = t->w32_delivery;
        t->w32_delivery = d->next; kfree(d);
    }
}

int w32_spawn(struct process *p, const struct ciuki_file *file, const struct proc_strings *s, uint64_t mask)
{
    if (!s->argc || s->argc > 2) return -EINVAL;
    uint32_t scratch = pmm_alloc();
    if (!scratch) return -ENOMEM;
    struct w32_image *im = P2V(scratch);
    _Static_assert(sizeof(*im) <= PAGE_SIZE, "PE metadata scratch");
    int err = w32_pe_validate(file, im);
    if (err) { klog("[win32] load check=%s error=193", im->failure ? im->failure : "read"); goto done; }
    if (im->flags & 0x2000) { err = -ENOEXEC; goto done; }
    p->personality = PROC_WIN32; p->memory->win32 = true;
    p->w32_image = im->base; p->w32_stack_reserve = im->stack_reserve; p->w32_stack_commit = im->stack_commit;
    err = w32_image_map(p, file, im, false);
    if (err) goto done;
    const struct ciuki_file_ops *ops = proc_get_file_ops();
    struct ciuki_file loader = {0};
    err = ops ? ops->open(p->cwd, "/System/Win32/ntdll.dll", &loader) : -ENOENT;
    if (err) goto done;
    err = w32_pe_validate(&loader, im);
    if (!err && !(im->flags & 0x2000)) err = -ENOEXEC;
    if (!err) err = w32_image_map(p, &loader, im, true);
    uint32_t entry = im->base + im->entry;
    if (loader.close) loader.close(loader.cookie);
    if (err) goto done;
    err = w32_map_system(p->memory, PAGE_SIZE, 0, &p->w32_peb);
    uint32_t parameters = 0;
    if (!err) err = w32_map_system(p->memory, PAGE_ALIGN_UP(s->used + CIUKI_PATH_MAX + 64), 0, &parameters);
    if (err) goto done;
    struct ciuki_w32_parameters params = { .version = 1, .size = sizeof(params) };
    uint32_t at = parameters + sizeof(params), bytes;
    params.image_path = at;
    err = copy_string(p->memory, at, s, s->argv[0], &bytes); at += bytes;
    params.command_line = at;
    if (!err && s->argc == 2) err = copy_string(p->memory, at, s, s->argv[1], &bytes);
    else { bytes = 1; } /* zero backing supplies the empty command line */
    at += bytes;
    params.environment = at;
    for (unsigned i = 0; !err && i < s->envc; i++) {
        err = copy_string(p->memory, at, s, s->envp[i], &bytes); at += bytes;
    }
    at += s->envc ? 1 : 2; /* double NUL */
    params.directory = at;
    /* Cwd is retained by the native spawn transaction. */
    char *path = P2V(scratch); /* image metadata no longer needed */
    if (!err && p->cwd) {
        struct px_node *cwd = p->cwd;
        fs_lock_take(&cwd->space->vfs->lock);
        err = px_getcwd_locked(cwd, path);
        fs_lock_drop(&cwd->space->vfs->lock);
    }
    else { path[0] = '/'; path[1] = 0; }
    if (!err) err = ua_write(p->memory, at, path, (uint32_t)strlen(path) + 1);
    if (!err) err = ua_write(p->memory, parameters, &params, sizeof(params));
    if (!err) err = ua_write(p->memory, p->w32_peb + 8, &p->w32_image, 4);
    if (!err) err = ua_write(p->memory, p->w32_peb + 0x10, &parameters, 4);
    uint32_t version[4] = {4, 10, 2222, 1};
    if (!err) err = ua_write(p->memory, p->w32_peb + 0xa4, version, sizeof(version));
    if (!err) {
        err = ua_map_at(p->memory, CIUKI_W32_SHARED, PAGE_SIZE, 0, PROT_READ, UA_W32, 0);
        if (!err) {
            struct ua_extent *e = ua_find(p->memory, CIUKI_W32_SHARED);
            e->w32_allocation = CIUKI_W32_SHARED; e->w32_type = W32_MEM_MAPPED;
            e->w32_allocation_protect = W32_PAGE_READONLY;
            err = ua_w32_change(p->memory, CIUKI_W32_SHARED, PAGE_SIZE, W32_VM_ALLOC, W32_PAGE_READONLY, PROT_READ, true);
            e = ua_find(p->memory, CIUKI_W32_SHARED); e->w32_locked = true;
            const uint8_t gate[] = {0xcd, 0x80, 0xc3};
            if (!err) err = ua_write(p->memory, CIUKI_W32_GATE, gate, sizeof(gate));
        }
    }
    struct proc_thread *t = 0;
    if (!err) err = proc_thread_prepare(p, entry, p->w32_peb, 0, 0, 0, mask, true, &t);
    if (!err) {
        uint32_t top = t->stack_base + t->stack_bytes - 8;
        uint32_t frame[2] = {0, p->w32_peb};
        err = ua_write(p->memory, top, frame, sizeof(frame));
        if (!err) task_native_frame(t->task, top);
    }
done:
    pmm_free(scratch);
    return err;
}
