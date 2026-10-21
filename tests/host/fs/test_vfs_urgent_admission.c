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
static bool *held_busy;
static unsigned wait_mode;
uint32_t fs_host_clock_ms(void) { return clock_ms; }
static uint64_t now(void *arg) { (void)arg; return (uint64_t)clock_ms*1000; }
static void urgent_pause(void *arg) { (void)arg; clock_ms++; }
static void hold_pause(void *arg) {
    (void)arg; clock_ms+=125;
    if (wait_mode==3 && clock_ms-origin==125) {
        /* The first wait had no owner. A real blocker appears on retry. */
        struct vfs_request owner={.now=now,.pause=urgent_pause,.deadline=UINT64_MAX,
            .operation="write",.site="writer",.task={.name="writer",.id=17}};
        assert(!vfs_admit(&space,1u<<2,&owner)); held=vfs_io_begin_locked(&space);
    }
    if (clock_ms-origin==250) {
        if (wait_mode==1 || wait_mode>=3) *held_busy=false;
        if (wait_mode==2) { space.waiters=0; space.serving[2]=space.next[2]; }
        if (!wait_mode || wait_mode==3) { vfs_io_end_locked(&space,held); vfs_unlock(&space); }
    }
}
static int read_busy_admission(struct vfs_request *r,bool *busy) {
    struct fs_request_scope scope; fs_request_enter(&scope,r);
    vfs_lock(&space,0);
    struct fat_entry entry={0}; struct fat_cursor cursor={0};
    uint64_t pos=0; size_t done=99;
    int e=vfs_read_identity_locked(&space,0,&volume,&entry,&cursor,busy,&pos,&pos,
        false,0,0,0,0,&done);
    vfs_unlock(&space); fs_request_leave(&scope);
    assert(!*busy && (!e ? done==0 : true)); return e;
}
static int read_admission(struct vfs_request *r) { bool busy=false; return read_busy_admission(r,&busy); }
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
static void backlog_progress(void) {
    struct vfs_file_identity other={0};
    struct vfs_node writer={.identity=&other};
    struct vfs_request urgent={.now=now,.pause=urgent_pause,.deadline=UINT64_MAX,
        .operation="read",.site="native",.urgent_reader=true};
    for (unsigned wrap=0;wrap<2;wrap++) {
        clock_ms=wrap ? UINT32_MAX-200 : 1000;
        struct vfs_waiter tickets[64]={0};
        uint32_t base=space.serving[2];
        for (unsigned i=0;i<64;i++) {
            tickets[i]=(struct vfs_waiter){.next=i<63 ? &tickets[i+1] : 0,
                .mask=1u<<2,.started=clock_ms-200,
                .owner={.task={.name="writer",.id=17},.op="write",.site="writer",.attribution=VFS_ATTR_COMPLETE}};
            tickets[i].ticket[2]=base+i;
        }
        space.next[2]=base+64; space.waiters=tickets;
        /* An unrelated active mutation blocks the whole metadata queue,
         * but must never close urgent admission on this different file. */
        space.mutating=1u<<2; space.io_file[2]=&writer;
        for (unsigned i=0;i<100;i++,clock_ms++) assert(!read_admission(&urgent));
        space.mutating=0; space.io_file[2]=0;
        /* A known different-file ticket and read-only metadata also cannot
         * close the urgent gate. An unknown file mutation still can. */
        tickets[0].file=&other;
        for (unsigned i=0;i<50;i++,clock_ms++) assert(!read_admission(&urgent));
        tickets[0].file=0; strcpy(tickets[0].owner.op,"commit");
        for (unsigned i=0;i<50;i++,clock_ms++) assert(!read_admission(&urgent));
        strcpy(tickets[0].owner.op,"write");
        tickets[0].mask|=1u<<3; tickets[0].ticket[3]=space.serving[3]; space.mutating=1u<<3;
        for (unsigned i=0;i<50;i++,clock_ms++) assert(!read_admission(&urgent));
        space.mutating=0; tickets[0].mask=1u<<2;
        /* Drain/open turns are shared across 64 successive aged tickets.
         * The unit fixture advances their FIFO frontier; every reader uses
         * production admission, cancellation and the native read loop. */
        uint32_t begin=clock_ms,last=clock_ms; unsigned completed=0,max_gap=0;
        while (clock_ms-begin<1000) {
            unsigned head=(clock_ms-begin)/16; if (head>63) head=63;
            space.waiters=&tickets[head]; space.serving[2]=base+head;
            urgent.deadline=now(0)+1000;
            int e=read_admission(&urgent);
            if (!e) {
                unsigned gap=clock_ms-last; if (gap>max_gap) max_gap=gap;
                last=clock_ms; completed++;
            } else assert(e==-VFS_ETIMEOUT);
            clock_ms++;
        }
        assert(completed>=450 && max_gap<=VFS_URGENT_DRAIN_MS+2);
        printf("F1-87 C backlog: 64 aged tickets, urgent reads=%u/1000ms max_gap=%ums wrap=%u PASS\n",completed,max_gap,wrap);
        space.waiters=0; space.serving[2]=space.next[2]; advance_cancelled(&space);
        urgent.deadline=UINT64_MAX;
    }
}
void vfs_wait_observer(char drive,uint32_t ms,char holder,const char *reason,
    const struct fs_task_identity *task,const struct vfs_hold_info *owner) {
    if (!wait_test || task->id!=29) return;
    assert(drive=='C' && ms==250 && holder=='C' && !strcmp(reason,"volume"));
    assert(owner->attribution==VFS_ATTR_COMPLETE && !strcmp(owner->op,wait_mode==1 ? "read" : "write") &&
        !strcmp(owner->site,wait_mode==1 ? "native" : "writer") && owner->task.id==17);
    records++;
}
int main(void) {
    vfs_init(&space); space.volumes[2]=&volume;
    for (unsigned wrap=0;wrap<2;wrap++) {
        clock_ms=wrap ? UINT32_MAX-50 : 1000; origin=clock_ms;
        vfs_lock(&space,0); space.readers[2]=2; vfs_unlock(&space);
        struct vfs_request ticket={.now=now,.pause=ticket_pause,.deadline=UINT64_MAX,
            .operation="write",.site="native"};
        assert(!vfs_admit(&space,1u<<2,&ticket));
        assert(clock_ms-origin<=120 && !space.readers[2] && !space.waiters);
        vfs_unlock(&space);
    }
    assert(admitted==20 && refused==2);
    backlog_progress();
    /* Native read loops must publish one accumulated successful wait, even
     * though each cancellable pause returns without an error. */
    clock_ms=origin=5000; wait_test=true;
    struct vfs_request owner={.now=now,.pause=urgent_pause,.deadline=UINT64_MAX,
        .operation="write",.site="writer",.task={.name="writer",.id=17}};
    assert(!vfs_admit(&space,1u<<2,&owner)); held=vfs_io_begin_locked(&space);
    struct vfs_request reader={.now=now,.pause=hold_pause,.deadline=UINT64_MAX,
        .operation="read",.site="native",.task={.name="native",.id=29}};
    assert(!read_admission(&reader) && records==1);
    /* io_active has no volume holder. Retain the actual reading task's
     * copied attribution until the shared description becomes available. */
    for (wait_mode=1;wait_mode<=4;wait_mode++) {
        clock_ms=origin+=3000;
        struct vfs_reader active;
        bool busy=wait_mode!=2; held_busy=&busy;
        unsigned expected=records+(wait_mode<4);
        struct vfs_waiter ticket={.mask=1u<<2,.started=clock_ms-200,
            .owner={.task={.name="writer",.id=17},.op="write",.site="writer",.attribution=VFS_ATTR_COMPLETE}};
        if (wait_mode==1) {
            struct fs_request_scope scope;
            struct vfs_request reading=reader; reading.task.id=17;
            fs_request_enter(&scope,&reading); vfs_lock(&space,0);
            reader_begin(&space,&active,2,&busy); vfs_unlock(&space); fs_request_leave(&scope);
        } else if (wait_mode==2) {
            ticket.ticket[2]=space.serving[2]; space.next[2]++; space.waiters=&ticket;
        } else if (wait_mode==4) {
            /* A fully attributed task published namespace_owner; the reader
             * that follows has no site. Its record must not borrow the
             * previous owner's site, and a fallback-only blocker never
             * produces a valid wait record. */
            struct fs_request_scope outer,inner;
            struct vfs_request siteless={.now=now,.pause=urgent_pause,.deadline=UINT64_MAX,
                .operation="read",.task={.name="kernel",.id=41}};
            fs_request_enter(&outer,&owner); vfs_lock(&space,0);
            assert(space.namespace_owner.attribution==VFS_ATTR_COMPLETE && !strcmp(space.namespace_owner.site,"writer"));
            fs_request_enter(&inner,&siteless); reader_begin(&space,&active,2,&busy); fs_request_leave(&inner);
            assert(active.owner.task.id==41 && !strcmp(active.owner.op,"read") && !strcmp(active.owner.site,"vfs") &&
                active.owner.attribution==VFS_ATTR_OPERATION && !meaningful_owner(&active.owner));
            vfs_unlock(&space); fs_request_leave(&outer);
        }
        assert(!read_busy_admission(&reader,&busy) && records==expected);
        if (wait_mode==1 || wait_mode==4) { vfs_lock(&space,0); reader_end(&space,&active); vfs_unlock(&space); }
    }
    wait_mode=0; wait_test=false;
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
    puts("F1-87 A/C admission: bounded 10ms drain/reader turns, FIFO <=120ms; tagged 250ms mutation/ticket/io_active waits, empty-first retry, siteless reader not borrowed; explicit/fallback tags PASS");
    return 0;
}
