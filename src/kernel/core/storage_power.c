/* Bounded suspend-flush mailbox. The worker owns all transport lifetimes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/storage.h>
#include <ciuki/task.h>

static struct kwait flush_wake;
static struct task *flush_worker;
static bool flush_pending, flush_done, flush_observed;
static uint64_t flush_deadline;
static int flush_result;
static struct vfs_request flush_request;

static uint64_t flush_now(void *ctx)
{
    (void)ctx; return deadline_after_ms(0) * 1000;
}
static void flush_pause(void *ctx) { (void)ctx; task_sleep_ms(1); }
static bool flush_ready(void *ctx) { (void)ctx; return flush_pending; }
static bool flush_run_once(void)
{
    if (!flush_pending) return false;
    /* Mailbox/context are static, not a suspended caller's stack. A
     * timed-out caller may acknowledge while retirement remains pinned. */
    int e = storage_sync_until(storage_get(), &flush_request);
    uint32_t f = irq_save();
    flush_result = e; flush_pending = false; flush_done = true;
    irq_restore(f);
    return true;
}
static void flush_main(void *ctx)
{
    (void)ctx;
    for (;;) {
        if (!flush_run_once())
            kwait_wait_until(&flush_wake, flush_ready, NULL, deadline_after_ms(1000));
    }
}
int storage_power_init(void)
{
    if (flush_worker) return 0;
    kwait_init(&flush_wake);
    flush_worker = task_create_kernel("power-flush", flush_main, NULL, P_DEVICE);
    if (!flush_worker) return -ENOMEM;
    task_start(flush_worker);
    return 0;
}
int storage_sync_bounded(uint64_t end)
{
    uint32_t f = irq_save();
    if (!flush_worker) { irq_restore(f); return -ENOTSUP; }
    if ((flush_pending || (flush_done && !flush_observed)) && end != flush_deadline) {
        irq_restore(f); return -EBUSY;
    }
    if (flush_done && end == flush_deadline) {
        flush_observed = true;
        int e = flush_result;
        irq_restore(f); return e;
    }
    if (deadline_passed(end)) { flush_observed = true; irq_restore(f); return -VFS_ETIMEOUT; }
    if (!flush_pending) {
        if (end > UINT64_MAX / 1000) { irq_restore(f); return -EINVAL; }
        flush_deadline = end;
        flush_request = (struct vfs_request){.deadline = end * 1000,
            .now = flush_now, .pause = flush_pause, .operation = "power-flush", .site = "mount"};
        flush_done = flush_observed = false; flush_pending = true;
        kwait_wake_all(&flush_wake);
    }
    irq_restore(f);
    return -EAGAIN;
}
