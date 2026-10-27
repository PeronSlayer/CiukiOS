/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/proc.h>
#include <ciuki/security.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
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
int ciuki_launch_mark(uint32_t event, ciuki_pid_t pid, uint64_t click_us)
{ return proc_result(CU_CALL(LAUNCH_MARK, event, pid, (uint32_t)click_us, (uint32_t)(click_us >> 32), 0, 0)); }

int ciuki_security(struct ciuki_security_args *args)
{ return proc_result(CU_CALL(SECURITY, CU_PTR(args), sizeof(*args), 0, 0, 0, 0)); }
static int security_query(uint32_t op, uint32_t pid, int fd, struct ciuki_credential *out)
{
    struct ciuki_security_args a = { .version = 1, .size = 64, .operation = op,
        .target_pid = pid, .endpoint_fd = fd, .output_address = CU_PTR(out), .output_bytes = 64 };
    return ciuki_security(&a);
}
int ciuki_security_self(struct ciuki_credential *out)
{ return security_query(CIUKI_SECURITY_QUERY_SELF, 0, 0, out); }
int ciuki_security_process(uint32_t pid, struct ciuki_credential *out)
{ return security_query(CIUKI_SECURITY_QUERY_PROCESS, pid, 0, out); }
int ciuki_security_peer(int fd, struct ciuki_credential *out)
{ return security_query(CIUKI_SECURITY_QUERY_PEER, 0, fd, out); }
int ciuki_security_session(uint32_t session, uint32_t *state)
{
    if (!state) { errno = EFAULT; return -1; }
    struct ciuki_security_args a = { .version = 1, .size = 64,
        .operation = CIUKI_SECURITY_SESSION_QUERY, .session_id = session };
    int result = ciuki_security(&a);
    if (!result) *state = a.state;
    return result;
}
uid_t getuid(void)
{
    struct ciuki_credential c;
    return ciuki_security_self(&c) ? CIUKI_UID_INVALID : c.uid;
}
uid_t geteuid(void) { return getuid(); }
gid_t getgid(void)
{
    struct ciuki_credential c;
    return ciuki_security_self(&c) ? CIUKI_GID_INVALID : c.primary_gid;
}
gid_t getegid(void) { return getgid(); }
int getgroups(int count, gid_t groups[])
{
    if (count < 0) { errno = EINVAL; return -1; }
    struct ciuki_credential c;
    if (ciuki_security_self(&c)) return -1;
    if (!count) return (int)c.group_count;
    if ((unsigned)count < c.group_count) { errno = EINVAL; return -1; }
    if (!groups) { errno = EFAULT; return -1; }
    memcpy(groups, c.groups, c.group_count * sizeof(*groups));
    return (int)c.group_count;
}

static int identity_denied(void) { errno=EPERM;return -1; }
int setuid(uid_t u) { (void)u;return identity_denied(); }
int seteuid(uid_t u) { (void)u;return identity_denied(); }
int setgid(gid_t g) { (void)g;return identity_denied(); }
int setegid(gid_t g) { (void)g;return identity_denied(); }
int setreuid(uid_t r,uid_t e) { (void)r;(void)e;return identity_denied(); }
int setregid(gid_t r,gid_t e) { (void)r;(void)e;return identity_denied(); }
