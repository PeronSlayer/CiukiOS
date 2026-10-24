/* F0 system calls, frozen in docs/design/execution-abi.md:
 * 0 exit, 1 yield, 2 debug_write, 3 probe_report, 4 sleep_ms, 5 probe_query.
 * Every buffer is validated completely before any visible side effect.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/devices.h>
#pragma weak devices_query
#include <ciuki/files.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/process.h>
#include <ciuki/supervisor.h>
#include <ciuki/dosvm.h>
#include <ciuki/display.h>
#include <ciuki/cpustat.h>
#pragma weak grants_display_owner
extern int32_t w32_syscall(struct trap_frame *) __attribute__((weak));
extern int32_t display_syscall(struct trap_frame *) __attribute__((weak));
extern int32_t power_syscall(uint32_t action) __attribute__((weak));
extern int32_t power_status_query(uint32_t address, uint32_t bytes) __attribute__((weak));
extern int32_t dosvm_client_syscall(struct trap_frame *) __attribute__((weak));

extern int copy_user(void *dst, const void *src, uint32_t len);
extern bool probe_f2_libc_report(struct task *, const char *, uint32_t) __attribute__((weak));

static int32_t sys_debug_write(uint32_t buf, uint32_t len)
{
    char kbuf[241];
    if (len > 240)
        return -EINVAL;
    if (!as_range_ok(&g_current->as, buf, len, false))
        return -EFAULT;
    if (copy_user(kbuf, (const void *)buf, len) < 0)
        return -EFAULT;
    kbuf[len] = 0;
    for (uint32_t i = 0; i < len; i++)
        if (kbuf[i] < 32 || kbuf[i] > 126)
            kbuf[i] = '.';
    g_current->debug_bytes += len;
    klog("[user %u] %s", g_current->id, kbuf);
    if (cpu_stall_record && len >= sizeof("[desktop] stall ")-1 &&
        !strncmp(kbuf, "[desktop] stall ", sizeof("[desktop] stall ")-1) &&
        grants_display_owner && grants_display_owner(proc_current()))
        cpu_stall_record(kbuf, len);
    return (int32_t)len;
}

static int32_t sys_probe_report(uint32_t buf, uint32_t len)
{
    char kbuf[241];
    if (len > 240)
        return -EINVAL;
    if (!as_range_ok(&g_current->as, buf, len, false))
        return -EFAULT;
    if (copy_user(kbuf, (const void *)buf, len) < 0)
        return -EFAULT;
    kbuf[len] = 0;
    /* Printable ASCII only: no CR/LF or control bytes can forge a record line. */
    if (len == 0)
        return -EINVAL;
    for (uint32_t i = 0; i < len; i++)
        if (kbuf[i] < 32 || kbuf[i] > 126)
            return -EINVAL;
    /* Desktop call-3 summaries are controller input, not application streams.
     * The hook consumes them by kernel task identity, including rejections;
     * libc-smoke/app-gate still use their existing untrusted framing below. */
    if (probe_f2_libc_report && probe_f2_libc_report(g_current, kbuf, len))
        return 0;
    if (!supervisor_report(g_current, kbuf, len))
        probe_user_report(g_current, kbuf, len);
    return 0;
}

static int32_t sys_probe_query(uint32_t buf, uint32_t cap)
{
    uint8_t out[4 + 8 + 4 + CIUKI_TEST_REQ_MAX];
    uint32_t reqlen = g_boot.test_request_len;
    uint32_t need = 4 + 8 + 4 + reqlen;
    if (cap > 4096)
        return -EINVAL;
    if (cap < need)
        return -ENOSPC;
    if (!as_range_ok(&g_current->as, buf, need, true))
        return -EFAULT;
    uint32_t id = g_current->id;
    uint64_t ticks = g_ticks;
    memcpy(out, &id, 4);
    memcpy(out + 4, &ticks, 8);
    memcpy(out + 12, &reqlen, 4);
    memcpy(out + 16, g_boot.test_request, reqlen);
    if (copy_user((void *)buf, out, need) < 0)
        return -EFAULT;
    return (int32_t)need;
}

static void sys_log(uint32_t nr, int32_t r, uint32_t side)
{
    struct task *t = g_current;
    if (t->sys_log_n < 16) {
        t->sys_log[t->sys_log_n].nr = nr;
        t->sys_log[t->sys_log_n].result = r;
        t->sys_log[t->sys_log_n].side_effects = side;
        t->sys_log_n++;
    }
}

