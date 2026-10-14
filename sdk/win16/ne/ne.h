/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_WIN16_NE_H
#define CIUKI_WIN16_NE_H

#include <stdint.h>

/* Parser work limits, not runtime task/memory quotas. Storage is caller-owned. */
#define NE_MAX_SEGMENTS 254u
#define NE_MAX_ENTRIES 1024u
#define NE_MAX_NAMES 512u
#define NE_MAX_MODULES 256u
#define NE_MAX_RESOURCES 256u
#define NE_MAX_RELOCS 8192u
#define NE_SEGMENT_BYTES 65536u

#define NE_SEG_DATA 0x0001u
#define NE_SEG_ITERATED 0x0008u
#define NE_SEG_MOVABLE 0x0010u
#define NE_SEG_SHAREABLE 0x0020u
#define NE_SEG_PRELOAD 0x0040u
#define NE_SEG_READONLY 0x0080u /* Execute-only for code. */
#define NE_SEG_RELOC 0x0100u
#define NE_SEG_LOADER_BITS 0x0006u  /* Loader-owned runtime state; linkers write 0. */
#define NE_SEG_CONFORM_DEBUG 0x0200u /* Appendix K: debug info; OS/2 newexe: conforming. Recorded only. */
#define NE_SEG_DPL 0x0C00u          /* Protected-mode DPL; recorded, never used to pick a ring. */
#define NE_SEG_DISCARDABLE 0x1000u
#define NE_SEG_FLAG_32BIT 0x2000u        /* Wine/Open Watcom; Appendix K calls F000 discard priority. */
#define NE_MODULE_DATA_MASK 0x0003u
#define NE_MODULE_SELFLOAD 0x0800u
#define NE_MODULE_LINK_ERROR 0x2000u
#define NE_MODULE_LIBRARY 0x8000u
#define NE_TGT_OS2 1u
#define NE_TGT_WINDOWS 2u
#define NE_TGT_WIN386 4u
#define NE_ALIGN_DEFAULT 9u         /* Appendix K: a stored shift of 0 is equivalent to 9. */
#define NE_SELFLOAD_MAGIC 0x3041u /* "A0" */
#define NE_SELFLOAD_BYTES 40u      /* Bytes of the first-segment self-loader table. */

#define NE_RULES(X) \
 X(OK, "ok") \
 X(ARGUMENT, "invalid-argument") \
 X(DOS_HEADER, "dos-header-truncated") \
 X(DOS_MAGIC, "dos-magic") \
 X(HEADER_BOUNDS, "ne-header-bounds") \
 X(SIGNATURE, "ne-signature") \
 X(IMAGE_PE, "image-is-pe") \
 X(IMAGE_LX, "image-is-le-lx") \
 X(TARGET_OS2, "target-os2") \
 X(TARGET, "target-not-windows") \
 X(LINK_ERRORS, "module-link-errors") \
 X(DATA_MODEL, "module-data-model-invalid") \
 X(SELFLOAD, "self-loading-unsupported") \
 X(SELFLOAD_HEADER, "self-loader-header-bounds") \
 X(SELFLOAD_VERSION, "self-loader-version") \
 X(LIMIT, "parser-work-limit") \
 X(SHIFT, "alignment-shift") \
 X(ARITHMETIC, "sector-offset-overflow") \
 X(TABLE_BOUNDS, "table-bounds") \
 X(TABLE_ORDER, "table-order") \
 X(OVERLAP, "table-or-payload-overlap") \
 X(SEG_FLAGS, "segment-loader-bits") \
 X(SEG_32BIT, "segment-32bit-unsupported") \
 X(SEG_BOUNDS, "segment-file-bounds") \
 X(SEG_ZERO, "zero-sector-relocations-or-iteration") \
 X(ITERATED, "iterated-record-bounds") \
 X(EXPANSION, "iterated-expansion-limit") \
 X(ENTRY_BOUNDS, "entry-bundle-bounds") \
 X(ENTRY_TERMINATOR, "entry-table-terminator") \
 X(ENTRY_SEGMENT, "entry-segment-index") \
 X(ENTRY_OFFSET, "entry-offset-bounds") \
 X(ENTRY_INT3F, "movable-entry-int3f") \
 X(NAME_BOUNDS, "name-record-bounds") \
 X(NAME_TERMINATOR, "name-table-terminator") \
 X(NAME_ORDINAL, "name-entry-ordinal") \
 X(NAME_DUPLICATE, "duplicate-export-name") \
 X(NAME_ZERO, "ordinal-zero-name-position") \
 X(IMPORT_NAME, "import-name-bounds") \
 X(MODULE_INDEX, "import-module-index") \
 X(RESOURCE_BOUNDS, "resource-table-bounds") \
 X(RESOURCE_NAME, "resource-name-bounds") \
 X(RESOURCE_DUPLICATE, "duplicate-resource-id") \
 X(RESOURCE_DATA, "resource-file-bounds") \
 X(STARTUP, "startup-segment-or-offset") \
 X(AUTODATA, "autodata-size-overflow") \
 X(RELOC_BOUNDS, "relocation-table-bounds") \
 X(RELOC_TYPE, "relocation-source-type") \
 X(RELOC_FLAGS, "relocation-flags") \
 X(RELOC_TARGET, "relocation-target") \
 X(PATCH_BOUNDS, "relocation-patch-bounds") \
 X(CHAIN_CYCLE, "relocation-chain-cycle-or-overlap") \
 X(OSFIXUP, "osfixup-policy-unsupported") \
 X(IMPORT_MISSING, "import-unresolved") \
 X(STORE, "segment-store-bounds-or-selector") \
 X(ADDITIVE_SELECTOR, "additive-selector-nonzero")

