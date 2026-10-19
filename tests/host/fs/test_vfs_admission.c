/* Combined f1-83/f3-49 admission invariants, fake request clock only.
 * Include the production implementation to exercise late _locked admission.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../../src/kernel/fs/vfs.c"
#include <assert.h>
#include <stdio.h>

static struct vfs space;
static struct blkdev provider={.logical=true};
static struct fat_volume volume={.dev=&provider,.mounted=true};
static uint64_t clock_us;
static bool cancel;
static uint64_t now(void *arg) { (void)arg; return clock_us; }
static bool cancelled(void *arg) { (void)arg; return cancel; }
static void pause_request(void *arg) { (void)arg; clock_us+=1000; }
static struct vfs_wait_info evidence;
static struct vfs_request request={.now=now,.cancelled=cancelled,.pause=pause_request,
    .owner=49,.wait=&evidence};
static void renew(void) {
    request.deadline=clock_us+2000000; memset(&evidence,0,sizeof(evidence)); cancel=false;
}
static void expired(int error) {
    assert(error==-VFS_ETIMEOUT && clock_us==request.deadline);
    assert(evidence.valid && evidence.owner==49 && evidence.reason==VFS_WAIT_VOLUME);
    assert(evidence.drives==(1u<<2) && evidence.elapsed_us==2000000 && evidence.outcome==error);
}
static void idle(void) {
    assert(!space.active && !space.reserved && !space.mutating && !space.active_tickets && !space.waiters);
    for (unsigned d=0;d<26;d++) assert(space.next[d]==space.serving[d] && !space.readers[d]);
}
static void cancel_successor(void *arg) {
    (void)arg;
    /* The outer request remains a live predecessor while the inner ticket
     * expires. Skipping that live predecessor would strand the outer wait. */
    uint32_t serving=space.serving[2];
    struct vfs_request successor=request;
    successor.deadline=clock_us+1000; successor.pause=pause_request; successor.wait=0;
    assert(vfs_admit(&space,1u<<2,&successor)==-VFS_ETIMEOUT);
    assert(space.serving[2]==serving);
    clock_us=request.deadline;
}
int main(void) {
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume));
    /* Retired tickets cannot admit metadata through an outstanding guard;
     * cancelled successors leave no hole or double retirement on unlock. */
    vfs_lock(&space,1u<<2); struct vfs_io_state io=vfs_io_begin_locked(&space);
    renew(); expired(vfs_admit(&space,1u<<2,&request));
    assert(space.mutating==(1u<<2) && space.next[2]==space.serving[2]);
    renew(); cancel=true; assert(vfs_admit(&space,1u<<2,&request)==-VFS_ECANCELLED); cancel=false;
    renew(); request.pause=cancel_successor;
    expired(vfs_admit(&space,1u<<2,&request)); request.pause=pause_request;
    vfs_io_end_locked(&space,io); vfs_unlock(&space); idle();
    renew(); assert(!vfs_admit(&space,1u<<2,&request)); vfs_unlock(&space); idle();

    /* Extending an active operation cannot sleep behind a guard whose FIFO
     * ticket has retired, or behind a ticket-free per-file reader. */
    vfs_lock(&space,1u<<2); io=vfs_io_begin_locked(&space);
    vfs_lock(&space,1u<<3); renew();
    assert(reserve_request(&space,1u<<2,&request)==-FS_EBUSY);
    vfs_unlock(&space); vfs_io_end_locked(&space,io); vfs_unlock(&space); idle();
    vfs_lock(&space,1u<<3); space.readers[2]=1; renew();
    assert(reserve_request(&space,1u<<2,&request)==-FS_EBUSY);
    space.readers[2]=0; vfs_unlock(&space); idle();

    /* Acquiring a lower additional letter releases/reacquires the complete
     * set under namespace, passing the production host lock-order checker. */
    vfs_lock(&space,1u<<3); vfs_reserve_locked(&space,1u<<2); vfs_unlock(&space); idle();

    /* The new fast paths must retain DOS deadlines and mandatory locks.
     * A synthetic in-flight description never reaches a disk operation. */
    struct vfs_table table={.vfs=&space,.request=&request};
    struct vfs_node *node=&space.nodes[0];
    *node=(struct vfs_node){.volume=&volume,.drive=2,.generation=space.generation[2],.refs=1};
    struct vfs_description *d=&space.descriptions[0];
    *d=(struct vfs_description){.node=node,.refs=1,.access=VFS_READ,.io_active=true};
    table.handles[0]=d; space.readers[2]=1;
    uint8_t byte=0xa5; size_t done=99; uint64_t position=99;
    renew(); expired(vfs_read(&table,0,&byte,1,&done));
    assert(!done && byte==0xa5 && d->refs==1 && !d->position);
    renew(); expired(vfs_seek(&table,0,1,VFS_SEEK_SET,&position)); assert(position==99);
    renew(); expired(vfs_close(&table,0)); assert(table.handles[0]==d && d->refs==1);
    renew(); expired(vfs_admit(&space,1u<<2,&request));
    space.readers[2]=0; d->io_active=false;
    space.range_locks[0]=(struct vfs_range_lock){.description=&space.descriptions[1],
        .node=node,.owner=49,.start=0,.end=1,.length=1};
    renew(); assert(vfs_read(&table,0,&byte,1,&done)==-VFS_ELOCK && !done && byte==0xa5);
    memset(space.range_locks,0,sizeof(space.range_locks)); idle();
    memset(space.descriptions,0,sizeof(space.descriptions)); memset(space.nodes,0,sizeof(space.nodes));
    space.volumes[2]=0; vfs_destroy(&space);
    puts("PASS merged VFS: retired tickets, mutation/read guards, late EBUSY, ordered union, fast-path deadlines and record locks");
    return 0;
}
