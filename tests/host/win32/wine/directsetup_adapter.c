/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Production loader on H0/H1; PE execution is conditional on H1. */
#include "linux.h"
#include "directsetup_store.h"

static struct w32_loader loader;

int host_main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    int observe = !w32_strcmp(argv[1], "observe");
    host.observer = observe;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    loader.builtins = directsetup_store;
    loader.builtin_count = 2;
    if (!w32_ldr_thread_attach(&loader, &host.teb))
        return 3;
    ULONG before = host.invocations;
    for (unsigned i = 0; i < 2; i++) {
        struct w32_module *module;
        if (!w32_ldr_load(&loader, directsetup_store[i].name, 0, &module))
            return 4;
        void *version = w32_ldr_proc(&loader, module, "DirectXSetupGetVersion", 0, 0xffffffffu);
        void *ansi = w32_ldr_proc(&loader, module, "DirectXSetupA", 0, 0xffffffffu);
        void *wide = w32_ldr_proc(&loader, module, "DirectXSetupW", 0, 0xffffffffu);
        void *alias = w32_ldr_proc(&loader, module, "DirectXSetup", 0, 0xffffffffu);
        if (!version || !ansi || !wide || ansi != alias)
            return 5;
#ifndef W32_HOST_MODEL
        if (!observe) {
            ULONG v = 0, r = 0;
            LONG(W32API *query)(ULONG *, ULONG *) = version;
            LONG(W32API *setup)(void *, const void *, ULONG) = ansi;
            if (query(&v, &r) != 1 || v != 0x00040007u || r != 716 || query(0, 0) != 1)
                return 6;
            if (setup(0, 0, 0x10018u) != 0)
                return 7;
            setup = wide;
            if (setup(0, 0, 0x10018u) != 0)
                return 8;
        }
#endif
    }
    w32_ldr_shutdown(&loader);
    if (host.invocations - before != 4 || host.allocations || host.mappings)
        return 9;
    host_print(observe ? "PASS DirectSetup H0 binding attach/detach observation; no PE execution\n"
                       : "PASS DirectSetup H1 PE calls attach/detach\n");
    return 0;
}
