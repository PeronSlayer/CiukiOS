/* SPDX-License-Identifier: MIT
 * Target binding of kernel_sys.h over the static Ciuki SDK (libciuki). */
#include "kernel_sys.h"
#include <ciuki/runtime.h>
#include <ciuki/security.h>
#include <ciuki/channel.h>
#include <ciuki/proc.h>
#include <ciuki/recovery.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int acct_sys_security(struct ciuki_security_args *a, const void *input, uint32_t input_bytes,
                      void *output, uint32_t output_bytes)
{
    if (!a) return -EINVAL;
    uint32_t saved_in=a->spawn_address, saved_out=a->output_address, saved_bytes=a->output_bytes;
    if (input) a->spawn_address=CU_PTR(input);
    (void)input_bytes;
    if (output) { a->output_address=CU_PTR(output); a->output_bytes=output_bytes; }
    uint32_t r=CU_CALL(SECURITY,CU_PTR(a),sizeof(*a),0,0,0,0);
    int e=ciuki_error(r);
    /* Addresses are transport detail; the adapter keeps record fields only. */
    a->spawn_address=saved_in; a->output_address=saved_out; a->output_bytes=saved_bytes;
    return e ? -e : 0;
}
int acct_sys_channel_send(int fd, const struct ciuki_message *m)
{
    uint32_t r=CU_CALL(CHANNEL_SEND,(uint32_t)fd,CU_PTR(m),0,0,0,0);
    int e=ciuki_error(r); return e ? -e : 0;
}
int acct_sys_channel_recv(int fd, struct ciuki_message *m, int wait)
{
    uint32_t r=CU_CALL(CHANNEL_RECV,(uint32_t)fd,CU_PTR(m),wait ? 0u : (uint32_t)DONTWAIT,0,0,0);
    int e=ciuki_error(r); return e ? -e : (int)r;
}
int acct_sys_close(int fd)
{
    uint32_t r=CU_CALL(CLOSE,(uint32_t)fd,0,0,0,0,0);
    int e=ciuki_error(r); return e ? -e : 0;
}
int acct_sys_wait_readable(const int *fds, unsigned count, unsigned timeout_ms)
{
    if (!count) {
        struct timespec t; memset(&t,0,sizeof(t));
        t.tv_sec=(time_t)(timeout_ms/1000u); t.tv_nsec=(long)(timeout_ms%1000u)*1000000L;
        return nanosleep(&t,NULL) ? -errno : 0;
    }
    struct pollfd set[64]; if (count>64) count=64;
    for (unsigned i=0;i<count;i++) set[i]=(struct pollfd){ .fd=fds[i], .events=POLLIN };
    int r=poll(set,count,(int)timeout_ms);
    return r<0 ? -errno : r;
}
uint32_t acct_sys_pid(void) { return (uint32_t)getpid(); }
static struct { struct ciuki_proc_system system; struct ciuki_proc_record records[CIUKI_PROCESS_MAX]; } snapshot;
int acct_sys_pids(uint32_t *pids, unsigned capacity)
{
    int n=ciuki_proc_list(&snapshot,sizeof(snapshot));
    if (n<0) return -errno;
    unsigned count=0;
    for (int i=0;i<n && count<capacity;i++)
        if (snapshot.records[i].state==CIUKI_PROC_LIVE) pids[count++]=snapshot.records[i].pid;
    memset(&snapshot,0,sizeof(snapshot));
    return (int)count;
}
uint64_t acct_sys_monotonic_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
int acct_sys_entropy(void *buffer, size_t bytes)
{
    if (bytes>256) return -EINVAL;
    uint32_t r=CU_CALL(GETENTROPY,CU_PTR(buffer),(uint32_t)bytes,0,0,0,0);
    int e=ciuki_error(r); return e ? -e : 0;
}
int acct_sys_boot_flags(uint32_t *flags)
{
    struct ciuki_recovery_status s;
    if (ciuki_recovery_status(&s)) return -errno;
    *flags=s.boot_flags; return 0;
}
int acct_sys_file_present(const char *path)
{
    struct stat st;
    if (stat(path,&st)) return errno==ENOENT || errno==ENOTDIR ? 0 : -errno;
    return S_ISREG(st.st_mode) ? 1 : 0;
}
void acct_sys_log(const char *line)
{ CU_CALL(DEBUG_WRITE,CU_PTR(line),(uint32_t)strlen(line),0,0,0,0); }
