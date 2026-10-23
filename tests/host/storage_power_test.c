/* Production bounded flush mailbox; a fake slow transport retains its
 * request after caller timeout, then retires it before the next admission.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_CPU_H
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { (void)f; }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/storage.h>
#include <ciuki/task.h>
static unsigned checks, failures, calls;
static uint64_t ticks;
static struct task task;
static bool allocation_fails;
static int transport_result;
static const struct vfs_request *transport_request;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)
uint64_t deadline_after_ms(uint32_t ms) { return ticks + ms; }
bool deadline_passed(uint64_t end) { return ticks >= end; }
void task_sleep_ms(uint32_t ms) { ticks += ms; }
void kwait_init(struct kwait *q) { memset(q, 0, sizeof(*q)); }
void kwait_wake_all(struct kwait *q) { (void)q; }
bool kwait_wait_until(struct kwait *q, bool (*ready)(void *), void *ctx, uint64_t end)
{ (void)q; (void)end; return ready(ctx); }
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *ctx, enum task_prio p)
{ (void)fn; (void)ctx; CHECK(!strcmp(name,"power-flush") && p == P_DEVICE); return allocation_fails ? NULL : &task; }
void task_start(struct task *t) { CHECK(t == &task); }
struct storage *storage_get(void) { return NULL; }
int storage_sync_until(struct storage *s, const struct vfs_request *r)
{ (void)s; calls++; transport_request = r; return transport_result; }
#include "../../src/kernel/core/storage_power.c"
int main(void)
{
    CHECK(storage_sync_bounded(2000) == -ENOTSUP);
    allocation_fails = true; CHECK(storage_power_init() == -ENOMEM);
    allocation_fails = false; CHECK(!storage_power_init());
    CHECK(storage_sync_bounded(2000) == -EAGAIN && flush_pending);
    const struct vfs_request *retained = &flush_request;
    CHECK(retained->deadline == 2000000 && retained->now(NULL) == 0);
    CHECK(storage_sync_bounded(2000) == -EAGAIN && !calls);
    ticks = 2000;
    CHECK(storage_sync_bounded(2000) == -VFS_ETIMEOUT && flush_pending);
    CHECK(storage_sync_bounded(3000) == -EBUSY && flush_request.deadline == 2000000);
    transport_result = -VFS_ETIMEOUT;
    CHECK(flush_run_once() && calls == 1 && transport_request == retained);
    CHECK(!flush_run_once());
    CHECK(storage_sync_bounded(3000) == -EAGAIN && flush_pending && flush_request.deadline == 3000000);
    transport_result = 0; CHECK(flush_run_once());
    CHECK(!storage_sync_bounded(3000) && !storage_sync_bounded(3000));
    CHECK(storage_sync_bounded(4000) == -EAGAIN);
    transport_result = -FS_EIO; CHECK(flush_run_once());
    CHECK(storage_sync_bounded(4000) == -FS_EIO);
    CHECK(storage_sync_bounded(2000) == -VFS_ETIMEOUT && !flush_pending);
    printf("Storage bounded power worker: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
