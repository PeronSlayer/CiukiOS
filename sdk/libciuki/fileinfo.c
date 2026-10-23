/* G6 native file attributes and times (call 98).
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/fileinfo.h>
#include <errno.h>
#include <string.h>
int ciuki_file_info_get(const char *path, struct ciuki_file_info *out)
{
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_FILE_INFO_VERSION; out->size = sizeof(*out); out->operation = CIUKI_FILE_INFO_GET;
    int e = ciuki_error(ciuki_syscall(CIUKI_SYS_FILE_INFO, CU_PTR(path), CU_PTR(out), sizeof(*out), 0, 0, 0));
    if (e) { errno = e; return -1; }
    return 0;
}
int ciuki_file_info_set(const char *path, uint32_t mask, const struct ciuki_file_info *in)
{
    struct ciuki_file_info record = *in;
    record.version = CIUKI_FILE_INFO_VERSION; record.size = sizeof(record);
    record.operation = CIUKI_FILE_INFO_SET; record.mask = mask; record.reserved = 0;
    int e = ciuki_error(ciuki_syscall(CIUKI_SYS_FILE_INFO, CU_PTR(path), CU_PTR(&record), sizeof(record), 0, 0, 0));
    if (e) { errno = e; return -1; }
    return 0;
}
int ciuki_set_attributes(const char *path, uint32_t attributes)
{
    struct ciuki_file_info record = {.attributes = attributes & CIUKI_ATTR_SETTABLE};
    return ciuki_file_info_set(path, CIUKI_FILE_INFO_ATTRIBUTES, &record);
}
