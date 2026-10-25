/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Production personality startup. */
#include <w32/store.h>
#include <w32/shims.h>
#include <ciuki/abi.h>
#include <ciuki/w32_core.h>
int w32_ciuki_store(struct w32_loader *l, struct w32_file *snapshot) {
    const struct w32_services *s = l->services;
    if (!s->open(s->opaque, "C:\\System\\Win32\\BUILTIN.BIN", snapshot))
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
        w32_copy_string(paths + i * 96, 96, "C:\\System\\Win32\\");
        w32_copy_string(paths + i * 96 + 16, 80, row[i].name);
        b[i] = (struct w32_builtin){row[i].name, paths + i * 96, row[i].sha256,
                                    row[i].base, row[i].size,    {0}};
        w32_copy(b[i].version, row[i].version, 8);
    }
    l->builtins = b;
    l->builtin_count = count;
    return w32_builtin_validate(b, count, &l->error);
}
static WCHAR *unicode(const struct w32_services *s, const char *text, UNICODE_STRING *out, int utf8) {
    ULONG n = w32_strlen(text);
    if (n > 32766)
        return 0;
    WCHAR *p = s->alloc(s->opaque, (n + 1) * 2);
    if (!p)
        return 0;
    if (utf8) {
        if (!w32_utf8_path(text,p,n+1)) { s->free(s->opaque,p,(n+1)*2); return 0; }
        n=0;while (p[n]) n++;
    } else for (ULONG i = 0; i <= n; i++) p[i]=w32_acp_character((uint8_t)text[i]);
    if (out) {
        out->Length = n * 2;
        out->MaximumLength = (n + 1) * 2;
        out->Buffer = p;
    }
    return p;
}
/* q3exec: Windows 98 starts every program with IO.SYS's default environment
 * ("tmp=c:\windows\temp", "temp=c:\windows\temp": Windows 95 Resource
 * Kit, "How Windows 95 processes Autoexec.bat"; the same values in a Windows
 * 98 SET listing). A program launched by the native desktop inherits only
 * the native environment, so the absent names are added here; an inherited
 * value wins. Installers locate their temporary directory this way: the
 * MindVision VISE stub of the Quake III Arena demo tries TMP, then TEMP, then
 * "<Windows drive>\TEMP", a directory CiukiOS does not create (its Windows
 * 98 tree has C:\WINDOWS\TEMP), and cannot unpack its engine without one. */
static const char *const default_environment[] = {"TMP=C:\\WINDOWS\\TEMP", "TEMP=C:\\WINDOWS\\TEMP"};
static int environment_has(const char *environment, const char *entry) {
    ULONG n = 0;
    while (entry[n] != '=')
        n++;
    for (const char *q = environment; *q; q += w32_strlen(q) + 1) {
        ULONG i = 0;
        while (i < n && q[i] && (q[i] | 32) == (entry[i] | 32))
            i++;
        if (i == n && q[n] == '=')
            return 1;
    }
    return 0;
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
        if(v->title&&!unicode(l->services,(const char *)(uintptr_t)v->title,&p->WindowTitle,0))return STATUS_NO_MEMORY;
    }
    /* Initial native-to-PE shell launch has no Win32 STARTUPINFO record.
     * Never inherit this private fallback over an explicit CreateProcess
     * startup (including an explicit startup without USESHOWWINDOW). */
    if(!boot || boot->version!=3){
        for(const char *q=environment;*q;q+=w32_strlen(q)+1){
            static const char key[]="CIUKI_SHELL_SHOW=";
            unsigned i=0;while(i<sizeof(key)-1 && q[i] && q[i]==key[i])i++;
            if(i==sizeof(key)-1 && (q[i]=='1' || q[i]=='3' || q[i]=='7') && !q[i+1]){
                p->WindowFlags|=1;p->ShowWindowFlags=(unsigned)(q[i]-'0');break;
            }
        }
    }

    if (!unicode(l->services, exe, &p->ImagePathName,1) ||
        !unicode(l->services, command, &p->CommandLine,0) ||
        !unicode(l->services, directory, &p->CurrentDirectory.DosPath,1))
        return STATUS_NO_MEMORY;
    ULONG size = 0;
    do {
        while (environment[size++])
            ;
    } while (environment[size]);
    size++;
    ULONG entries = environment[0] ? size - 1 : 0, total = entries;
    for (unsigned d = 0; d < sizeof(default_environment) / sizeof(*default_environment); d++)
        if (!environment_has(environment, default_environment[d]))
            total += w32_strlen(default_environment[d]) + 1;
    total += total ? 1 : 2;
    p->Environment = l->services->alloc(0, total * 2);
    if (!p->Environment)
        return STATUS_NO_MEMORY;
    for (ULONG i = 0; i < entries; i++)
        p->Environment[i] = w32_acp_character((uint8_t)environment[i]);
    ULONG at = entries;
    for (unsigned d = 0; d < sizeof(default_environment) / sizeof(*default_environment); d++)
        if (!environment_has(environment, default_environment[d]))
            for (const char *c = default_environment[d];; c++) {
                p->Environment[at++] = (uint8_t)*c;
                if (!*c)
                    break;
            }
    while (at < total)
        p->Environment[at++] = 0;
    for (const char *q = environment; *q; q += w32_strlen(q) + 1)
        if ((q[0] == 'P' || q[0] == 'p') && (q[1] == 'A' || q[1] == 'a') &&
            (q[2] == 'T' || q[2] == 't') && (q[3] == 'H' || q[3] == 'h') && q[4] == '=')
        {
            UNICODE_STRING path;
            WCHAR *wide=unicode(l->services,q+5,&path,0);
            ULONG cap=w32_strlen(q+5)*3+1;
            char *narrow=l->services->alloc(0,cap);
            if (!wide || !narrow || !w32_utf16_path(wide,path.Length/2,narrow,cap)) return STATUS_NO_MEMORY;
            l->services->free(0,wide,path.MaximumLength);
            l->path=narrow;
        }
    peb->SubSystemData = peb->ProcessParameters; /* retain v2 endpoints/startup handoff */
    peb->ProcessParameters = p;
    w32_copy_string(l->current_directory, sizeof(l->current_directory), directory);
    if(!w32_shim_start(l,exe))return STATUS_INVALID_PARAMETER;
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
    const struct ciuki_w32_parameters_v2 *services=(const void *)peb->ProcessParameters;
    if(services->desktop_endpoint==UINT32_MAX||services->w32srv_endpoint==UINT32_MAX){
        w32_report_refusal(W32_REFUSE_SERVER,"w32srv",1062);w32_terminate(STATUS_DLL_INIT_FAILED);
    }
    /* start() resets the loader; retain manifest ownership outside it. */
    if (!w32_ciuki_store(&process_loader, &store_snapshot)) {
        w32_diag("load", "builtin-store", "builtin-manifest");
        w32_terminate(0xc000007b);
    }
    /* A real child already exists, so the parent's process handle remains
     * waitable. Do not bind or execute the redistributable's original code. */
    if(w32_installer_intercept(&process_loader,(const char *)(uintptr_t)bootstrap.image_path)){
        w32_ldr_shutdown(&process_loader);
        w32_terminate(0);
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
