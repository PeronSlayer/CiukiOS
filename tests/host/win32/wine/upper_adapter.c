/* SPDX-License-Identifier: LGPL-2.1-or-later
 * H0: production loader binding of the reused Wine upper DLLs against the
 * CiukiOS core builtins; DllMain is observed, never executed (no PE code). */
#include "linux.h"
#include "upper_store.h"

static struct w32_loader loader;

int host_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    host.observer = 1;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    loader.builtins = upper_store;
    loader.builtin_count = sizeof(upper_store) / sizeof(upper_store[0]);
    if (!w32_ldr_thread_attach(&loader, &host.teb))
        return 3;
    for (unsigned i = 0; i < sizeof(upper_names) / sizeof(upper_names[0]); i++) {
        struct w32_module *module;
        ULONG before = host.invocations;
        /* Diagnostics print during the load; the result line follows it. */
        int loaded = w32_ldr_load(&loader, upper_names[i], 0, &module);
        host_print("RESULT ");
        host_print(upper_names[i]);
        if (loaded) {
            host_print(" OK attach=");
            host_number(host.invocations - before);
            host_print("\n");
            if (!w32_ldr_free(&loader, module))
                return 4;
        } else {
            host_print(" FAIL ");
            host_number(loader.error.code);
            host_print(" ");
            host_print(loader.error.reason ? loader.error.reason : "-");
            host_print(" ");
            host_print(loader.error.module[0] ? loader.error.module : "-");
            host_print(" ");
            host_print(loader.error.symbol[0] ? loader.error.symbol : "-");
            host_print("\n");
        }
    }
    w32_ldr_shutdown(&loader);
    if (host.allocations || host.mappings)
        return 5;
    host_print("PASS upper H0 binding observation; resources released\n");
    return 0;
}
