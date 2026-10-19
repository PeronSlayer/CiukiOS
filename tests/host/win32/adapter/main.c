/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "linux.h"
static struct w32_loader loader;
static struct w32_pe pe;
static struct w32_error err;
#ifndef W32_HOST_MODEL
static struct w32_builtin core_builtins[3];
static char core_paths[3][W32_PATH_MAX];
static const char zero_hash[] = "0000000000000000000000000000000000000000000000000000000000000000";
/* Register DIRECTORY/NAME as a builtin at its linked base, if present. */
static int add_builtin(const char *directory, const char *name, ULONG i) {
    ULONG n = w32_strlen(directory), k = w32_strlen(name);
    if (n + k + 2 >= W32_PATH_MAX)
        return 0;
    w32_copy(core_paths[i], directory, n);
    core_paths[i][n++] = '/';
    w32_copy(core_paths[i] + n, name, k + 1);
    struct w32_file file;
    if (!host.services.open(&host, core_paths[i], &file))
        return 0;
    int valid = w32_pe_parse(&pe, file.bytes, file.size, &err);
    host.services.close(&host, &file);
    if (!valid)
        return 0;
    core_builtins[i] = (struct w32_builtin){name, core_paths[i], zero_hash, pe.preferred, pe.size,
                                            {0, 0, 0, 0}};
    return 1;
}
#endif
int host_main(int argc, char **argv) {
    if (argc < 3) {
        host_print("usage: w32host validate|load|exec IMAGE [DLL_DIRECTORY] [observe]\n");
        return 2;
    }
    if (!w32_strcmp(argv[1], "validate")) {
        struct w32_file f;
        if (!host.services.open(&host, argv[2], &f))
            return 3;
        int ok = w32_pe_parse(&pe, f.bytes, f.size, &err);
        if (ok) {
            void *image = host.services.alloc(&host, pe.size);
            ok = w32_pe_copy_image(&pe, image) && w32_pe_validate(&pe);
            if (image)
                host.services.free(&host, image, pe.size);
        }
        host.services.close(&host, &f);
        if (ok) {
            host_print("PASS validate\n");
            return 0;
        }
        host_print("REFUSE ");
        host_print(err.reason ? err.reason : "allocation");
        host_print(" ");
        host_number(err.code);
        host_print("\n");
        return 1;
    }
    if (w32_strcmp(argv[1], "load") && w32_strcmp(argv[1], "exec") &&
        w32_strcmp(argv[1], "exec-core"))
        return 2;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    if (argc > 3)
        w32_copy_string(loader.exe_directory, sizeof(loader.exe_directory), argv[3]);
    host.observer = argc > 4 && !w32_strcmp(argv[4], "observe");
    if (!w32_ldr_thread_attach(&loader, &host.teb))
        return 4;
    if (!w32_strcmp(argv[1], "exec-core")) {
#ifdef W32_HOST_MODEL
        host_print("H0 CANNOT EXECUTE PE\n");
        return 91;
#else
        if (argc < 4)
            return 2;
        if (!add_builtin(argv[3], "ntdll.dll", 0))
            return 3;
        loader.builtins = core_builtins;
        /* Optional H1 test shim (tests/host/win32/adapter/kernel32_shim.c). */
        loader.builtin_count = add_builtin(argv[3], "kernel32.dll", 1) ? 2 : 1;
        /* Optional H1 USER shim (tests/host/win32/input): core names resolve only
         * from the builtin table since f5-06B, so a staged user32.dll joins it. */
        if (loader.builtin_count == 2 && add_builtin(argv[3], "user32.dll", 2))
            loader.builtin_count = 3;
        struct w32_module *core;
        if (!w32_ldr_load(&loader, "ntdll.dll", 0, &core))
            return 3;
        void (*bind_services)(const struct w32_services *) =
            w32_ldr_proc(&loader, core, "w32_set_services", 0, 0xffffffffu);
        void (*bind_loader)(struct w32_loader *) =
            w32_ldr_proc(&loader, core, "w32_ldr_bind", 0, 0xffffffffu);
        void *dispatcher = w32_ldr_proc(&loader, core, "KiUserExceptionDispatcher", 0, 0xffffffffu);
        if (!bind_services || !bind_loader || !dispatcher)
            return 3;
        host.dispatcher = dispatcher;
        bind_services(&host.services);
        bind_loader(&loader);
#endif
    }
    struct w32_module *m;
    int executing = !w32_strcmp(argv[1], "exec") || !w32_strcmp(argv[1], "exec-core");
    NTSTATUS start = 0;
    int ok = executing ? !(start = w32_ldr_start_process(&loader, argv[2], &m))
                       : w32_ldr_load(&loader, argv[2], 0, &m);
    if (!ok) {
        host_print("REFUSE ");
        host_print(loader.error.reason ? loader.error.reason : "unknown");
        host_print(" ");
        host_number(loader.error.code);
        if (start) {
            host_print(" STATUS ");
            host_number(start);
        }
        host_print("\n");
    } else if (executing) {
        void *stack = host.services.alloc(&host, 1024 * 1024);
        if (!stack)
            return 4;
        void *oldbase = host.teb.Tib.StackBase, *oldlimit = host.teb.Tib.StackLimit;
        host.teb.Tib.StackLimit = stack;
        host.teb.Tib.StackBase = (uint8_t *)stack + 1024 * 1024;
        int value = host_entry(m->pe.image + m->pe.entry, host.teb.Tib.StackBase);
        host.teb.Tib.StackBase = oldbase;
        host.teb.Tib.StackLimit = oldlimit;
        host.services.free(&host, stack, 1024 * 1024);
        host_print("RESULT ");
        host_number(value);
        host_print("\n");
    }
    if (ok)
        host_print("PASS load\n");
    w32_ldr_shutdown(&loader);
    if (host.peb.ProcessHeap) {
        RtlDestroyHeap(host.peb.ProcessHeap);
        host.peb.ProcessHeap = 0;
    }
    if (host.allocations || host.mappings) {
        host_print("LEAK\n");
        return 5;
    }
    return ok ? 0 : 1;
}
