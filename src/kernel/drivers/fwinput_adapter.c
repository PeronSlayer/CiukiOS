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

static struct task *adapter;
static gen_t generation;
static uint64_t loss_seen;

gen_t fwinput_adapter_generation(void) { return generation; }

unsigned fwinput_adapter_step(void)
{
    if (!generation || !(read_eflags() & 0x200) || !g_current) return 0;
    struct fwinput_event events[16];
    unsigned n = fwinput_poll(events, ARRAY_SIZE(events));
    struct fwinput_stats stats;
    fwinput_stats(&stats);
    if (stats.loss > loss_seen) input_firmware_loss(stats.loss - loss_seen);
    loss_seen = stats.loss;
    for (unsigned i = 0; i < n; i++) {
        /* f1-07's synthetic Pause position precedes the public F2 contract. */
        if (events[i].type == FWINPUT_KEY && events[i].code == 0x145)
            events[i].code = INPUT_KEY_PAUSE;
        input_firmware_event(&events[i], generation);
    }
    if (biosvm_backend_state() == BIOSVM_DISABLED_BACKEND) input_firmware_disable(generation);
    return n;
}

static void adapter_main(void *arg)
{
    (void)arg;
    for (;;) {
        fwinput_adapter_step();
        task_sleep_ms(1);             /* bounded batch, host timer stays live */
    }
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
    struct task *t = task_create_kernel("firmware-queue", adapter_main, 0, P_DEVICE);
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
    generation = next;
    adapter = t;
    task_start(t);                    /* queue and generation published first */
    return 0;
}
