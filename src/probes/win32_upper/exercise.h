/* f5-20D w32-upper-dlls fixture: the module services the guest entry (the
 * imported KERNEL32) and the H1 adapter dry-run (the PE KERNEL32 loaded by
 * the production loader) supply. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_UPPER_EXERCISE_H
#define CIUKI_W32_UPPER_EXERCISE_H
#include <w32/kernel32.h>
#include "controller.h"
struct w32_upper_entry {
    const char *name; /* lower-case builtin name */
    uint32_t base;    /* manifest base (config/wine-upper.json) */
};
struct w32_upper_env {
    void *(W32API *load)(const char *);          /* LoadLibraryA */
    void *(W32API *proc)(void *, const char *);  /* GetProcAddress */
    BOOL (W32API *free)(void *);                 /* FreeLibrary */
    void *(W32API *handle)(const char *);        /* GetModuleHandleA */
    DWORD (W32API *last_error)(void);            /* GetLastError */
    void *(W32API *local_free)(void *);          /* LocalFree (CommandLineToArgvW) */
};
void w32_upper_run(const struct w32_upper_env *, const struct w32_upper_entry *, uint32_t,
                   struct w32_upper_result *);
#endif
