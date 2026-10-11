/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/audio.h>
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size)
{
    uint32_t value = CU_CALL(DEVCTL, fd, op, CU_PTR(record), size, 0, 0);
    int error = ciuki_error(value);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)value;
}
