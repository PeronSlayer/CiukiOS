/* PE spawn handoff, kernel-owned TEBs and private emergency stacks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
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
    if(w32_thread_objects_init(t))return -ENOMEM;
    struct process *p = t->process;
    if(p->live_threads>p->w32->peak_threads)p->w32->peak_threads=p->live_threads;
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
    w32_thread_objects_cleanup(t);
    while (t->w32_delivery) {
        struct w32_delivery *d = t->w32_delivery;
        t->w32_delivery = d->next; kfree(d);
    }
}

/* Only the two IS5 stub operations are replaced; no Win16 code is run. */
#include "is5boot.h"
/* fx-autorun: a 16-bit Windows program (MZ with an "NE" header whose target
 * OS byte, NE+0x36, is 2) that the IS5 admission did not take. Windows 9x
 * runs it in its Win16 subsystem; CiukiOS has none yet (the Win16 subsystem
 * follows the 1.0 Beta), so the refusal names that instead of calling the
 * file an invalid Win32 application. Classification as Wine 10.0
 * server/mapping.c get_image_params (STATUS_INVALID_IMAGE_WIN_16). */
static bool w32_win16_image(const struct ciuki_file *file) {
    uint8_t dos[64],ne[64];
    if(file->bytes<64 || file->read(file->cookie,0,dos,64) || is5_word(dos)!=0x5a4d)return false;
    uint32_t h=is5_word(dos+60)|(uint32_t)is5_word(dos+62)<<16;
    return h>=64 && h<=file->bytes && file->bytes-h>=64 && !file->read(file->cookie,h,ne,64) &&
           is5_word(ne)==0x454e && ne[54]==2;
}
int w32_spawn(struct process *p, const struct ciuki_file *file, const struct proc_strings *s, uint64_t mask)
{
    if (!s->argc || s->argc > 2) return -EINVAL;
    int bootstrap = is5_redirect(p, file, s, mask);
    if (bootstrap != 1) return bootstrap;
    if (w32_win16_image(file)) {
        klog("[win32] load check=win16-application error=193");
        w32_refusal_queue(p,W32_REFUSE_WIN16,p->name,193);
        return -ENOEXEC;
    }
    uint32_t scratch = pmm_alloc();
    if (!scratch) return -ENOMEM;
    struct w32_image *im = P2V(scratch);
    _Static_assert(sizeof(*im) <= PAGE_SIZE, "PE metadata scratch");
    int err = w32_pe_validate(file, im);
    if (err) { klog("[win32] load check=%s error=193", im->failure ? im->failure : "read");
        w32_refusal_queue(p,im->failure&&(!strncmp(im->failure,"machine-not-i386",17)||!strncmp(im->failure,"unsupported-subsystem",21)||!strncmp(im->failure,"optional-magic-not-pe32",23)||!strncmp(im->failure,"unsupported-runtime",20))?W32_REFUSE_RUNTIME:W32_REFUSE_IMAGE,p->name,193);goto done; }
    /* fx-autorun: a DLL started as a program gets the same "not a valid
     * Win32 application" dialog as any other image refusal, so a refused
     * spawn (-ENOEXEC) is always reported to the user in words. */
    if (im->flags & 0x2000) { klog("[win32] load check=expected-executable error=193");w32_refusal_queue(p,W32_REFUSE_IMAGE,p->name,193);err = -ENOEXEC; goto done; }
    p->personality = PROC_WIN32; p->memory->win32 = true;
    err = w32_process_init(p);
    if (err) goto done;
    p->w32->gui = im->subsystem == 2;
    err=w32_diagnostic_image(p,im->timestamp,im->bytes);if(err)goto done;
    p->w32_image = im->base; p->w32_stack_reserve = im->stack_reserve; p->w32_stack_commit = im->stack_commit;
    err = w32_image_map(p, file, im, false);
    if (err) {klog("[win32] load check=image-address-range error=193");goto done;}
    const struct ciuki_file_ops *ops = proc_get_file_ops();
    struct ciuki_file loader = {0};
    err = ops ? ops->open(p->cwd, "/System/Win32/ntdll.dll", &loader) : -ENOENT;
    if (err) { klog("[win32] load pid=%u check=missing-dll error=126",p->pid);w32_refusal_queue(p,W32_REFUSE_MISSING_DLL,"ntdll.dll",126); goto done; }
    err = w32_pe_validate(&loader, im);
    if (!err && !(im->flags & 0x2000)) err = -ENOEXEC;
    if (!err) err = w32_image_map(p, &loader, im, true);
    uint32_t entry = 0;
    if(!err)err=w32_bootstrap_entry(p,im,&entry);
    if(err)klog("[win32] load pid=%u check=builtin-entry error=193",p->pid);
    if (loader.close) loader.close(loader.cookie);
    if (err) goto done;
    err = w32_map_system(p->memory, PAGE_SIZE, 0, &p->w32_peb);
    uint32_t parameters = 0;
    if (!err) err = w32_map_system(p->memory, PAGE_ALIGN_UP(s->used + CIUKI_PATH_MAX + 64), 0, &parameters);
    if (err) goto done;
    struct ciuki_w32_parameters_v2 extended = { .core = { .version = 2, .size = sizeof(extended) }, .desktop_endpoint=UINT32_MAX, .w32srv_endpoint=UINT32_MAX };
    struct ciuki_w32_parameters *params = &extended.core;
    uint32_t at = parameters + sizeof(extended), bytes;
    params->image_path = at;
    err = copy_string(p->memory, at, s, s->argv[0], &bytes); at += bytes;
    params->command_line = at;
    if (!err && s->argc == 2) err = copy_string(p->memory, at, s, s->argv[1], &bytes);
    else { bytes = 1; } /* zero backing supplies the empty command line */
    at += bytes;
    params->environment = at;
    for (unsigned i = 0; !err && i < s->envc; i++) {
        err = copy_string(p->memory, at, s, s->envp[i], &bytes); at += bytes;
    }
    at += s->envc ? 1 : 2; /* double NUL */
    params->directory = at;
    /* Cwd is retained by the native spawn transaction. */
    char *path = P2V(scratch); /* image metadata no longer needed */
    if (!err && p->cwd) {
        struct px_node *cwd = p->cwd;
        vfs_lock(cwd->space->vfs,0);
        err = px_getcwd_locked(cwd, path);
        vfs_unlock(cwd->space->vfs);
    }
    else { path[0] = '/'; path[1] = 0; }
    if (!err) err = ua_write(p->memory, at, path, (uint32_t)strlen(path) + 1);
    uint32_t endpoints[2];
    if(!err)err=w32_services_attach(p,endpoints);
    if(!err){extended.desktop_endpoint=endpoints[0];extended.w32srv_endpoint=endpoints[1];}
    if(!err)err=w32_loader_snapshot(p,file,&p->w32_source);
    if(!err)p->w32_source_bytes=file->bytes;
    if(!err){
        unsigned n=0;
        do{err=ua_read(p->memory,p->w32_source_name+n,params->image_path+n,1);if(err)break;}
        while(p->w32_source_name[n++] && n<sizeof(p->w32_source_name));
        if(!err && p->w32_source_name[n-1])err=-ENAMETOOLONG;
    }
    if (!err) err = ua_write(p->memory, parameters, &extended, sizeof(extended));
    if (!err) err = ua_write(p->memory, p->w32_peb + 8, &p->w32_image, 4);
    if (!err) err = ua_write(p->memory, p->w32_peb + 0x10, &parameters, 4);
    uint32_t version[4] = {4, 10, 2222, 1};
    if (!err) err = ua_write(p->memory, p->w32_peb + 0xa4, version, sizeof(version));
    if(!err)err=w32_shared_map(p);
    struct proc_thread *t = 0;
    if (!err) err = proc_thread_prepare(p, entry, p->w32_peb, 0, 0, 0, mask, true, &t);
    if (!err) {
        uint32_t top = t->stack_base + t->stack_bytes - 8;
        uint32_t frame[2] = {0, p->w32_peb};
        err = ua_write(p->memory, top, frame, sizeof(frame));
        if (!err) task_native_frame(t->task, top);
    }
done:
    if (!err && p->w32) p->w32->launch_feedback_until = w32_now() + 5000;
    pmm_free(scratch);
    return err;
}