enum ne_rule {
#define NE_ENUM(id, name) NE_##id,
    NE_RULES(NE_ENUM)
#undef NE_ENUM
};
struct ne_error { uint32_t rule, file_offset, segment, record; };
struct ne_string { uint32_t offset, length; }; /* Borrowed file span; no NUL. */
struct ne_segment {
    uint32_t file_offset, stored_size, initialized_size, allocation;
    uint32_t reloc_first, reloc_count;
    uint16_t sector, flags;
};
enum ne_entry_kind { NE_UNUSED, NE_FIXED, NE_MOVABLE, NE_CONSTANT };
struct ne_entry { uint16_t kind, flags, segment, offset; };
struct ne_name { struct ne_string text; uint16_t ordinal, nonresident; };
struct ne_id { uint32_t integer; struct ne_string text; }; /* integer=0 => string. */
struct ne_resource {
    struct ne_id type, name;
    uint32_t file_offset, size;
    uint16_t flags, reserved;
};
struct ne_reloc {
    uint16_t segment, source, target1, target2;
    uint8_t source_type, flags;
    uint16_t reserved;
};
struct ne_image {
    const uint8_t *bytes; /* Must stay immutable/live while the plan is used. */
    uint32_t size, ne_offset, valid;
    uint16_t flags, autodata, heap, stack, ip, cs, sp, ss;
    uint16_t alignment, resource_alignment, expected_version, target;
    uint16_t linker_version, other_flags, alignment_stored, selfload_version;
    uint32_t autodata_bytes; /* DGROUP allocation + heap (+ stack when SP=0). */
    uint32_t selfload_boot, selfload_loadseg, selfload_exit; /* Raw, unrelocated far words. */
    uint32_t selfload_state; /* 0 not self-loading, 1 header read, 2 header outside segment 1. */
    uint32_t segment_count, entry_count, name_count, module_count;
    uint32_t resource_count, reloc_count;
    struct ne_segment segments[NE_MAX_SEGMENTS];
    struct ne_entry entries[NE_MAX_ENTRIES]; /* Index = ordinal minus one. */
    struct ne_name names[NE_MAX_NAMES];
    struct ne_string modules[NE_MAX_MODULES];
    struct ne_resource resources[NE_MAX_RESOURCES];
    struct ne_reloc relocs[NE_MAX_RELOCS];
};
struct ne_buffer { uint8_t *bytes; uint32_t size; uint16_t selector; };
struct ne_address { uint32_t offset; uint16_t selector; };
/* Resolver sees the original numeric ordinal OR exact case-sensitive name.
 * Module identity lookup is case-insensitive, performed by the caller.
 * Return zero on success; a missing import never resolves to a dummy pointer. */
typedef int (*ne_resolver)(void *, const struct ne_image *, uint16_t module,
                          uint16_t ordinal, struct ne_string name,
                          struct ne_address *);
struct ne_workspace { uint8_t bytes[NE_SEGMENT_BYTES], seen[NE_SEGMENT_BYTES / 8u]; };

const char *ne_rule_name(uint32_t rule);
uint32_t ne_image_bytes(void); /* Layout guard for host bindings. */
int ne_parse(const uint8_t *bytes, uint32_t size, struct ne_image *, struct ne_error *);
/* Parse success is structural only. Self-load is deliberately refused here. */
int ne_load_policy(const struct ne_image *, struct ne_error *);
int ne_load_segment(const struct ne_image *, uint16_t segment, struct ne_buffer,
                    struct ne_error *);
/* Atomic for one segment. On any failure, no destination byte is changed.
 * Workspace, plan/input and destination stores must not alias. Selectors and
 * resolver results are mock identities, never installed in an LDT by this API. */
int ne_relocate_segment(const struct ne_image *, uint16_t segment,
                       const struct ne_buffer *store, uint32_t store_count,
                       ne_resolver, void *, struct ne_workspace *, struct ne_error *);
#endif
