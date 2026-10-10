/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PATH_H
#define CIUKI_PATH_H
#include "fs_port.h"
#define FS_PATH_CHARS 259u
#define FS_NAME_CHARS 255u
#define FS_PATH_BYTES (FS_PATH_CHARS*3u+1u)
#define FS_NAME_BYTES (FS_NAME_CHARS*3u+1u)
int path_decode(const char **s, uint16_t *ch);
int path_to_ucs2(const char *, uint16_t *, unsigned capacity, unsigned *length);
int path_from_ucs2(const uint16_t *, unsigned length, char *, unsigned capacity);
uint16_t path_upper(uint16_t);
bool path_equal(const char *, const char *);
bool path_wildcard(const char *pattern, const char *name);
int path_validate_name(const char *, uint16_t out[256], unsigned *length);
int path_cp437(uint16_t); /* -FS_EILSEQ if not representable */
uint16_t path_from_cp437(uint8_t);
/* canonical absolute within-volume path, with / separators and no . or .. */
int path_normalize(const char *input, unsigned current_drive, const char *cwd,
                   unsigned *drive, char out[FS_PATH_BYTES]);
#endif
