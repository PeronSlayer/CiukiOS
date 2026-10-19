/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_STORE_H
#define CIUKI_W32_STORE_H
#include "ldr.h"
/* Deterministic on-disk companion of builtin-store.json, little-endian.
 * Names are basenames; runtime paths always use C:\System\Win32. */
struct w32_store_row {
    char name[64], sha256[65];
    uint8_t reserved[3];
    uint32_t base, size;
    uint16_t version[4];
};
_Static_assert(sizeof(struct w32_store_row) == 148, "store row");
int w32_ciuki_store(struct w32_loader *, struct w32_file *);
NTSTATUS w32_ciuki_start(struct w32_loader *, struct w32_peb *, const char *, const char *,
                         const char *, const char *, struct w32_module **);
#endif
