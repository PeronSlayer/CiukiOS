/* Native PS/2 lifecycle and F1 probe boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_I8042_H
#define CIUKI_I8042_H

#include "input.h"

#define I8042_REPLY_MS 200u
#define I8042_SETUP_MS 500u
#define I8042_RESENDS 2u
/* Driver-local errors; no additions to the frozen F0 syscall ABI. */
#define I8042_EIO 5
#define I8042_ETIMEDOUT 110
#define I8042_EPROTO 71
struct i8042_stats {
    uint64_t reads, writes, commands, resends, timeouts, stalled, drained;
    uint32_t last_elapsed_ms;
    gen_t generation;
    int last_error;
    uint8_t initial_config, config;
    bool active, pending_command, quarantined, firmware;
};

/* Thread context with IF=1, after registry/scheduler initialization.
 * Firmware-first returns -ENOSYS before claims, PIC changes or port I/O.
 * Native activation transfers only the registry's exact "input" leases.
 * Failure after hardware access retains all claims in quarantine. */
int i8042_init(void);
int i8042_stop(gen_t generation);
const char *i8042_backend(void);
void i8042_snapshot(struct i8042_stats *out);
int probe_input(void);
int probe_input_fault(void);

/* Supervisor-only, runtime-selected input-fault fixture. There is no hook
 * installed in normal operation. A fixture has private state and no physical
 * claims/IRQs: callbacks MUST be fake I/O, never firmware or physical ports.
 * It exercises the same command engine, capture and queue as the native
 * backend. Begin requires the complete valid f1:input-fault selector.
 * End destroys the fixture; it cannot recover/reset the physical backend.
 * now returns nominal millisecond ticks; pause must return with IF=1 and
 * permit other tasks to run. Callback storage is pinned until end. */
struct i8042_test_io {
    uint8_t (*read)(void *arg, uint16_t port);
    void (*write)(void *arg, uint16_t port, uint8_t byte);
    uint64_t (*now)(void *arg);
    void (*pause)(void *arg);
};
int i8042_fault_begin(const struct i8042_test_io *io, void *arg);
int i8042_fault_command(bool aux, uint8_t byte);
int i8042_fault_capture(uint8_t status, uint8_t byte);
bool i8042_fault_read(struct input_event *out);
void i8042_fault_snapshot(struct i8042_stats *driver, struct input_stats *queue);
void i8042_fault_end(void);

#endif
