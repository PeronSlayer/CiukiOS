/* Production admission with deterministic overlapping readers and waits.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../../src/kernel/fs/vfs.c"
#include <assert.h>
#include <stdio.h>
static struct vfs space;
static struct fat_volume volume;
static uint32_t clock_ms, origin;
static unsigned admitted, refused, records;
static struct vfs_io_state held;
static bool wait_test;
uint32_t fs_host_clock_ms(void) { return clock_ms; }
static uint64_t now(void *arg) { (void)arg; return (uint64_t)clock_ms*1000; }
static void urgent_pause(void *arg) { (void)arg; clock_ms++; }
static void hold_pause(void *arg) {
    (void)arg; clock_ms+=125;
    if (clock_ms-origin==250) { vfs_io_end_locked(&space,held); vfs_unlock(&space); }
}
static int read_admission(struct vfs_request *r) {
    struct fs_request_scope scope; fs_request_enter(&scope,r);
    vfs_lock(&space,0);
    struct fat_entry entry={0}; struct fat_cursor cursor={0};
    uint64_t pos=0; bool busy=false; size_t done=99;
    int e=vfs_read_identity_locked(&space,0,&volume,&entry,&cursor,&busy,&pos,&pos,
        false,0,0,0,0,&done);
    vfs_unlock(&space); fs_request_leave(&scope);
    assert(!busy && (!e ? done==0 : true)); return e;
}
static void ticket_pause(void *arg) {
    (void)arg;
    /* Two overlapping outstanding readers never drain while new readers
     * continue entering. Exercise the real native admission path, with a
     * finite deadline when the aged-ticket gate closes. */
    clock_ms+=10;
    struct vfs_request urgent={.now=now,.pause=urgent_pause,.deadline=now(0)+1000,
        .operation="read",.site="native",.urgent_reader=true};
    int e=read_admission(&urgent);
    if (!e) admitted++; else { assert(e==-VFS_ETIMEOUT); refused++; }
    if (clock_ms-origin>=VFS_URGENT_OVERTAKE_MS+10) {
        vfs_lock(&space,0); assert(space.readers[2]==2); space.readers[2]=0;
        fs_event_wake(&space.volume_changed); vfs_unlock(&space);
    }
}
void vfs_wait_observer(char drive,uint32_t ms,char holder,const char *reason,
    const struct fs_task_identity *task,const struct vfs_hold_info *owner) {
    if (!wait_test || task->id!=29) return;
    assert(drive=='C' && ms==250 && holder=='C' && !strcmp(reason,"volume"));
    assert(owner->attribution==VFS_ATTR_COMPLETE && !strcmp(owner->op,"write") && !strcmp(owner->site,"writer"));
    records++;
}
int main(void) {
    vfs_init(&space); space.volumes[2]=&volume;
    for (unsigned wrap=0;wrap<2;wrap++) {
        clock_ms=wrap ? UINT32_MAX-50 : 1000; origin=clock_ms;
        vfs_lock(&space,0); space.readers[2]=2; vfs_unlock(&space);
        struct vfs_request ticket={.now=now,.pause=ticket_pause,.deadline=UINT64_MAX,
            .operation="stat",.site="native"};
        assert(!vfs_admit(&space,1u<<2,&ticket));
        assert(clock_ms-origin<=120 && !space.readers[2] && !space.waiters);
        vfs_unlock(&space);
    }
    assert(admitted==18 && refused==4);
    /* Native read loops must publish one accumulated successful wait, even
     * though each cancellable pause returns without an error. */
    clock_ms=origin=5000; wait_test=true;
    struct vfs_request owner={.now=now,.pause=urgent_pause,.deadline=UINT64_MAX,
        .operation="write",.site="writer",.task={.name="writer",.id=17}};
    assert(!vfs_admit(&space,1u<<2,&owner)); held=vfs_io_begin_locked(&space);
    struct vfs_request reader={.now=now,.pause=hold_pause,.deadline=UINT64_MAX,
        .operation="read",.site="native",.task={.name="native",.id=29}};
    assert(!read_admission(&reader) && records==1);
    /* Explicit metadata remains valid, but fallback metadata cannot pass
     * the exact same observer predicate. */
    vfs_lock(&space,1u<<2); assert(space.holds[2].attribution!=VFS_ATTR_COMPLETE);
    vfs_tag_locked(&space,"metadata"); assert(space.holds[2].attribution==VFS_ATTR_OPERATION);
    vfs_site_locked(&space,"vfs"); assert(space.holds[2].attribution==VFS_ATTR_COMPLETE);
    vfs_unlock(&space);
    struct vfs_request partial={.now=now,.pause=urgent_pause,.deadline=UINT64_MAX,.operation="read"};
    assert(!vfs_admit(&space,1u<<2,&partial));
    assert(space.holds[2].attribution==VFS_ATTR_OPERATION);
    held=vfs_io_begin_locked(&space); vfs_io_end_locked(&space,held);
    assert(space.holds[2].attribution==VFS_ATTR_OPERATION); vfs_unlock(&space);
    partial.operation=0; partial.site="native";
    assert(!vfs_admit(&space,1u<<2,&partial));
    assert(space.holds[2].attribution==VFS_ATTR_SITE); vfs_unlock(&space);
    space.volumes[2]=0; vfs_destroy(&space);
    puts("F1-87 A/C admission: overlapping urgent readers close at 100ms, FIFO runs <=120ms, clock wrap; one tagged 250ms native wait; explicit/fallback tags PASS");
    return 0;
}
