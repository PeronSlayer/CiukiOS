/* f3-02 DOS window VM client call; the kernel checks the delegated authority.
 * SPDX-License-Identifier: MIT */
#include <ciuki/dosvm.h>
#include <ciuki/runtime.h>
int ciuki_dosvm(uint32_t operation, struct ciuki_dosvm_command *command)
{
    uint32_t result = CU_CALL(DOSVM, operation, CU_PTR(command),
                              sizeof(struct ciuki_dosvm_command), 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)result;
}
