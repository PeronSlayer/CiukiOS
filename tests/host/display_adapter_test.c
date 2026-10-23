/* Exercise the actual adapter's cursor-evidence orchestration without I/O.
 * The unused hardware registration sections are discarded by the linker.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <ciuki/kernel.h>
#include "../../src/kernel/drivers/video/s3savage/s3savage_register.c"
volatile uint64_t g_ticks;
static unsigned sets,moves,shows,hides;
static bool fail_set;
static int shape(void *ctx,const struct accel_cursor *cursor)
{
    (void)ctx; sets++; g_ticks++;
    for (unsigned i=0;i<32;i++) assert(!cursor->mask[i] && !cursor->image[i]);
    return fail_set ? -EIO : 0;
}
static int move(void *ctx,int32_t x,int32_t y) { (void)ctx; assert(!x && !y); moves++; g_ticks++; return 0; }
static int show(void *ctx,bool visible)
{ (void)ctx; if (visible) shows++; else hides++; g_ticks++; return 0; }
static bool cancelled(void *ctx) { (void)ctx; return true; }
static void reset(void) { g_ticks=sets=moves=shows=hides=0; fail_set=false; }
int main(void)
{
    struct accel_ops ops={.cursor_set=shape,.cursor_move=move,.cursor_show=show};
    reset(); assert(!cursor_test(&ops,100,NULL,NULL) && sets==1 && moves==1 && shows==1 && hides==2);
    reset(); fail_set=true; assert(cursor_test(&ops,100,NULL,NULL)==-EIO && !shows && hides==2);
    reset(); assert(cursor_test(&ops,3,NULL,NULL)==-ETIMEDOUT && !shows && hides==2);
    reset(); assert(cursor_test(&ops,100,cancelled,NULL)==-EINTR && !sets && !shows && hides==1);
    ops.cursor_set=NULL; assert(cursor_test(&ops,100,NULL,NULL)==-EOPNOTSUPP);
    puts("display adapter cursor evidence: transparent shape, driver hooks, hide-before-load/final hide, deadline, cancellation and failure cleanup PASS");
    return 0;
}
