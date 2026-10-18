/* Same orchestration on the host and in the boot page. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/hardware_check.h>
static const char *const items[HW_CHECK_ITEMS] = {
    "ATA scratch", "Mount repair", "USB storage", "USB mouse", "Keyboard / EC",
    "Timer 10 s", "SMI bit 10 s", "Serial burst", "Savage sweep", "Audio 10 s"
};
void hardware_check_model(const struct hw_check_ops *ops, struct hw_check_result results[HW_CHECK_ITEMS])
{
    char line[200];
    ops->line(ops->ctx, "Hardware check - CiukiOS (each item <= 15 s)");
    for (unsigned i = 0; i < HW_CHECK_ITEMS; i++) {
        uint64_t start = ops->now(ops->ctx);
        results[i] = (struct hw_check_result){ .status = HW_FAIL };
        ksnprintf(line, sizeof(line), "RUN  %s", items[i]);
        ops->line(ops->ctx, line);
        ops->step(ops->ctx, i, start + HW_CHECK_LIMIT_MS, &results[i]);
        uint64_t elapsed = ops->now(ops->ctx) - start;
        if (elapsed > HW_CHECK_LIMIT_MS) {
            results[i].status = HW_FAIL;
            /* Preserve measurements from a truncated sweep/check. The
             * result line already reports elapsed time and failure. */
            if (!results[i].detail[0])
                ksnprintf(results[i].detail, sizeof(results[i].detail), "deadline exceeded: %llu ms", elapsed);
        }
        ksnprintf(line, sizeof(line), "%s %s: %s (%llu ms)",
            results[i].status == HW_PASS ? "PASS" : results[i].status == HW_SKIP ? "SKIP" : "FAIL",
            items[i], results[i].detail, elapsed);
        ops->line(ops->ctx, line);
    }
    /* One physical row per compact result leaves room for the PM snapshot
     * and before/clear/restore comparison on an 80x25 H-page photograph.
     * The complete result detail was already logged above. */
    ops->line(ops->ctx, "Hardware check complete - results:");
    for (unsigned i = 0; i < HW_CHECK_ITEMS; i++) {
        ksnprintf(line, 80, "%s %s: %s",
            results[i].status == HW_PASS ? "PASS" : results[i].status == HW_SKIP ? "SKIP" : "FAIL",
            items[i], results[i].detail);
        ops->line(ops->ctx, line);
    }
}
