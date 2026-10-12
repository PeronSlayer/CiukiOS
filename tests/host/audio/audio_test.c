/* Production fd/path/syscall, converter, persistence and transport with fake
 * RAM, disk, time and register boundary. SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#undef FD_TABLE_STACK_TEST
#include <stdarg.h>
#include <ciuki/ac97.h>
#include <ciuki/timing.h>
struct ciuki_boot_info g_boot;
uint64_t g_tsc_per_ms = 1000;
static uint8_t inb(uint16_t p) { (void)p; return 0; }
static uint32_t inl(uint16_t p) { (void)p; return 0; }
static void outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
static void outl(uint16_t p, uint32_t v) { (void)p; (void)v; }
uint64_t ktime_cycles(void) { return g_ticks * 1000; }
void klog(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
bool ktime_elapsed_us(uint64_t start, uint32_t us) { return ktime_cycles() - start >= us; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool deadline_passed(uint64_t deadline) { return (int64_t)(g_ticks - deadline) >= 0; }
gen_t gen_alloc(void) { static gen_t next; return ++next; }
uint32_t pmm_alloc_zone(int zone) { CHECK(zone == ZONE_DMA); return pmm_alloc(); }
void task_yield(void) { task_sleep_ms(0); }
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { CHECK(!m->owner); m->owner=g_current; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner==g_current); m->owner=0; }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn condition, void *arg, uint64_t deadline)
{
    (void)q;
    while (!condition(arg) && !deadline_passed(deadline)) task_sleep_ms(1);
    return condition(arg);
}
static struct resource audio_resources[12];
static unsigned resource_releases, resource_quarantines, irq_removals;
const struct resource *registry_get(unsigned i) { CHECK(i<12);return &audio_resources[i]; }
int registry_activate(int h,gen_t generation)
{
    CHECK(h>=0&&h<12&&audio_resources[h].generation==generation&&audio_resources[h].state==RS_CLAIMED);
    audio_resources[h].state=RS_ACTIVE;return 0;
}
int registry_quiesce(int h,gen_t generation,registry_idle_proof_fn proof)
{
    CHECK(h>=0&&h<12&&audio_resources[h].generation==generation&&audio_resources[h].state==RS_ACTIVE);
    audio_resources[h].state=RS_QUIESCING;CHECK(proof(h,generation));return 0;
}
int registry_release(int h,uint32_t generation)
{
    CHECK(h>=0&&h<12&&audio_resources[h].generation==generation&&audio_resources[h].state==RS_QUIESCING);
    audio_resources[h].state=RS_RELEASED;resource_releases++;return 0;
}
int registry_quarantine(int h,gen_t generation)
{
    CHECK(h>=0&&h<12&&audio_resources[h].generation==generation);
    audio_resources[h].state=RS_QUARANTINED;resource_quarantines++;return 0;
}
int irq_chain_remove(unsigned irq,irq_chain_handler_t handler,const char *owner)
{ CHECK(irq==11&&handler&&!strcmp(owner,"ac97"));irq_removals++;return 0; }
#include "../../../src/kernel/audio/stream.c"
#include "../../../src/kernel/audio/mixer.c"
#include "../../../src/kernel/drivers/ac97.c"
#include "../../../src/kernel/audio/audio_dev.c"

struct mock_hw {
    uint64_t tick;
    uint64_t busy_until, stop_after;
    uint16_t codec[128], posted_reg, posted_value;
    uint8_t civ, piv, cr, lvi;
    uint16_t sr, remaining;
    bool busy, posted, stuck, reject, stop_stuck;
    bool reset_stuck;
    bool any_pcm; /* the service publishes mixed or silent periods, not test_bdl's marker */
    unsigned post_delay;
    unsigned writes, lvi_writes;
    struct ac97_device *device;
};
static uint32_t mock_read(void *cookie, uint16_t port, unsigned width)
{
    struct mock_hw *h=cookie; (void)width;
    if (port >= 0x100 && port < 0x200) return h->codec[(port-0x100)/2];
    unsigned off=port-0x200;
    if(off==0x34) {
        if(h->busy || h->tick < h->busy_until)return 1;
        if(h->posted) { h->codec[h->posted_reg/2]=h->posted_value;h->posted=false; }
        return 0;
    }
    if(off==0x30)return 0x100;
    if(off==0x14)return h->civ;
    if(off==0x1a)return h->piv;
    if(off==0x16) {
        if(!h->cr && !h->stop_stuck && h->tick >= h->stop_after)h->sr|=AC97_DCH;
        return h->sr;
    }
    if(off==0x18)return h->remaining;
    if(off==0x1b)return h->cr;
    return 0;
}
static void mock_write(void *cookie, uint16_t port, uint32_t value, unsigned width)
{
    struct mock_hw *h=cookie; (void)width;h->writes++;
    if(port>=0x100 && port<0x200) {
        h->posted=true;h->posted_reg=port-0x100;h->posted_value=h->reject ? value^1u : value;
        h->busy_until=h->tick+h->post_delay;return;
    }
    unsigned off=port-0x200;
    if(off==0x15) {
        unsigned slot=value;
        CHECK(h->device->bdl[slot].length==((1u<<31)|512));
        if(!h->any_pcm)CHECK(h->device->pcm[slot][0].left==123);
        h->lvi=value;h->lvi_writes++;
    }
    if(off==0x16) { CHECK(!(value&~0x1cu));h->sr&=~value; }
    if(off==0x1b) {
        if(value&2) { CHECK(!(h->cr&1));h->civ=h->piv=0;h->remaining=0;h->sr=AC97_DCH;h->cr=h->reset_stuck?2:0; }
        else { h->cr=value;if(!(value&1)&&!h->stop_stuck&&h->tick>=h->stop_after)h->sr|=AC97_DCH; }
    }
}
static uint64_t mock_now(void *cookie) { return ((struct mock_hw *)cookie)->tick; }
static void mock_sleep(void *cookie,uint32_t ms) { ((struct mock_hw *)cookie)->tick+=ms; }
static struct mock_hw hw;
static void hardware_setup(struct ac97_device *d)
{
    memset(d,0,sizeof(*d));memset(&hw,0,sizeof(hw));hw.device=d;
    d->nam=0x100;d->nabm=0x200;d->pcm_unity=0x0808;
    d->io=(struct ac97_io){&hw,mock_read,mock_write,mock_now,mock_sleep};
    hw.sr=AC97_DCH;hw.codec[0x7c/2]=0x8384;hw.codec[0x7e/2]=0x7600;
    hw.codec[0x26/2]=0xf;hw.codec[0x28/2]=1;hw.codec[0x2a/2]=1;hw.codec[0x2c/2]=48000;
    hw.codec[2/2]=0x8000;hw.codec[0x18/2]=0x8808;
}
static void test_codec(void)
{
    struct ac97_device d;hardware_setup(&d);
    CHECK(!ac97_codec_prepare(&d));
    CHECK(d.codec_id==0x83847600 && hw.codec[2/2]==0 && hw.codec[0x18/2]==0x0808);
    CHECK(hw.codec[0x2a/2]==0 && !hw.posted);
    CHECK(!ac97_set_mute(&d,true) && hw.codec[2/2]==0x8000 && hw.codec[0x18/2]==0x8808);
    CHECK(!ac97_codec_restore(&d) && hw.codec[0x18/2]==0x8808 && hw.codec[0x2a/2]==1);
    hardware_setup(&d);d.pcm_unity=0;CHECK(!ac97_codec_prepare(&d) && hw.codec[0x18/2]==0);
    CHECK(!ac97_set_mute(&d,true) && hw.codec[0x18/2]==0x8000);
    struct ac97_device layout={.pci={.device=0x2415,.bar={0x1001,0x2001}}};
    CHECK(controller_layout(&layout,0) && layout.nam_bytes==256 && layout.nabm_bytes==64 && layout.pcm_unity==0x0808);
    CHECK(controller_layout(&layout,CBI_F_SMBIOS_QEMU) && layout.nam_bytes==1024 && layout.nabm_bytes==256 && !layout.pcm_unity);
    layout.pci.bar[0]=0x1101;CHECK(controller_layout(&layout,0) && !controller_layout(&layout,CBI_F_SMBIOS_QEMU));
    layout.pci.bar[0]=0x1003;CHECK(!controller_layout(&layout,0));
    layout.pci.bar[0]=0x10001;CHECK(!controller_layout(&layout,0));
    hardware_setup(&d);hw.busy=true;unsigned writes=hw.writes;uint16_t value;
    CHECK(ac97_codec_read(&d,2,&value,500)==-ETIMEDOUT && hw.tick==100 && hw.writes==writes);
    hardware_setup(&d);hw.busy_until=30;hw.post_delay=90;
    CHECK(ac97_codec_write(&d,2,0,500)==-ETIMEDOUT && hw.tick==100 && hw.writes==1);
    hardware_setup(&d);hw.busy=true;d.activation_deadline=50;
    CHECK(ac97_codec_prepare(&d)==-ETIMEDOUT && hw.tick==50 && !hw.writes);
    hardware_setup(&d);hw.codec[0x2c/2]=44100;
    CHECK(ac97_codec_prepare(&d)==-EIO && !d.codec_changed && !d.quarantine);
    hardware_setup(&d);hw.reject=true;
    CHECK(ac97_codec_prepare(&d)==-EIO && d.quarantine);
    puts("audio codec: posted CAS/readback, 100ms timeout without writes, rollback/mute PASS");
}
static void test_bdl(void)
{
    struct ac97_device d;hardware_setup(&d);
    struct ac97_bd bdl[32]={0};struct audio_frame buffers[32][256];
    memset(buffers,0xa5,sizeof(buffers));d.bdl=bdl;d.ready=true;
    for(unsigned i=0;i<32;i++)d.pcm[i]=buffers[i];
    struct audio_frame block[256];for(unsigned i=0;i<256;i++)block[i]=(struct audio_frame){123,-123};
    for(unsigned i=0;i<8;i++)CHECK(!ac97_publish(&d,block));
    CHECK(hw.lvi==7 && hw.lvi_writes==8); /* the lead policy lives in the service; the driver limits the ring (31) */
    CHECK(!ac97_start(&d));hw.sr=0;hw.civ=0;hw.piv=1;hw.remaining=512;
    for(unsigned absolute=1;absolute<100;absolute++) {
        hw.civ=absolute%32;hw.piv=(hw.civ+1)%32;hw.remaining=512;hw.tick++;
        CHECK(!ac97_progress(&d) && d.retired==absolute);
        struct audio_frame current= buffers[hw.civ][0],prefetch=buffers[hw.piv][0];
        CHECK(!ac97_publish(&d,block));
        CHECK(!memcmp(&current,&buffers[hw.civ][0],sizeof(current)) && !memcmp(&prefetch,&buffers[hw.piv][0],sizeof(prefetch)));
    }
    /* Ring limit: 31 outstanding periods, then -EAGAIN (idle-tail cushion of 28 fits). */
    for(unsigned i=0;i<23;i++)CHECK(!ac97_publish(&d,block));
    CHECK(d.published-d.retired==31 && ac97_publish(&d,block)==-EAGAIN);
    uint64_t fetched=d.fetched;
    CHECK(!ac97_stop(&d) && d.fetched==fetched);
    d.frame_base=1000000;
    for(unsigned i=0;i<8;i++)CHECK(!ac97_publish(&d,block));
    CHECK(!ac97_start(&d));hw.sr=0;hw.piv=1;hw.remaining=512;
    CHECK(!ac97_progress(&d) && d.fetched==fetched);
    hw.remaining=384;CHECK(!ac97_progress(&d) && d.fetched==fetched+64);
    hw.sr=AC97_FIFOE;CHECK(!ac97_progress(&d) && d.fifo_errors==1 && !(hw.sr&AC97_FIFOE));
    hw.civ=(hw.civ+10)%32;CHECK(ac97_progress(&d)==-EIO);
    hardware_setup(&d);d.running=true;hw.cr=1;hw.sr=0;hw.stop_stuck=true;
    uint32_t canary[4]={1,2,3,4};d.bdl=(struct ac97_bd *)canary;
    CHECK(ac97_stop(&d)==-EIO && d.quarantine && hw.tick==500 && canary[0]==1 && canary[3]==4);
    CHECK(hw.cr==0); /* no reset/BDL overwrite after failed proof */
    hardware_setup(&d);d.running=true;hw.cr=1;hw.sr=0;hw.stop_after=400;hw.reset_stuck=true;d.bdl=(struct ac97_bd *)canary;
    CHECK(ac97_stop(&d)==-EIO && d.quarantine && hw.tick==500 && canary[0]==1);
    puts("audio DMA: 3 wraps, prefetched/current canaries, release-before-LVI, W1C, ambiguity and failed-stop retention PASS");
}
static void test_formats(void)
{
    audio.ready=true;audio.master.left=audio.master.right=100;
    for(unsigned format=1;format<=2;format++)for(unsigned channels=1;channels<=2;channels++) {
        struct audio_stream *s;CHECK(!audio_stream_open(&s));
        struct ciuki_audio_format f={1,32,48000,format,channels,0,{0}};
        CHECK(!audio_stream_format(s,&f));
        uint8_t bytes[8]={0,255,128,128,0,128,255,127};
        if(format==2) { bytes[0]=0;bytes[1]=128;bytes[2]=255;bytes[3]=127; }
        unsigned frame=channels*(format==1?1:2);
        CHECK(audio_stream_enqueue(s,bytes,frame)==(int)frame);
        struct audio_frame out;audio_mix(&out,1);
        CHECK(out.left==-32768 && out.right==(channels==1?-32768:format==1?32512:32767));
        CHECK(s->accepted==1&&s->consumed==1);
        f.output_rate=0;
        CHECK(audio_stream_format(s,&f)==-EBUSY);
        audio_stream_reset(s);CHECK(!s->queued&&!s->phase);audio_stream_close(s);
    }
    for(unsigned rate=8000;rate<=48000;rate++) {
        struct audio_stream *s;CHECK(!audio_stream_open(&s));
        s->rate=rate;s->draining=true;s->drain_goal=64;
        struct audio_frame source[64];for(unsigned i=0;i<64;i++)source[i]=(struct audio_frame){(int16_t)(i*200-6000),(int16_t)(6000-i*200)};
        CHECK(audio_stream_enqueue(s,source,sizeof(source))==sizeof(source));
        struct audio_frame out[256];unsigned phase=0,index=0;
        audio_mix(out,256);
        for(unsigned i=0;i<256;i++) {
            if(index<64) {
                int a=source[index].left,b=source[index<63?index+1:index].left;
                CHECK(out[i].left==a+(b-a)*(int)phase/48000);
                phase+=rate;if(phase>=48000){phase-=48000;index++;}
            } else CHECK(!out[i].left&&!out[i].right);
        }
        CHECK(s->phase==phase);audio_stream_close(s);
    }
    struct audio_stream *s;CHECK(!audio_stream_open(&s));s->rate=11025;
    struct audio_frame source[128];for(unsigned i=0;i<128;i++)source[i]=(struct audio_frame){(int16_t)(i*97),-(int16_t)(i*97)};
    CHECK(audio_stream_enqueue(s,source,sizeof(source))==sizeof(source));
    struct audio_frame whole[256],split[256];audio_mix(whole,256);audio_stream_reset(s);
    CHECK(audio_stream_enqueue(s,source,sizeof(source))==sizeof(source));audio_mix(split,17);audio_mix(split+17,239);
    CHECK(!memcmp(whole,split,sizeof(whole)));audio_stream_close(s);
    CHECK(!audio_stream_open(&s));s->rate=8000;s->phase=40000;
    struct audio_frame extrema[2]={{-32768,32767},{32767,-32768}},interpolated;
    CHECK(audio_stream_enqueue(s,extrema,sizeof(extrema))==sizeof(extrema));audio_mix(&interpolated,1);
    CHECK(interpolated.left==21844 && interpolated.right==-21845);audio_stream_close(s);
    struct audio_stream *all[8];for(unsigned i=0;i<8;i++)CHECK(!audio_stream_open(&all[i]));
    struct audio_stream *extra;CHECK(audio_stream_open(&extra)==-ENOMEM);
    struct audio_frame positive={30000,30000},negative={-30000,-30000},out;
    CHECK(audio_stream_enqueue(all[0],&positive,4)==4&&audio_stream_enqueue(all[1],&positive,4)==4);
    audio.master.left=audio.master.right=50;audio_mix(&out,1);CHECK(out.left==30000);
    audio.master.left=audio.master.right=100;
    CHECK(audio_stream_enqueue(all[0],&positive,4)==4&&audio_stream_enqueue(all[1],&positive,4)==4);
    audio_mix(&out,1);CHECK(out.left==32767);
    CHECK(audio_stream_enqueue(all[0],&negative,4)==4&&audio_stream_enqueue(all[1],&negative,4)==4);
    audio_mix(&out,1);CHECK(out.left==-32768);
    CHECK(audio_stream_enqueue(all[0],&positive,4)==4&&audio_stream_enqueue(all[1],&negative,4)==4);
    audio_mix(&out,1);CHECK(out.left==0);
    for(unsigned i=0;i<8;i++)audio_stream_close(all[i]);
    CHECK(!audio.commitment_pages);
    for(int failure=0;failure<3;failure++) {
        unsigned before=pages_used,heap_before=heap_blocks;
        alloc_fail=failure;CHECK(audio_stream_open(&s)==-ENOMEM);alloc_fail=-1;
        CHECK(pages_used==before && heap_blocks==heap_before && !audio.commitment_pages);
    }
    puts("audio converter: every integer rate 8000..48000, all formats, mono/stereo, sum/master/clipping and split phase PASS");
}
static void test_shutdown(void)
{
    for(unsigned fault=0;fault<2;fault++) {
        unsigned before=pages_used;hardware_setup(&ac97);
        resource_releases=resource_quarantines=irq_removals=0;
        for(unsigned i=0;i<12;i++) {
            ac97.claims[i]=i;ac97.claim_generation[i]=100+i;
            audio_resources[i]=(struct resource){.generation=100+i,.state=RS_CLAIMED};
        }
        ac97.bdl_phys=pmm_alloc();CHECK(ac97.bdl_phys);ac97.bdl=P2V(ac97.bdl_phys);
        for(unsigned i=0;i<8;i++){ac97.payload_pages[i]=pmm_alloc();CHECK(ac97.payload_pages[i]);}
        audio.commitment_pages=9;ac97.ready=true;
        if(fault) {
            for(unsigned i=0;i<12;i++)CHECK(!registry_activate(i,100+i));
            ac97.touched=ac97.running=ac97.irq_attached=true;ac97.pci.irq_line=11;
            hw.cr=1;hw.sr=0;hw.stop_stuck=true;
        }
        uint32_t canary=ac97.bdl[0].address;
        ac97_shutdown();
        if(fault) {
            CHECK(ac97.quarantine && audio.quarantined && resource_quarantines==12 && !resource_releases);
            CHECK(pages_used==before+9 && audio.commitment_pages==9 && ac97.bdl[0].address==canary && irq_removals==1);
            hw.stop_stuck=false;ac97_shutdown();
            CHECK(hw.cr==0 && pages_used==before+9 && !resource_releases && ac97.bdl[0].address==canary);
            /* Host teardown owns this fake device; production retained every page. */
            pmm_free(ac97.bdl_phys);for(unsigned i=0;i<8;i++)pmm_free(ac97.payload_pages[i]);
            audio.commitment_pages=0;audio.quarantined=false;
        } else {
            CHECK(resource_releases==12 && !resource_quarantines && pages_used==before && !audio.commitment_pages);
            CHECK(!hw.writes); /* refused admission before any hardware takeover */
        }
    }
    memset(&ac97,0,sizeof(ac97));
    puts("audio shutdown: preactivation claims released; unproved stop retains all nine DMA pages/claims; no quarantine reset PASS");
}
/* f4-05 hooks (weak in audio_dev.c; the VM sources are not linked here). */
static bool vm_active_hook, vm_attached_hook;
bool audio_vm_active(void) { return vm_active_hook; }
bool audio_vm_attached(void) { return vm_attached_hook; }
/* The idle tail with a DOS VM attached keeps AUDIO_LEAD periods. A worker that
 * runs late can find the DMA on the last valid period (published-retired 1)
 * or halted at it; ac97_publish refuses the CIV/PIV slots. Either way the tail
 * must end as a normal transport stop, never as a backend failure, also when
 * a VM sound arrives in that service, and the next sound restarts it
 * (QEMU campaign 13, image 22320ccc: the backend died between VMPLAY stages
 * 4 and 5, after_pages 17 -> 8, no OPL3 event consumed). */
