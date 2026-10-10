/* F0 system calls, frozen in docs/design/execution-abi.md:
 * 0 exit, 1 yield, 2 debug_write, 3 probe_report, 4 sleep_ms, 5 probe_query.
 * Every buffer is validated completely before any visible side effect.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/process.h>

extern int copy_user(void *dst, const void *src, uint32_t len);

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
    default:
        r = proc_syscall(tf);
        break;
    }
    if (nr != 1 && nr != 4) {
        /* side effects: bytes emitted (2), reports accepted (3), bytes written (5) */
        uint32_t side = r > 0 ? (uint32_t)r : (nr == 3 && r == 0 ? 1u : 0u);
        sys_log(nr, r, side);
    }
    tf->eax = (uint32_t)r;
}
