/* SPDX-License-Identifier: MIT */
#include <ciuki/dosvm.h>
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
int ciuki_dosvm(uint32_t operation, struct ciuki_dosvm_command *command)
{
    uint32_t result = ciuki_raw_dosvm(operation, CU_PTR(command), 0, 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)result;
}