static void transport_restart(void)
{
    vm_active_hook=true;
    for(unsigned i=0;i<AUDIO_LEAD;i++) { g_ticks++;audio_service(); }
    CHECK(audio.running && ac97.running && ac97.published==AUDIO_LEAD && !audio.error && audio.ready);
    hw.sr=0;hw.civ=0;hw.piv=1;hw.remaining=512;
    vm_active_hook=false;g_ticks++;audio_service(); /* the VM goes idle: the shallow tail starts */
    CHECK(idle_since && ac97.published-ac97.retired==AUDIO_LEAD);
}
static void test_idle_tail_drain(void)
{
    static struct ac97_bd bdl[32];static struct audio_frame buffers[32][256];
    hardware_setup(&ac97);hw.any_pcm=true;
    ac97.bdl=bdl;ac97.ready=true;for(unsigned i=0;i<32;i++)ac97.pcm[i]=buffers[i];
    for(unsigned i=0;i<12;i++)ac97.claims[i]=-1;
    audio.ready=true;audio.error=0;audio.running=false;audio.output=0;idle_since=0;
    vm_attached_hook=true;g_ticks=1000;
    transport_restart();
    /* Late worker: seven periods played, the DMA still on the last one. */
    hw.civ=7;hw.piv=8;hw.remaining=300;hw.tick+=40;g_ticks+=40;
    audio_service();
    printf("idle tail drained: error=%d ready=%u running=%u published=%llu halts=%llu\n",audio.error,audio.ready,audio.running,
           (unsigned long long)ac97.published,(unsigned long long)audio.halts);
    CHECK(!audio.error && audio.ready && !audio.running && !ac97.running && !ac97.published && ac97.frame_base==audio.output);
    transport_restart();
    /* Halted at the last period (underrun) when a VM sound arrives in the same service. */
    hw.civ=7;hw.piv=8;hw.remaining=0;hw.sr=AC97_DCH;hw.tick+=50;g_ticks+=50;
    vm_active_hook=true;audio_service();
    printf("idle tail halted, source arriving: error=%d ready=%u running=%u published=%llu halts=%llu\n",audio.error,audio.ready,
           audio.running,(unsigned long long)ac97.published,(unsigned long long)audio.halts);
    CHECK(!audio.error && audio.ready && !audio.running && ac97.published==1 && !audio.halts); /* restarting: first silent period */
    for(unsigned i=1;i<AUDIO_LEAD;i++) { g_ticks++;audio_service(); }
    CHECK(audio.running && ac97.running && !audio.error && !audio.halts && !audio.deadlines);
    vm_active_hook=vm_attached_hook=false;audio.ready=audio.running=false;idle_since=0;
    memset(&ac97,0,sizeof(ac97));
    puts("audio idle tail: drained or halted VM tail stops the transport, a VM sound restarts it, no backend failure PASS");
}
static struct audio_stream *waiting_stream;
static void signal_waiter(void) { client_thread->interrupted=true; }
static void reset_waiter(void) { audio_stream_reset(waiting_stream); }
static void fail_waiter(void) { audio_fail(-EIO); }
static void finish_drain(void)
{
    struct audio_frame output;
    CHECK(waiting_stream->draining && waiting_stream->queued==1);
    audio_mix(&output,1);audio.played_output=waiting_stream->last_output;
    on_schedule=0;
}
static int ctl(int fd,unsigned op,void *record,unsigned bytes)
{
    CHECK(!ua_write(client->memory,USER2,record,bytes));
    int r=call(CIUKI_SYS_DEVCTL,fd,op,USER2,bytes,0);
    CHECK(!ua_read(client->memory,record,USER2,bytes));return r;
}
static void test_stream_syscalls(void)
{
    audio.ready=false;CHECK(file_open(client,"/dev/audio",O_WRONLY,0)==-ENODEV);
    audio.ready=true;audio.error=0;
    CHECK(file_open(client,"/dev/audio",O_RDONLY,0)==-EACCES);
    CHECK(file_open(client,"/dev/mixer",O_RDWR,0)==-EACCES);
    int fd=file_open(client,"/dev/audio",O_WRONLY|O_NONBLOCK,0);CHECK(fd>=0);
    waiting_stream=file_fd(client,fd)->audio;
    struct ciuki_audio_format f={1,32,11025,CIUKI_AUDIO_U8,1,0,{0}};CHECK(!ctl(fd,AUDIO_SET_FORMAT,&f,sizeof(f))&&f.output_rate==48000);
    uint8_t bytes[2048];memset(bytes,128,sizeof(bytes));CHECK(!ua_write(client->memory,USER,bytes,sizeof(bytes)));
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,2048,0,0)==1024);
    bytes[0]=255;CHECK(!ua_write(client->memory,USER,bytes,1));
    CHECK(audio_frame_at(waiting_stream,waiting_stream->head)->left==0);
    bytes[0]=128;CHECK(!ua_write(client->memory,USER,bytes,1));
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,2048,0,0)==1024);
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,1,0,0)==-EAGAIN);
    struct ua_pin *pins_before=client->memory->pins;
    CHECK(call(CIUKI_SYS_WRITE,fd,UINT32_MAX,1,0,0)==-EFAULT);
    CHECK(waiting_stream->accepted==2048 && client->memory->pins==pins_before);
    ac97.published=8;ac97.generation_fetched=128;
    struct ciuki_audio_status status={.version=1,.size=112};
    CHECK(!ctl(fd,AUDIO_GET_STATUS,&status,sizeof(status)));
    CHECK(status.queued_frames==2048 && status.transport_frames==1920 && !status.reserved);
    CHECK(!ctl(fd,AUDIO_GET_STATUS,&status,sizeof(status))); /* output field is not reserved input */
    ac97.published=ac97.generation_fetched=0;
    CHECK(call(CIUKI_SYS_WRITE,fd,UINT32_MAX,0,0,0)==0);
    CHECK(call(CIUKI_SYS_LSEEK64,fd,0,0,SEEK_SET,USER2)==-ESPIPE);
    CHECK(!file_fcntl(client,fd,F_SETFL,0));on_schedule=signal_waiter;
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,1,0,0)==-EINTR);on_schedule=0;client_thread->interrupted=false;
    on_schedule=reset_waiter;CHECK(call(CIUKI_SYS_WRITE,fd,USER,1,0,0)==-EINTR);on_schedule=0;
    struct ciuki_audio_command cmd={1,16,{0}};CHECK(!ctl(fd,AUDIO_RESET,&cmd,sizeof(cmd)));
    CHECK(audio_stream_enqueue(waiting_stream,bytes,2047)==2047);
    on_schedule=signal_waiter;CHECK(call(CIUKI_SYS_WRITE,fd,USER,4,0,0)==1);on_schedule=0;client_thread->interrupted=false;
    on_schedule=fail_waiter;CHECK(call(CIUKI_SYS_WRITE,fd,USER,1,0,0)==-EIO);on_schedule=0;audio.ready=true;audio.error=0;
    int duplicate=file_dup(client,fd,0,false,0);CHECK(file_fd(client,duplicate)->audio==waiting_stream);
    struct process *child;CHECK(!proc_prepare(client,&child));
    struct ciuki_spawn_fd map={fd,9};CHECK(!proc_inherit(child,client,&map,1));
    CHECK(file_fd(child,9)->audio==waiting_stream);
    CHECK(!file_close(client,fd));CHECK(!file_close(client,duplicate));CHECK(audio.commitment_pages==2);
    proc_discard(child);CHECK(!audio.commitment_pages);
    fd=file_open(client,"/dev/audio",O_WRONLY,0);CHECK(fd>=0);
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,3,0,0)==-EINVAL);
    CHECK(file_fd(client,fd)->audio->accepted==0);
    struct ciuki_audio_gain gain={2,24,100,100,{0}};CHECK(ctl(fd,AUDIO_SET_GAIN,&gain,sizeof(gain))==-EOPNOTSUPP);
    gain.version=1;gain.reserved[0]=1;CHECK(ctl(fd,AUDIO_SET_GAIN,&gain,sizeof(gain))==-EINVAL);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,0xffffffff,UINT32_MAX,24,0)==-ENOTTY);
    CHECK(!ctl(fd,AUDIO_RESET,&cmd,sizeof(cmd)));
    waiting_stream=file_fd(client,fd)->audio;
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,4,0,0)==4);
    audio.caps.drain_ms=20;uint64_t before=g_ticks;on_schedule=finish_drain;
    CHECK(!ctl(fd,AUDIO_DRAIN,&cmd,sizeof(cmd)) && g_ticks>=before+20);
    CHECK(!waiting_stream->queued && !waiting_stream->started && waiting_stream->configured);
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,4,0,0)==4);on_schedule=signal_waiter;
    CHECK(ctl(fd,AUDIO_DRAIN,&cmd,sizeof(cmd))==-EINTR && !waiting_stream->draining);
    on_schedule=0;client_thread->interrupted=false;
    CHECK(!file_close(client,fd));
    puts("audio fds: copied EFAULT, short/EAGAIN/EINTR/partial/EIO, reset generation, dup/inheritance/final close and quota PASS");
}
static void test_persistence(void)
{
    CHECK(!make_dir("/system"));
    kmutex_init(&control_lock);saved_sequence=0;audio_settings_load(&vfs);
    CHECK(audio.master.left==50&&audio.master.right==50&&!audio.master.mute);
    int mixer=file_open(client,"/dev/mixer",O_RDONLY,0);CHECK(mixer>=0);
    struct ciuki_mixer_master m={1,32,60,40,1,0,0,0};
    CHECK(ctl(mixer,MIXER_SET_MASTER,&m,32)==-EACCES);
    file_fd(client,mixer)->mixer_control=true; /* bootstrap authority is tested separately below */
    hardware_setup(&ac97);audio.ready=true;
    CHECK(!ctl(mixer,MIXER_SET_MASTER,&m,32)&&m.save_state==1);
    m=(struct ciuki_mixer_master){1,32,25,75,0,0,0,0};CHECK(!ctl(mixer,MIXER_SET_MASTER,&m,32));
    saved_sequence=0;audio_settings_load(&vfs);CHECK(audio.master.left==25&&audio.master.right==75&&!audio.master.mute);
    volumes[0].readonly=true;m=(struct ciuki_mixer_master){1,32,10,20,1,0,0,0};
    CHECK(!ctl(mixer,MIXER_SET_MASTER,&m,32)&&m.save_state==2&&m.save_error==-EROFS);
    CHECK(audio.master.left==10&&audio.master.mute);
    volumes[0].readonly=false;saved_sequence=0;audio_settings_load(&vfs);
    CHECK(audio.master.left==25&&!audio.master.mute);
    CHECK(!file_close(client,mixer));
    struct process *supervisor=proc_supervisor();supervisor->cwd=space->root;
    int delegated=audio_mixer_delegate(supervisor,client);CHECK(delegated>=0);
    CHECK(!file_fd(client,delegated)->object.inheritable);
    struct process *child;CHECK(!proc_prepare(client,&child));
    struct ciuki_spawn_fd map={delegated,9};CHECK(proc_inherit(child,client,&map,1)==-EBADF || proc_fd_validate(client,&map,1)==-EPERM);
    proc_discard(child);CHECK(!file_close(client,delegated));supervisor->cwd=0;
    puts("audio settings: defaults, public refusal/delegation, alternating durable records and applied-unsaved/last saved restoration PASS");
}
int main(int argc,char **argv)
{
    CHECK(argc==3);ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);
    controller.state=T_RUNNING;g_current=&controller;proc_init();
    CHECK(!fake_open(&disk[0],argv[1])&&!fake_open(&disk[1],argv[2]));
    disk[0].undo_enabled=disk[1].undo_enabled=true;CHECK(!cache_init(&cache,0));vfs_init(&vfs);
    for(unsigned i=0;i<2;i++){CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true));CHECK(!vfs_attach(&vfs,i+2,&volumes[i]));}
    CHECK(!files_attach(&vfs,&space));client_thread=make_process(proc_supervisor(),&client);g_current=client_thread->task;g_current->state=T_RUNNING;
    client->cwd=space->root;px_retain(client->cwd);client->cwd_retain=px_retain;client->cwd_release=px_release;
    test_codec();test_bdl();test_formats();test_shutdown();test_idle_tail_drain();test_stream_syscalls();test_persistence();
    all_closed();stop_collect(client);for(unsigned i=0;i<2;i++)CHECK(!vfs_detach(&vfs,i+2));vfs_destroy(&vfs);cache_destroy(&cache);
    for(unsigned i=0;i<2;i++){fake_reset(&disk[i]);fake_close(&disk[i]);}
    CHECK(!pages_used&&!audio.commitment_pages);printf("audio production: %u checks PASS\n",checks);free(ram);return 0;
}
