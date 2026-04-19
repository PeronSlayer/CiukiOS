#ifndef STAGE2_APP_CATALOG_H
#define STAGE2_APP_CATALOG_H

/*
 * OPENGEM-004 — App Discovery and File Catalog.
 *
 * Joins two discovery lanes into a single, de-duplicated list:
 *   1. FAT scan across a small set of well-known roots for 8.3
 *      names ending in .COM / .EXE / .BAT.
 *   2. The loader-provided COM catalog (`handoff->com_entries`),
 *      which is authoritative for demo COMs shipped in the image
 *      via the loader.
 *
 * Consumers (shell `catalog` command, future GEMVDI host-app,
 * future PATH resolver) iterate with `app_catalog_count()` +
 * `app_catalog_get(i)`.
 *
 * Design constraints:
 *   - Static backing storage only (no dynamic allocation).
 *   - Append-only struct shape: new fields only at the tail of
 *     `app_catalog_entry_t`, never reorder existing ones.
 *   - De-dupe tie-break: FAT wins over the handoff COM catalog so
 *     a user can override bundled demos by dropping a COM on the
 *     image.
 */

#include "types.h"
#include "handoff.h"

#define APP_CATALOG_MAX_ENTRIES 256U
#define APP_CATALOG_MAX_ROOTS   4U

/* Entry kinds (append-only values; do not renumber). */
#define APP_CATALOG_KIND_UNKNOWN 0U
#define APP_CATALOG_KIND_COM     1U
#define APP_CATALOG_KIND_EXE     2U
#define APP_CATALOG_KIND_BAT     3U

/* Source lane (for diagnostics; append-only). */
#define APP_CATALOG_SRC_UNKNOWN 0U
#define APP_CATALOG_SRC_FAT     1U
#define APP_CATALOG_SRC_HANDOFF 2U

typedef struct app_catalog_entry {
    char name[13];    /* 8.3 uppercase name, NUL-terminated */
    char path[64];    /* canonical path (`/FREEDOS/FOO.COM`) or
                       * `(handoff)/NAME` for handoff-only entries */
    u8   kind;        /* APP_CATALOG_KIND_* */
    u8   source;      /* APP_CATALOG_SRC_* */
    u8   reserved[2]; /* future fields (size_hint, etc.) */
} app_catalog_entry_t;

int                         app_catalog_init(handoff_v0_t *handoff);
u32                         app_catalog_count(void);
const app_catalog_entry_t  *app_catalog_get(u32 index);
const char                 *app_catalog_kind_label(u8 kind);

/* Case-insensitive lookup by 8.3 name. Returns NULL when absent. */
const app_catalog_entry_t  *app_catalog_find(const char *name);

#endif /* STAGE2_APP_CATALOG_H */
