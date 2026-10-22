/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_NTIMAGE_H
#define CIUKI_NDIS_NTIMAGE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../../sdk/pe/pe.h"
#if defined(__i386__)
#define NDIS_STDCALL __attribute__((stdcall))
#define NDIS_FASTCALL __attribute__((fastcall))
#define NDIS_CDECL __attribute__((cdecl))
#else
/* Native-width builds are structural models, never x86 ABI evidence. */
#define NDIS_STDCALL
#define NDIS_FASTCALL
#define NDIS_CDECL
#endif
#define NDIS_NO_ORDINAL 0u
#define NDIS_IMAGE_LIMIT (4u * 1024u * 1024u)
enum ndis_abi { NDIS_ABI_STDCALL, NDIS_ABI_FASTCALL, NDIS_ABI_CDECL, NDIS_ABI_DATA };
enum ndis_export_kind { NDIS_FUNCTION, NDIS_DATA };
struct ndis_export {
    const char *module, *name;
    uint16_t ordinal, stack_bytes;
    enum ndis_abi abi;
    enum ndis_export_kind kind;
    uint32_t address;
    bool implemented;
};
const struct ndis_export *ndis_exports(size_t *);
/* Ordinal 0 means no reviewed NT5 ordinal mapping, never an inferred index. */
const struct ndis_export *ndis_export_find(const struct ndis_export *, size_t,
                                         const char *, const char *, uint16_t);
uint32_t ndis_export_address(const struct ndis_export *, const char **reason);
/* Counted UTF-16 view; no native-width UNICODE_STRING crosses the PE ABI. */
uint32_t ndis_routine_address(const struct ndis_export *, size_t, const uint16_t *, uint16_t bytes,
                             bool ndis_module);
uint32_t ndis_mm_get_system_routine_address(const uint16_t *, uint16_t);
uint32_t ndis_get_routine_address(const uint16_t *, uint16_t);
struct ntimage_platform {
    struct pe_ops memory;
    /* Prove private pinned pages at the actual execution VA: supervisor for
     * the old policy, owned user mappings for user_host. Staging may differ.
     * This is not a statement that the driver has DMA-safe access to it. */
    int (*resident)(void *, void *, uint32_t, uint32_t);
    void (*record)(void *, const char *file, const char *module, const char *symbol,
                   uint16_t ordinal, const char *reason);
    const struct ndis_export *exports;
    size_t export_count;
    uint32_t image_budget; /* already reserved from the common 4 MiB F6 budget */
    void (*scan_record)(void *, const char *file, uint32_t rva, const char *reason);
    /* ND14: the returned pointer is a supervisor staging view; base is the
     * distinct process execution VA. resident proves owned user mappings. */
    bool user_host;
    int (*user_import)(void *, const struct ndis_export *, uint32_t *address);
};
struct ntimage {
    struct pe_image pe;
    struct pe_error error;
    struct pe_ops ops;
    struct ntimage_platform platform;
    const char *file; /* pinned caller-owned name */
    bool loaded, entered, quarantined, scanned;
    uint32_t scan_rva;
};
int ntimage_load(struct ntimage *, const void *, uint32_t, const char *, const struct ntimage_platform *);
int ntimage_unload(struct ntimage *);
bool ntimage_executable(const struct ntimage *, uint32_t address);
/* Production gate is deliberately closed until ND12 independent exception,
 * nonreturn watchdog and per-profile fence evidence are cross-reviewed. */
bool ndis_execution_available(void);
int ntimage_enter(struct ntimage *, uint32_t driver_object, uint32_t registry_path);
#endif
