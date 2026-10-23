/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_FILEINFO_H
#define CIUKI_SDK_FILEINFO_H
#include <ciuki/abi.h>
/* G6: FAT attributes, created/accessed/modified times, size on disk and the
 * MS-DOS (8.3) name of a path; call 98, record in ciuki/abi.h. Both return
 * 0, or -1 with errno (EROFS on read-only media, EOPNOTSUPP for devices).
 * ciuki_file_info_get initializes version, size and operation itself.
 * ciuki_file_info_set changes only the fields named in mask
 * (CIUKI_FILE_INFO_ATTRIBUTES/CREATED/ACCESSED/MODIFIED). */
int ciuki_file_info_get(const char *path, struct ciuki_file_info *out);
int ciuki_file_info_set(const char *path, uint32_t mask, const struct ciuki_file_info *in);
/* Convenience: replace the four settable FAT attribute bits. */
int ciuki_set_attributes(const char *path, uint32_t attributes);
#endif
