/* F1-87 amendment 1: one continuous reconstructed 400 s trial. Production
 * run queues, CPU accounting, timers, block requests, PCM mixer and refill
 * service; fake CPU/device clocks and transport registers. Storage arrivals
 * use the measured hold histogram, split into <=111 ms commands. No PCM
 * reset between holds. This tests C at the block boundary, not B admission.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#include "t23_trial.h"
#include "../../src/kernel/fs/blkdev.h"
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#undef proc_task_switch
static void advance(unsigned us);
#include <ciuki/audio_backend.h>
#include "../../src/kernel/audio/mixer.c"
#include "../../src/kernel/audio/audio_dev.c"

static struct audio_transport transport;
static struct audio_stream pcm;
static struct audio_frame pcm_ring[AUDIO_RING];
static struct task decoder, device_peer, vm_peer;
static struct blk_queue queue;
static struct blkdev disk;
static struct blk_request *decode_request;
static uint8_t compressed[4096];
static uint64_t decode_arrived, read_latency_max;
static unsigned decoded, decoded_min, decode_reads, refills, commands, service_ms;
static unsigned audio_overruns, other_overruns, priority_checks;
static uint64_t class_total[P_COUNT];
static uint64_t next_refill, next_pump;
static struct vfs_request audio_read;
static bool vfs_replay, old_admission;
static unsigned admission_overruns;
static struct vfs admission_space;
static struct fat_volume admission_volume;
static struct vfs_node admission_writer;
static struct vfs_file_identity unrelated_identity;
static struct vfs_io_state admission_hold;
static int admission_current=-1;
static uint64_t admission_end;
static void step(void);

/* The scheduler fixture supplies only architectural/time boundaries. Refill
 * takes an uncontended service mutex here; no fake wait advances CPU time. */
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { CHECK(!m->owner); m->owner=g_current; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner==g_current); m->owner=0; }
void kwait_init(struct kwait *q) { memset(q,0,sizeof(*q)); }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kwait_wake_io(struct kwait *q) { (void)q; }
void kwait_wake_irq(struct kwait *q,bool io) { (void)q; (void)io; }
void klog(const char *fmt,...) { (void)fmt; }
bool ktime_elapsed_us(uint64_t start,uint32_t us) { return cycles-start>=us; }
uint32_t fs_host_clock_ms(void) { return (uint32_t)g_ticks; }

/* Native mixer arithmetic is real. The boundary charge is a conservative
 * 100 us/quantum CPU assumption, separate from sleep/device elapsed time. */
void audio_vm_tap(const struct audio_frame *out,unsigned frames)
{ CHECK(out && frames==AUDIO_QUANTUM); advance(100); }
void audio_backend_snapshot(struct audio_transport *out) { *out=transport; }
void audio_backend_idle(bool idle) { transport.idle_tail=idle; }
void audio_backend_rebase(uint64_t base) { transport.frame_base=base; }
int audio_backend_progress(void)
{
    uint64_t fetched=cycles*AUDIO_RATE/1000000u;
    if (fetched>transport.published*AUDIO_QUANTUM) return -EIO;
    transport.fetched=transport.generation_fetched=fetched;
    transport.retired=fetched/AUDIO_QUANTUM;
    return 0;
}
int audio_backend_publish(const struct audio_frame *frames)
{
    CHECK(frames && transport.published-transport.retired<AUDIO_MAX_LEAD);
    transport.published++; advance(20); return 0;
}
int audio_backend_start(void) { transport.running=true; return 0; }
int audio_backend_stop(void) { transport.running=false; return 0; }
void audio_backend_shutdown(void) { transport.running=false; }
void audio_backend_deadline(void) { transport.deadline_misses++; }

struct operation {
    unsigned due, remaining, service, kind;
    struct blk_request *request;
    uint8_t block[512];
};
#define OPERATIONS (T23_DESKTOP_HOLDS+T23_LOG_HOLDS+6+1)
static struct operation operations[OPERATIONS];
static unsigned operation_count, finished[4];

