/* f5-20D w32-upper-dlls probe: one result block shared by the PE fixture
 * (writer), the kernel observer (reader, CIUKI_TEST framing) and the host
 * dry-run on the H1 adapter. The kernel reads the block from the fixture's
 * address space after validating the process identity; application stdout
 * is never evidence. Every string in the block is untrusted until checked.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_W32_UPPER_CONTROLLER_H
#define CIUKI_W32_UPPER_CONTROLLER_H
#include <stdint.h>
/* Fixed, 64 KiB-aligned application allocation, above the dialogs page.
 * The loader's top-down metadata owns 0x6fff0000 and can extend below it;
 * neither that address nor 0x6ffe0000 is a free result-page guarantee. */
#define W32_UPPER_RESULT_VA 0x10010000u
#define W32_UPPER_MAGIC 0x52505055u /* "UPPR" */
#define W32_UPPER_VERSION 1u
#define W32_UPPER_PATH "/System/Win32/Fixtures/upper-exercise.exe"
#define W32_UPPER_MAX 24u
/* first_error: 10 * (1 + table index) + kind. */
enum { W32_UPPER_ERR_LOAD = 1, W32_UPPER_ERR_BASE, W32_UPPER_ERR_ENTRY, W32_UPPER_ERR_FREE };
struct w32_upper_dll {
    char name[16];        /* lower-case builtin name, NUL-terminated */
    uint32_t base;        /* manifest base (config/wine-upper.json) */
    uint32_t loaded_base; /* HMODULE from LoadLibraryA, 0 when it failed */
    uint32_t load_error;  /* GetLastError after a failed LoadLibraryA */
    uint32_t entry;       /* 1: the census entry was found and behaved */
    uint32_t entry_error; /* 0, or the step of the failed check */
    uint32_t freed;       /* FreeLibrary returned TRUE */
    uint32_t resident;    /* still a module after FreeLibrary (builtins stay until process exit) */
};
struct w32_upper_result {
    uint32_t magic, version, done, release;
    uint32_t dlls, loaded, load_errors, entries, entry_errors, base_errors;
    uint32_t freed, free_errors, resident, errors, first_error;
    struct w32_upper_dll dll[W32_UPPER_MAX];
};
_Static_assert(sizeof(struct w32_upper_dll) == 44, "w32-upper-dlls slot");
_Static_assert(sizeof(struct w32_upper_result) == 60 + 44 * W32_UPPER_MAX, "w32-upper-dlls result block");
static inline int w32_upper_valid(const struct w32_upper_result *r)
{
    return r->magic == W32_UPPER_MAGIC && r->version == W32_UPPER_VERSION && r->done == 1 &&
           r->dlls >= 1 && r->dlls <= W32_UPPER_MAX;
}
/* A builtin name as the fixture copied it: 1..15 characters of [a-z0-9._-],
 * NUL-terminated inside the slot. Anything else prints as "invalid". */
static inline int w32_upper_name_ok(const char *name)
{
    unsigned n = 0;
    for (; n < 16 && name[n]; n++) {
        char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return n >= 1 && n < 16;
}
/* Every record stays well below the 240-byte CIUKI_TEST line limit. */
static inline void w32_upper_publish(const struct w32_upper_result *r, uint32_t exit_code,
    void (*emit)(const char *, const char *, const char *, ...))
{
    const char *p = "w32-upper-dlls";
    uint32_t count = r->dlls < W32_UPPER_MAX ? r->dlls : W32_UPPER_MAX;
    for (uint32_t i = 0; i < count; i++) {
        const struct w32_upper_dll *d = &r->dll[i];
        emit(p, "DATA", "case=upper_dll name=%s base=%08x loaded=%u load_error=%u entry=%u entry_error=%u freed=%u resident=%u",
             w32_upper_name_ok(d->name) ? d->name : "invalid", d->loaded_base, d->loaded_base ? 1u : 0u,
             d->load_error, d->entry ? 1u : 0u, d->entry_error, d->freed ? 1u : 0u, d->resident ? 1u : 0u);
    }
    emit(p, "DATA", "case=upper_dlls dlls=%u loaded=%u load_errors=%u entries=%u entry_errors=%u base_errors=%u errors=%u first_error=%u",
         r->dlls, r->loaded, r->load_errors, r->entries, r->entry_errors, r->base_errors, r->errors, r->first_error);
    emit(p, "DATA", "case=upper_teardown freed=%u free_errors=%u resident=%u exit=%u clean=%u",
         r->freed, r->free_errors, r->resident, exit_code,
         (!r->free_errors && !exit_code && r->freed == r->loaded) ? 1u : 0u);
}
#endif
