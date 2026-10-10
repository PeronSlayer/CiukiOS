/* Thread bridge for f1-07's sole firmware consumer. Polling drains decoded
 * observations only; it never calls firmware or touches controller ports.
 * SeaBIOS set-1 handling: https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c
 * Its observed KEY and INT16 TEXT are distinct evidence, so TEXT must not
 * be counted a second time from a firmware KEY in input_digest_add.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/init.h>
#include <ciuki/input.h>
#include <ciuki/fwinput.h>
#include <ciuki/biosvm.h>
#include <ciuki/timing.h>

static struct task *adapter;
static gen_t generation;
static uint64_t loss_seen, reflected_seen, poll_deadline;
static uint64_t budget_violations;
static uint64_t adapter_wakes, adapter_polls, adapter_drains, adapter_events;
static struct kwait available;

gen_t fwinput_adapter_generation(void) { return generation; }

unsigned fwinput_adapter_step(void)
{
    if (!generation || !(read_eflags() & 0x200) || !g_current) return 0;
    reflected_seen = biosvm_input_reflections();
    if (!fwinput_pending()) return 0;
    adapter_drains++;
    uint64_t start = ktime_cycles(), tick = deadline_after_ms(0);
    struct fwinput_stats stats;
    fwinput_stats(&stats);
    if (stats.loss > loss_seen) input_firmware_loss(stats.loss - loss_seen);
    loss_seen = stats.loss;
    unsigned n = 0;
    while (n < 16 && !ktime_elapsed_us(start, 1000) && deadline_after_ms(0) - tick < 1) {
        struct fwinput_event e;
        if (!fwinput_poll(&e, 1)) break;
        /* f1-07's synthetic Pause position precedes the public F2 contract. */
        if (e.type == FWINPUT_KEY && e.code == 0x145) e.code = INPUT_KEY_PAUSE;
        input_firmware_event(&e, generation);
        adapter_events++;
        n++;
    }
    fwinput_stats(&stats);
    if (stats.loss > loss_seen) input_firmware_loss(stats.loss - loss_seen);
    loss_seen = stats.loss;
    if (biosvm_backend_state() == BIOSVM_DISABLED_BACKEND) input_firmware_disable(generation);
    uint64_t us = g_cpu_tsc && g_tsc_per_ms ? (ktime_cycles() - start) * 1000 / g_tsc_per_ms :
                                           (deadline_after_ms(0) - tick) * 1000;
    if (us > 1000) {
        budget_violations++;
        klog("[fwinput-adapter] budget exceeded site=queue us=%llu pending=%u violations=%llu",
             us, fwinput_pending(), budget_violations);
    }
    return n;
}

static bool adapter_ready(void *arg)
{
    (void)arg;
    /* A reflection bypasses the poll deadline. Backlog alone waits for the
     * next period, giving lower priorities a dispatch between bounded steps. */
    return biosvm_input_reflections() != reflected_seen ||
           (deadline_passed(poll_deadline) && fwinput_pending());
}

static void adapter_main(void *arg)
{
    (void)arg;
    for (;;) {
        fwinput_adapter_step();
        poll_deadline = deadline_after_ms(10);
        if (kwait_wait_until(&available, adapter_ready, 0, poll_deadline)) adapter_wakes++;
        else adapter_polls++;
    }
}

void fwinput_adapter_log_delivery(void)
{
    struct biosvm_input_diag d;
    struct fwinput_stats s;
    biosvm_input_snapshot(&d);
    fwinput_stats(&s);
    /* Fixed line count, each <240 bytes even with 20-digit counters. No
     * logging or extra controller reads in IRQ/trap/observer paths. */
    for (unsigned i = 0; i < 2; i++) {
        klog("[fwdelivery] irq=%u arrivals=%llu dispatched=%llu eoi=%llu queued=%llu",
             i ? 12 : 1, d.arrivals[i], d.dispatched[i], d.eois[i], d.queued[i]);
        klog("[fwdelivery] irq=%u reflected=%llu entries=%llu sentinel=%llu",
             i ? 12 : 1, d.reflected[i], d.entries[i], d.done[i]);
    }
    klog("[fwdelivery] pending=%04x physical_imr=%02x/%02x virtual_imr=%02x/%02x irr=%02x/%02x isr=%02x/%02x base=%02x/%02x state=%u active=%u disabled=%u",
         d.pending, d.physical_imr[0], d.physical_imr[1], d.imr[0], d.imr[1],
         d.irr[0], d.irr[1], d.isr[0], d.isr[1], d.base[0], d.base[1], d.state, d.active, d.disabled);
    klog("[fwdelivery] bda_head=%04x bda_tail=%04x mouse_head=%u mouse_tail=%u mouse_lost=%u port60=%llu last_status=%02x last_byte=%02x",
         d.bda_head, d.bda_tail, d.mouse_head, d.mouse_tail, d.mouse_lost,
         d.port60, d.last_status, d.last_byte);
    klog("[fwdelivery] observed=%llu scans=%llu aux=%llu text=%llu packets=%llu",
         s.observed_bytes, s.scan_bytes, s.aux_bytes, s.text, s.packets);
    klog("[fwdelivery] signals=%llu wakes=%llu polls=%llu drains=%llu events=%llu",
         d.signals, adapter_wakes, adapter_polls, adapter_drains, adapter_events);
    klog("[fwdelivery] loss=%llu resync=%llu service_overruns=%llu queue_overruns=%llu worker_yields=%llu",
         s.loss, s.resyncs, s.service_budget_violations, budget_violations, d.budget_yields);
}

static int adapter_error(const char *step, int error)
{
    klog("[fwinput-adapter] step=%s error=%d backend=%u", step, error, biosvm_backend_state());
    return error;
}

int fwinput_adapter_init(void)
{
    if (!(read_eflags() & 0x200) || !g_current) return adapter_error("thread_context", -EINVAL);
    if (g_boot.input_policy != CBI_INPUT_FIRMWARE && !(g_boot.flags & CBI_F_INPUT_FORCED))
        return adapter_error("input_policy", -ENOSYS);
    if (biosvm_backend_state() == BIOSVM_DISABLED_BACKEND) return adapter_error("disabled", -V86_EIO);
    if (adapter) return 0;
    struct task *t = task_create_kernel("firmware-queue", adapter_main, 0, P_INTERACTIVE);
    if (!t) return adapter_error("worker_create", -ENOMEM);
    int err = fwinput_init();
    const char *step = "fwinput_init";
    gen_t next = err ? 0 : gen_alloc();
    if (!err && !next) {
        step = "generation";
        err = -ENOSPC;
    }
    if (!err && !input_firmware_begin(next)) {
        step = "queue_begin";
        err = -EINVAL;
    }
    if (err) {
        task_kill(t, err);
        task_reap(t);
        return adapter_error(step, err);
    }
    kwait_init(&available);
    biosvm_set_input_wait(&available);
    reflected_seen = biosvm_input_reflections();
    generation = next;
    adapter = t;
    task_start(t);                    /* queue and generation published first */
    return 0;
}