static void advance(unsigned us)
{
    uint64_t end=cycles+us;
    while ((g_ticks+1)*1000u<=end) {
        cycles=g_ticks*1000u;
        run_tick(0x87);
    }
    cycles=end;
}
static void block_until(struct task *t,uint64_t due)
{
    CHECK(g_current==t && due>g_ticks);
    task_sleep_ms((uint32_t)(due-g_ticks));
}
static uint64_t request_now(void *arg) { (void)arg; return g_ticks*1000u; }
static void request_pause(void *arg) {
    (void)arg; CHECK(vfs_replay && g_current==&decoder);
    block_until(&decoder,g_ticks+1);
    while (g_current!=&decoder) { step(); if (g_need_resched) schedule(); }
}
static struct blk_request *request(enum blk_operation op,uint64_t lba,unsigned bytes,
                                   void *buffer,bool urgent)
{
    struct fs_request_scope scope;
    fs_request_enter(&scope,urgent ? &audio_read : 0);
    struct blk_request *r=blk_request_prepare(&disk,op,lba,bytes,buffer,
        op==BLK_READ ? BLK_DEMAND : BLK_BACKGROUND,0,(uint32_t)g_ticks+20,30000);
    fs_request_leave(&scope);
    CHECK(r && !blk_request_submit(r)); return r;
}
static void retire(struct blk_request **slot)
{
    struct blk_request *r=*slot;
    CHECK(r && r->state==BLK_COMPLETED && !r->result && r->bytes==r->length);
    CHECK(!blk_request_release(r)); *slot=0;
}
static int transfer(struct blkdev *d,uint64_t lba,uint32_t n,void *buffer)
{
    (void)d; CHECK(buffer && n && g_current==&reporter);
    struct blk_request *r=queue.active;
    CHECK(r && lba==r->lba);
    service_ms=T23_SERVICE_MS;
    for (unsigned i=0;i<operation_count;i++)
        if (r==operations[i].request) service_ms=operations[i].service;
    CHECK(service_ms && service_ms<=T23_SERVICE_MS);
    /* Real queue choice: a fresh urgent read wins over queued normal reads
     * unless age promotion/dependencies require an older command first. */
    if (r->context && r->context->urgent_reader)
        for (struct blk_request *p=queue.head;p;p=p->next)
            if (p->operation==BLK_READ && !p->context && g_ticks-p->enqueued<BLKREQ_PROMOTE_MS)
                priority_checks++;
    if (r->operation==BLK_READ && !r->context)
        for (struct blk_request *p=queue.head;p;p=p->next)
            if (p->context && p->context->urgent_reader)
                CHECK(g_ticks-r->enqueued>=BLKREQ_PROMOTE_MS);
    commands++;
    uint64_t due=g_ticks+service_ms;
    block_until(&reporter,due);
    /* A synchronous controller continuation sleeps; other production tasks
     * run and the PCM clock advances during the entire device command. */
    while (g_current!=&reporter) { step(); if (g_need_resched) schedule(); }
    CHECK(g_ticks>=due); memset(buffer,0x87,n*512); return 0;
}
static int write_transfer(struct blkdev *d,uint64_t lba,uint32_t n,const void *buffer)
{ return transfer(d,lba,n,(void *)buffer); }
static int flush_transfer(struct blkdev *d)
{ uint8_t b[512]; return transfer(d,queue.active->lba,1,b); }

