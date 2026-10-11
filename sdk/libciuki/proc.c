/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/proc.h>
static int proc_result(uint32_t value)
{
    int error = ciuki_error(value);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)value;
}
int ciuki_proc_list(void *buffer, uint32_t bytes)
{ return proc_result(CU_CALL(PROC_LIST, CU_PTR(buffer), bytes, 0, 0, 0, 0)); }
int ciuki_proc_terminate(ciuki_pid_t pid, int signo, int authority_fd)
{
    uint32_t authority = authority_fd < 0 ? CIUKI_PROC_NO_AUTHORITY : (uint32_t)authority_fd;
    return proc_result(CU_CALL(PROC_TERMINATE, pid, signo, authority, 0, 0, 0));
}