int32_t net_socket_call(struct trap_frame *) __attribute__((weak));
int32_t entropy_syscall(struct trap_frame *) __attribute__((weak));
int32_t time_syscall(struct trap_frame *) __attribute__((weak)); /* G1 */

void syscall_dispatch(struct trap_frame *tf)
{
    int32_t r;
    uint32_t nr = tf->eax;
    switch (tf->eax) {
    case 0:
        task_exit((int)tf->ebx);
    case 1:
        task_yield();
        r = 0;
        break;
    case 2:
        r = sys_debug_write(tf->ebx, tf->ecx);
        break;
    case 3:
        r = sys_probe_report(tf->ebx, tf->ecx);
        break;
    case 4:
        if (tf->ebx > 60000) {
            r = -EINVAL;
        } else {
            task_sleep_ms(tf->ebx);
            r = 0;
        }
        break;
    case 5:
        r = sys_probe_query(tf->ebx, tf->ecx);
        break;
    case CIUKI_SYS_SOCKET:
    case CIUKI_SYS_BIND:
    case CIUKI_SYS_CONNECT:
    case CIUKI_SYS_LISTEN:
    case CIUKI_SYS_ACCEPT:
    case CIUKI_SYS_SENDTO:
    case CIUKI_SYS_RECVFROM:
    case CIUKI_SYS_SHUTDOWN:
    case CIUKI_SYS_SOCKNAME:
    case CIUKI_SYS_SOCKOPT:
    case CIUKI_SYS_POLL:
    case CIUKI_SYS_RESOLVE4:
    case CIUKI_SYS_NETCTL:
        r = net_socket_call ? net_socket_call(tf) : -ENOSYS;
        break;
    case CIUKI_SYS_GETENTROPY:
        r = entropy_syscall ? entropy_syscall(tf) : -ENOSYS;
        break;
    case CIUKI_SYS_TIME:
        r = time_syscall ? time_syscall(tf) : -ENOSYS;
        break;
    case CIUKI_SYS_W32:
        if (w32_syscall) { w32_syscall(tf); return; }
        r = -ENOSYS;
        break;
    case CIUKI_SYS_DOSVM:
        /* Weak like dosvm_trap: host harnesses link syscall.c without the VM. */
        r = dosvm_client_syscall ? dosvm_client_syscall(tf) : -ENOSYS;
        break;
    case CIUKI_SYS_DEVICES:
        r = devices_query ? devices_query(tf->ebx, tf->ecx) : -ENOSYS;
        break;
    case CIUKI_SYS_DISPLAY:
        r = display_syscall ? display_syscall(tf) : -ENOSYS;
        break;
    case CIUKI_SYS_POWER:
        /* Weak: host harnesses link syscall.c without core/power.c. */
        r = tf->ebx == CIUKI_POWER_STATUS ? (power_status_query ? power_status_query(tf->ecx, tf->edx) : -ENOSYS) :
            (power_syscall ? power_syscall(tf->ebx) : -ENOSYS);
        break;
    case CIUKI_SYS_UNAME:
        r = file_syscall(tf) ? (int32_t)tf->eax : -ENOSYS;
        break;
    case CIUKI_SYS_SURFACE_CREATE: case CIUKI_SYS_SURFACE_MAP:
    case CIUKI_SYS_SURFACE_INFO: case CIUKI_SYS_PRESENT: case CIUKI_SYS_INPUT_READ:
    case CIUKI_SYS_CHANNEL_PAIR: case CIUKI_SYS_CHANNEL_SEND: case CIUKI_SYS_CHANNEL_RECV:
    case CIUKI_SYS_DISPLAY_INFO: case CIUKI_SYS_DISPLAY_ACCEL:
        r = desktop_syscall(tf);
        break;
    default:
        r = file_syscall(tf) ? (int32_t)tf->eax : desktop_fd_syscall(tf) ? (int32_t)tf->eax : proc_syscall(tf);
        break;
    }
    if (nr != 1 && nr != 4) {
        /* side effects: bytes emitted (2), reports accepted (3), bytes written (5) */
        uint32_t side = r > 0 ? (uint32_t)r : (nr == 3 && r == 0 ? 1u : 0u);
        sys_log(nr, r, side);
    }
    tf->eax = (uint32_t)r;
}