static void add_operation(unsigned due,unsigned ms,unsigned kind)
{
    CHECK(operation_count<OPERATIONS);
    operations[operation_count++]=(struct operation){.due=due,.remaining=ms,.kind=kind};
}
static void admission_arrivals(void)
{
    if (admission_current>=0 && g_ticks>=admission_end) {
        struct operation *op=&operations[admission_current];
        vfs_io_end_locked(&admission_space,admission_hold);
        admission_space.io_file[2]=0; vfs_unlock(&admission_space);
        op->remaining=0; finished[op->kind]++; admission_current=-1;
    }
    if (admission_current>=0) return;
    unsigned next=operation_count;
    for (unsigned i=0;i<operation_count;i++) if (operations[i].remaining && operations[i].due<=g_ticks &&
        (next==operation_count || operations[i].due<operations[next].due)) next=i;
    if (next==operation_count) return;
    static const char *const tags[]={"read","log_write","commit","mkdir"};
    struct operation *op=&operations[next];
    struct vfs_request holder={.deadline=UINT64_MAX,.now=request_now,.pause=request_pause,
        .operation=tags[op->kind],.site=op->kind ? "writer" : "native"};
    struct fs_request_scope scope; fs_request_enter(&scope,&holder);
    CHECK(!vfs_admit(&admission_space,1u<<2,&holder));
    /* A synthetic transfer holds the real production mutation guard for the
     * complete measured interval, not a series of 111ms commands. Writers
     * identify a different file. The old coarse-volume policy is represented
     * by making every identity conflict; the admission code is unchanged. */
    admission_writer.identity=old_admission ? 0 : &unrelated_identity;
    admission_space.io_file[2]=&admission_writer;
    admission_hold=vfs_io_begin_locked(&admission_space); fs_request_leave(&scope);
    admission_current=(int)next; admission_end=g_ticks+op->remaining;
}
static void admission_read(void)
{
    struct fs_request_scope scope; fs_request_enter(&scope,&audio_read);
    vfs_lock(&admission_space,0);
    struct fat_entry entry={0}; struct fat_cursor cursor={0};
    uint64_t pos=0, arrived=g_ticks; bool busy=false; size_t done=99;
    /* Zero data here isolates namespace admission; the following production
     * block request supplies the actual compressed-data completion/PCM credit. */
    CHECK(!vfs_read_identity_locked(&admission_space,0,&admission_volume,&entry,&cursor,&busy,
        &pos,&pos,false,0,0,0,0,&done) && !done);
    vfs_unlock(&admission_space); fs_request_leave(&scope);
    if (g_ticks-arrived>250) admission_overruns++;
}
static void arrivals(void)
{
    if (vfs_replay) admission_arrivals();
    bool actor_busy[3]={false};
    for (unsigned i=0;!vfs_replay && i<operation_count;i++) {
        struct operation *op=&operations[i];
        unsigned actor=op->kind==0 ? 0 : op->kind==3 ? 2 : 1;
        if (op->request && op->request->state==BLK_COMPLETED) {
            retire(&op->request); CHECK(op->remaining>=op->service);
            op->remaining-=op->service;
            if (!op->remaining) finished[op->kind]++;
        }
        /* The desktop, writer and DOS caller each issue serial operations.
         * Arrivals may backlog, but one busy desktop cannot issue several
         * simultaneous reads; commit follows its writer's preceding append. */
        if (!op->request && op->remaining && g_ticks>=op->due && !actor_busy[actor]) {
            op->service=op->remaining<T23_SERVICE_MS ? op->remaining : T23_SERVICE_MS;
            op->request=request(op->kind ? BLK_WRITE : BLK_READ,100+i*2,512,op->block,false);
        }
        if (op->remaining) actor_busy[actor]=true;
    }
    if (queue.head && reporter.state==T_BLOCKED && !queue.busy) task_start(&reporter);
    if (decode_request && decode_request->state==BLK_COMPLETED && decoder.state==T_BLOCKED)
        task_start(&decoder);
}
static void step(void)
{
    arrivals();
    if (g_current==&hog) {
        CHECK(!audio_backend_progress());
        audio_service(); refills++;
        CHECK(!audio.error && !audio.deadlines && !audio.underruns);
        next_refill=g_ticks+4;
        task_device_deadline(&hog,next_refill); block_until(&hog,next_refill);
    } else if (g_current==&normal) {
        /* Music's pump copies available decoded frames into its small kernel
         * stream. Decoding is credited ONLY when its storage read completes. */
        unsigned n=AUDIO_RING-pcm.queued;
        if (n>decoded) n=decoded;
        pcm.queued+=n; pcm.accepted+=n; decoded-=n;
        if (decoded<decoded_min) decoded_min=decoded;
        advance(100); next_pump=g_ticks+4; block_until(&normal,next_pump);
    } else if (g_current==&decoder) {
        if (decode_request && decode_request->state==BLK_COMPLETED) {
            uint64_t latency=g_ticks-decode_arrived;
            if (latency>read_latency_max) read_latency_max=latency;
            retire(&decode_request); decode_reads++;
            decoded+=AUDIO_RATE/4;
            CHECK(decoded<=2*AUDIO_RATE); /* admission reserved this space */
        }
        /* Like Music's producer, refill available decoded space rather than
         * limiting catch-up to real-time rate after a delayed read. */
        if (!decode_request && decoded<=2*AUDIO_RATE-AUDIO_RATE/4) {
            decode_arrived=g_ticks;
            if (vfs_replay) admission_read();
            decode_request=request(BLK_READ,4096,sizeof(compressed),compressed,true);
        }
        advance(50);
        block_until(&decoder,g_ticks+(decode_request ? 30000 : 4));
    } else if (g_current==&reporter) {
        if (queue.head) {
            /* Requests carry one budget slice. The measured 111 ms command
             * maximum is a device delay, never device-task CPU credit. */
            CHECK(blk_queue_run_one(&queue));
            advance(50);
        } else block_until(&reporter,g_ticks+1000);
    } else if (g_current==&ui) {
        advance(250); block_until(&ui,g_ticks+2);
    } else if (g_current==&device_peer) {
        advance(300); block_until(&device_peer,g_ticks+2);
    } else if (g_current==&vm_peer) {
        advance(1000); block_until(&vm_peer,g_ticks+7);
    } else { CHECK(g_current==&idle); advance(1000); }
}
static void collect_overruns(void)
{
    cpu_account();
    for (unsigned i=0;i<P_COUNT;i++) class_total[i]+=classes[i];
    CHECK(!overrun_lost);
    for (unsigned i=0;i<overrun_used;i++) {
        if (overrun_events[i].id==hog.id) audio_overruns+=overrun_events[i].count;
        else other_overruns+=overrun_events[i].count;
    }
    /* Use the real reporting path and start another 2 s window. */
    record_bytes=0; records[0]=0; cpu_emit();
    CHECK(!overrun_used);
}
int main(int argc,char **argv)
{
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--vfs") || !strcmp(argv[1],"--old-vfs"))));
    vfs_replay=argc==2; old_admission=vfs_replay && !strcmp(argv[1],"--old-vfs");
    reset();
    strcpy(hog.name,"audio-refill"); strcpy(normal.name,"music-pump");
    reporter=(struct task){.id=5,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED};
    strcpy(reporter.name,"storage-writer");
    decoder=(struct task){.id=6,.generation=1,.prio=P_NORMAL,.state=T_BLOCKED};
    strcpy(decoder.name,"music-decoder"); ui.all_next=&reporter; reporter.all_next=&decoder;
    device_peer=(struct task){.id=7,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED};
    vm_peer=(struct task){.id=8,.generation=1,.prio=P_NORMAL,.state=T_BLOCKED};
    strcpy(device_peer.name,"device-work"); strcpy(vm_peer.name,"dosvm");
    decoder.all_next=&device_peer; device_peer.all_next=&vm_peer;
    task_set_class(&ui,P_NORMAL);
    hog.state=normal.state=ui.state=T_BLOCKED;
    blk_queue_init(&queue,"t23-replay",4096);
    disk=(struct blkdev){.read=blkdev_read,.write=blkdev_write,.flush=blkdev_flush,
        .request_read=transfer,.request_write=write_transfer,.request_flush=flush_transfer,
        .queue=&queue,.capacity=8192,.sector_size=512,.write_cache_state=BLKDEV_CACHE_DISABLED};
    if (vfs_replay) {
        vfs_init(&admission_space); admission_space.volumes[2]=&admission_volume;
        admission_volume.dev=&disk;
    }
    audio_read=(struct vfs_request){.deadline=UINT64_MAX,.now=request_now,.pause=request_pause,
        .urgent_reader=true,.operation="read",.site="native"};
    for (unsigned i=0;i<T23_DESKTOP_HOLDS;i++) {
        add_operation(t23_arrival_ms(i),t23_desktop_hold_ms(i),0);
        if (!(i%15)) add_operation(t23_arrival_ms(i)+37,t23_log_hold_ms(i/15),1);
        if (!(i%100)) add_operation(t23_arrival_ms(i)+53,602,2);
    }
    add_operation(t23_arrival_ms(250)+71,441,3);
    CHECK(operation_count==OPERATIONS);
    decoded=decoded_min=2*AUDIO_RATE;
    pcm=(struct audio_stream){.rate=AUDIO_RATE,.gain={100,100},.started=true,
        .queued=AUDIO_RING,.accepted=AUDIO_RING};
    decoded-=AUDIO_RING;
    for (unsigned i=0;i<AUDIO_RING;i++) pcm_ring[i]=(struct audio_frame){1000,-1000};
    pcm.ring[0]=pcm_ring; pcm.ring[1]=pcm_ring+1024;
    audio=(struct audio_state){.ready=true,.running=true,.master={.left=100,.right=100},
        .output=AUDIO_LEAD*AUDIO_QUANTUM};
    audio.streams[0]=&pcm;
    transport=(struct audio_transport){.ready=true,.running=true,.published=AUDIO_LEAD};
    task_start(&normal); task_start(&ui); task_start(&hog); task_start(&decoder);
    task_start(&device_peer); task_start(&vm_peer); schedule();
    unsigned report_due=2000;
    while (g_ticks<T23_SESSION_MS) {
        step(); if (g_need_resched) schedule();
        if (g_ticks>=report_due) { collect_overruns(); report_due=(unsigned)g_ticks+2000; }
    }
    collect_overruns();
    CHECK(finished[0]==513 && finished[1]==35 && finished[2]==6 && finished[3]==1);
    printf("F1-87 C continuous replay: wall_ms=%llu desktop=%u log=%u commit=%u mkdir=%u commands=%u priority_checks=%u decode_reads=%u decoder_latency_max_ms=%llu decoded_min_frames=%u refills=%u refill_gap_max_us=%u refill_service_max_us=%u audio_overruns=%u other_overruns=%u starvations=%llu idle_pm=%llu device_pm=%llu normal_pm=%llu\n",
        (unsigned long long)g_ticks,finished[0],finished[1],finished[2],finished[3],commands,priority_checks,decode_reads,
        (unsigned long long)read_latency_max,decoded_min,refills,audio.max_service_gap_us,
        audio.max_service_us,audio_overruns,other_overruns,(unsigned long long)pcm.starvation,
        (unsigned long long)(class_total[P_IDLE]*1000/cycles),
        (unsigned long long)(class_total[P_DEVICE]*1000/cycles),
        (unsigned long long)(class_total[P_NORMAL]*1000/cycles));
    CHECK(!audio_overruns && !audio.error && !audio.deadlines && !audio.underruns && (old_admission || !pcm.starvation));
    if (!vfs_replay) CHECK(decoded_min && decode_reads>1500 && refills>50000 && commands>2000 && priority_checks);
    else {
        printf("F1-87 C VFS continuous old_policy=%u holds=555 complete_intervals=125..1837ms admission_deadline_overruns=%u decoder_latency_max_ms=%llu sampler_audio_overruns=%u PCM_starvations=%llu\n",
            old_admission,admission_overruns,(unsigned long long)read_latency_max,audio_overruns,(unsigned long long)pcm.starvation);
        CHECK(old_admission ? admission_overruns>0 : admission_overruns==0 && decoded_min>0);
    }
    CHECK(decoded+pcm.accepted==2*AUDIO_RATE+(uint64_t)decode_reads*(AUDIO_RATE/4));
    CHECK(pcm.queued+pcm.consumed==pcm.accepted); /* no invented/discarded PCM */
    puts("F1-87 C continuous replay: all 555 operations, finite PCM and zero refill overruns PASS");
    /* Negative control: the actual sampler must attribute a device overrun
     * to this task when a refill consumes an excessive CPU interval. */
    while (g_current!=&hog) { step(); if (g_need_resched) schedule(); }
    advance(5000); schedule(); collect_overruns();
    CHECK(audio_overruns);
    puts("F1-87 C negative control: real CPU sampler detects audio-refill overrun PASS");
    for (unsigned i=0;i<operation_count;i++) CHECK(!operations[i].request);
    if (decode_request) { CHECK(blk_request_cancel(decode_request)); CHECK(!blk_request_release(decode_request)); }
    CHECK(!blk_queue_destroy(&queue) && !blk_request_pool_host_cleanup());
    if (vfs_replay) { CHECK(admission_current<0); admission_space.volumes[2]=0; vfs_destroy(&admission_space); }
    return 0;
}
