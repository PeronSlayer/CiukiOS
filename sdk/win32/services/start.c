/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Production personality startup. */
#include <w32/store.h>
#include <ciuki/abi.h>
#include <ciuki/w32_core.h>
int w32_ciuki_store(struct w32_loader *l, struct w32_file *snapshot) {
    const struct w32_services *s = l->services;
    if (!s->open(s->opaque, "C:\\WINDOWS\\SYSTEM\\BUILTIN.BIN", snapshot))
        return 0;
    if (snapshot->size < 8 || w32_u32(snapshot->bytes) != 1)
        return 0;
    ULONG count = w32_u32(snapshot->bytes + 4);
    if (!count || count > W32_MAX_MODULES ||
        snapshot->size != 8 + count * sizeof(struct w32_store_row))
        return 0;
    struct w32_builtin *b = s->alloc(s->opaque, count * sizeof(*b));
    char *paths = s->alloc(s->opaque, count * 96);
    if (!b || !paths)
        return 0;
    w32_fill(b, 0, count * sizeof(*b));
    const struct w32_store_row *row = (const void *)(snapshot->bytes + 8);
    for (ULONG i = 0; i < count; i++) {
        if (!row[i].name[0] || row[i].name[63] || row[i].sha256[64])
            return 0;
        for (unsigned j = 0; row[i].name[j]; j++)
            if (row[i].name[j] == '/' || row[i].name[j] == '\\' || row[i].name[j] == ':')
                return 0;
        w32_copy_string(paths + i * 96, 96, "C:\\WINDOWS\\SYSTEM\\");
        w32_copy_string(paths + i * 96 + 18, 78, row[i].name);
        b[i] = (struct w32_builtin){row[i].name, paths + i * 96, row[i].sha256,
                                    row[i].base, row[i].size,    {0}};
        w32_copy(b[i].version, row[i].version, 8);
    }
    l->builtins = b;
    l->builtin_count = count;
    return w32_builtin_validate(b, count, &l->error);
}
static WCHAR *unicode(const struct w32_services *s, const char *text, UNICODE_STRING *out) {
    ULONG n = w32_strlen(text);
    if (n > 32766)
        return 0;
    WCHAR *p = s->alloc(s->opaque, (n + 1) * 2);
    if (!p)
        return 0;
    for (ULONG i = 0; i <= n; i++)
        p[i] = (uint8_t)text[i];
    if (out) {
        out->Length = n * 2;
        out->MaximumLength = (n + 1) * 2;
        out->Buffer = p;
    }
    return p;
}
NTSTATUS w32_ciuki_start(struct w32_loader *l, struct w32_peb *peb, const char *exe,
                         const char *command, const char *directory, const char *environment,
                         struct w32_module **main) {
    const struct w32_builtin *builtins = l->builtins;
    ULONG count = l->builtin_count;
    w32_ldr_init(l, l->services ? l->services : &w32_ciuki_services, peb);
    w32_ldr_bind(l);
    l->builtins = builtins;
    l->builtin_count = count;
    struct w32_process_parameters *p = l->services->alloc(0, sizeof(*p));
    if (!p)
        return STATUS_NO_MEMORY;
    w32_fill(p, 0, sizeof(*p));
    p->MaximumLength = p->Length = sizeof(*p);
    p->Flags = 1;
    const struct ciuki_w32_parameters *boot=(const void *)peb->ProcessParameters;
    if(boot&&boot->version==3&&boot->size>=sizeof(struct ciuki_w32_parameters_v3)){
        const struct ciuki_w32_startup *v=&((const struct ciuki_w32_parameters_v3 *)boot)->startup;
        p->StartingX=v->x;p->StartingY=v->y;p->CountX=v->width;p->CountY=v->height;
        p->CountCharsX=v->chars_x;p->CountCharsY=v->chars_y;p->FillAttribute=v->fill;p->WindowFlags=v->flags;p->ShowWindowFlags=v->show;
        if(v->flags&0x100){p->StandardInput=(void *)(uintptr_t)v->input;p->StandardOutput=(void *)(uintptr_t)v->output;p->StandardError=(void *)(uintptr_t)v->error;}
        if(v->title&&!unicode(l->services,(const char *)(uintptr_t)v->title,&p->WindowTitle))return STATUS_NO_MEMORY;
    }

    if (!unicode(l->services, exe, &p->ImagePathName) ||
        !unicode(l->services, command, &p->CommandLine) ||
        !unicode(l->services, directory, &p->CurrentDirectory.DosPath))
        return STATUS_NO_MEMORY;
    ULONG size = 0;
    do {
        while (environment[size++])
            ;
    } while (environment[size]);
    size++;
    p->Environment = l->services->alloc(0, size * 2);
    if (!p->Environment)
        return STATUS_NO_MEMORY;
    for (ULONG i = 0; i < size; i++)
        p->Environment[i] = (uint8_t)environment[i];
    for (const char *q = environment; *q; q += w32_strlen(q) + 1)
        if ((q[0] == 'P' || q[0] == 'p') && (q[1] == 'A' || q[1] == 'a') &&
            (q[2] == 'T' || q[2] == 't') && (q[3] == 'H' || q[3] == 'h') && q[4] == '=')
            l->path = q + 5;
    peb->SubSystemData = peb->ProcessParameters; /* retain v2 endpoints/startup handoff */
    peb->ProcessParameters = p;
    w32_copy_string(l->current_directory, sizeof(l->current_directory), directory);
    if (!w32_ldr_thread_attach(l, NtCurrentTeb()))
        return STATUS_NO_MEMORY;
    if (l->builtin_count) {
        struct w32_module *core;
        l->process_start = 1;
        int loaded = w32_ldr_load(l, "ntdll.dll", 0, &core);
        l->process_start = 0;
        if (!loaded)
            return STATUS_DLL_INIT_FAILED;
    }
    return w32_ldr_start_process(l, exe, main);
}
#ifndef W32_HOST_MODEL
static struct w32_loader process_loader;
static struct w32_file store_snapshot;
/* PE's DllMain is distinct from the kernel's bootstrap export. */
int W32API CiukiDllMain(void *module, ULONG reason, void *reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
W32_NORETURN void W32API CiukiProcessStart(struct w32_peb *peb) {
    struct ciuki_w32_parameters bootstrap =
        *(const struct ciuki_w32_parameters *)peb->ProcessParameters;
    if ((bootstrap.version != 2 && bootstrap.version != 3) || bootstrap.size < 32) {
        w32_set_services(&w32_ciuki_services);
        w32_terminate(STATUS_INVALID_PARAMETER);
    }
    /* Install services and the dispatcher before any image callbacks. */
    w32_ldr_init(&process_loader, &w32_ciuki_services, peb);
    w32_ldr_bind(&process_loader);
    struct ciuki_w32_dispatcher d = {.header = {1, sizeof(d), W32_DISPATCHER, 0},
                                     .entry = (ULONG)(uintptr_t)KiUserExceptionDispatcher};
    if (w32_ciuki_call(&d, sizeof(d)))
        w32_terminate(STATUS_INVALID_PARAMETER);
    /* start() resets the loader; retain manifest ownership outside it. */
    if (!w32_ciuki_store(&process_loader, &store_snapshot)) {
        w32_diag("load", "builtin-store", "builtin-manifest");
        w32_terminate(0xc000007b);
    }
    const struct w32_builtin *builtins = process_loader.builtins;
    ULONG count = process_loader.builtin_count;
    /* Build public parameters first, then load with the manifest installed. */
    struct w32_module *main;
    /* start() below preserves the supplied builtin table. */
    process_loader.builtins = builtins;
    process_loader.builtin_count = count;
    NTSTATUS status =
        w32_ciuki_start(&process_loader, peb, (const char *)(uintptr_t)bootstrap.image_path,
                        (const char *)(uintptr_t)bootstrap.command_line,
                        (const char *)(uintptr_t)bootstrap.directory,
                        (const char *)(uintptr_t)bootstrap.environment, &main);
    if (status)
        w32_terminate(status);
    if(main->pe.subsystem==3&&!peb->ProcessParameters->StandardOutput){
        struct w32_module *kernel;
        if(w32_ldr_load(&process_loader,"kernel32.dll",0,&kernel)){
            void *allocate=w32_ldr_proc(&process_loader,kernel,"AllocConsole",0,0xffffffffu);
            if(allocate)((int(W32API *)(void))allocate)();
        }
    }
    ULONG result = ((ULONG(W32API *)(void))(main->pe.image + main->pe.entry))();
    w32_ldr_shutdown(&process_loader);
    w32_terminate(result);
}
#endif
