/* Adapter for the in-tree family driver. No upstream fragments copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>
#include "savage2d.h"
#include "supersavage_display.h"
static const struct display_match matches[] = {
#define SAVAGE_DEVICE(id, chip) {0x5333,id,3,255},
#include "savage_ids.h"
#undef SAVAGE_DEVICE
};
static bool mode_adapter(const struct pci_func *p)
{ return p && p->vendor == 0x5333 && p->device == 0x8c2e; }
static const struct display_mode_provider modes = {
    supersavage_display_supported, supersavage_display_prepare,
    supersavage_display_begin, supersavage_display_write,
    supersavage_display_end, supersavage_display_complete, supersavage_display_lfb
};
static int selftest(uint64_t deadline, void (*line)(void *, const char *), void *ctx,
                    struct display_selftest_result *out)
{
    struct savage_sweep_result r = {.first=-1};
    int e = savage2d_hardware_sweep(deadline,line,ctx,&r);
    *out = (struct display_selftest_result){r.tried,r.passed,r.skipped,r.timeouts,r.completed,r.first,r.error,r.restored};
    return e;
}
static int cursor_test(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (!ops || !ops->cursor_set || !ops->cursor_move || !ops->cursor_show) return -EOPNOTSUPP;
    /* The existing driver reads back cursor enable in SHOW and cursor base
     * in SET. MOVE currently supplies submission evidence only. No static
     * register accessor is referenced. Zero coverage is fully transparent. */
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned step=0;step<4 && !e;step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) e=ops->cursor_show(ops->ctx,false);
        else if (step==1) e=ops->cursor_set(ops->ctx,&transparent);
        else if (step==2) e=ops->cursor_move(ops->ctx,0,0);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
const struct display_driver s3savage_driver = {
    .id="s3savage", .attempt_files={"C:/system/settings/accel.cfg"}, .name="S3 Savage", .matches=matches,
    .match_count=sizeof(matches)/sizeof(matches[0]), .framebuffer_bar=1,
    .probe=savage2d_supported, .eligible=savage2d_eligible,
    .start=savage2d_init, .fallback=savage2d_fallback_ops,
    .attempt_begin=savage2d_attempt_begin, .attempt_end=savage2d_attempt_end,
    .attempt_failed=savage2d_attempt_failed, .record=savage2d_record,
    .quarantine=savage2d_quarantine, .summary=savage2d_summary,
    .snapshot=savage2d_snapshot, .modes=&modes, .mode_adapter=mode_adapter,
    .check_begin=savage2d_check_begin, .selftest=selftest, .cursor_test=cursor_test,
    .framebuffer_bar_for=savage2d_framebuffer_bar
};
