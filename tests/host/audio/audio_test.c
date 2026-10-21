/* Production fd/path/syscall, converter, persistence and transport with fake
 * RAM, disk, time and register boundary. SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#undef FD_TABLE_STACK_TEST
#include <stdarg.h>
#include <ciuki/ac97.h>
#include <ciuki/registry_value.h>
#include <ciuki/timing.h>
static uint32_t read_eflags(void) { return host_flags; }
static bool vm_active_hook;
static bool fullscreen_hook;
struct ciuki_boot_info g_boot;
uint64_t g_tsc_per_ms = 1000;
static uint8_t inb(uint16_t p) { (void)p; return 0; }
static uint32_t inl(uint16_t p) { (void)p; return 0; }
static void outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
static void outl(uint16_t p, uint32_t v) { (void)p; (void)v; }
static uint64_t modeled_cycles;
static unsigned modeled_mix_us;
uint64_t ktime_cycles(void) { return g_ticks * 1000 + modeled_cycles; }
static char audio_log[256]; /* match the production klog record capacity */
static bool capture_ec_records;
static char ec_records[64][256];
static unsigned ec_record_count;
static unsigned audio_log_count;
static unsigned late_grow_logs;
static unsigned backend_failure_logs;
static char backend_failure_record[256];
static unsigned analog_warning_logs, response_warning_logs;
static unsigned cold_reset_logs, qemu_reset_logs;
static bool slow_playback_log;
static bool serial_is_buffered;
void klog(const char *fmt, ...)
{
    va_list a; va_start(a, fmt); vsnprintf(audio_log,sizeof(audio_log),fmt,a); va_end(a);
    puts(audio_log); audio_log_count++;
    if (strstr(audio_log, "transport late-grow ")) late_grow_logs++;
    if (capture_ec_records && (!strncmp(audio_log,"[smbios]",8) || !strncmp(audio_log,"[ec]",4))) {
        CHECK(ec_record_count < sizeof(ec_records)/sizeof(ec_records[0]));
        memcpy(ec_records[ec_record_count++],audio_log,sizeof(audio_log));
    }
    if(strstr(audio_log,"backend failure ")) {
        backend_failure_logs++;strcpy(backend_failure_record,audio_log);
    }
    /* A physical polled UART record can take 84 ms (T23 transcript).
     * Let the fake DMA clock observe that delay on the next worker call. */
    if (slow_playback_log && !serial_is_buffered) g_ticks += 84;
    if(strstr(audio_log,"result=analog_not_ready"))analog_warning_logs++;
    if(strstr(audio_log,"result=analog_warning_accepted"))response_warning_logs++;
    if(strstr(audio_log,"reset=cold "))cold_reset_logs++;
    if(strstr(audio_log,"reset=qemu_model "))qemu_reset_logs++;
}
void klog_periodic(const char *fmt, ...)
{
    char line[256]; va_list a; va_start(a,fmt); vsnprintf(line,sizeof(line),fmt,a); va_end(a);
    klog("%s",line);
}
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
static struct resource audio_resources[64];
static unsigned resource_releases, resource_quarantines, irq_removals;
const struct resource *registry_get(unsigned i) { CHECK(i<64);return &audio_resources[i]; }
int registry_activate(int h,gen_t generation)
{
    CHECK(h>=0&&h<64&&audio_resources[h].generation==generation&&audio_resources[h].state==RS_CLAIMED);
    audio_resources[h].state=RS_ACTIVE;return 0;
}
int registry_quiesce(int h,gen_t generation,registry_idle_proof_fn proof)
{
    CHECK(h>=0&&h<64&&audio_resources[h].generation==generation&&audio_resources[h].state==RS_ACTIVE);
    audio_resources[h].state=RS_QUIESCING;CHECK(proof(h,generation));return 0;
}
int registry_release(int h,uint32_t generation)
{
    CHECK(h>=0&&h<64&&audio_resources[h].generation==generation&&audio_resources[h].state==RS_QUIESCING);
    audio_resources[h].state=RS_RELEASED;resource_releases++;return 0;
}
int registry_quarantine(int h,gen_t generation)
{
    CHECK(h>=0&&h<64&&audio_resources[h].generation==generation);
    audio_resources[h].state=RS_QUARANTINED;resource_quarantines++;return 0;
}
int irq_chain_remove(unsigned irq,irq_chain_handler_t handler,const char *owner)
{ CHECK(irq<16&&handler&&owner);irq_removals++;return 0; }
static struct pci_func second_pci[8];
static unsigned second_pci_count, second_claim_next, second_irq_adds, second_pic_masks, second_pic_unmasks;
static bool second_pic_masked = true;
static irq_handler_t second_isa_handler;
static int second_claim_failure=-1;
unsigned pci_count(void) { return second_pci_count; }
const struct pci_func *pci_get(unsigned i) { CHECK(i<second_pci_count); return &second_pci[i]; }
int registry_claim(enum res_type type,uint32_t start,uint32_t end,const char *owner,bool share)
{
    if (second_claim_failure>=0 && second_claim_next>=(unsigned)second_claim_failure) return -EBUSY;
    CHECK(second_claim_next<64 && end>start && owner);
    unsigned h=second_claim_next++;
    audio_resources[h]=(struct resource){type,start,end,owner,100+h,RS_CLAIMED,share};
    return h;
}
int irq_chain_add(unsigned irq,irq_chain_handler_t fn,const char *owner)
{ CHECK(irq<16 && fn && owner); second_irq_adds++; return 0; }
void irq_set_handler(unsigned irq,irq_handler_t handler) { CHECK(irq==5); second_isa_handler=handler; }
void pic_mask(unsigned irq)
{ CHECK(irq==5); if (!second_pic_masked) { second_pic_masked=true; second_pic_masks++; } }
void pic_unmask(unsigned irq)
{ CHECK(irq==5 && second_isa_handler); if (second_pic_masked) { second_pic_masked=false; second_pic_unmasks++; } }
uint32_t pmm_alloc_contig(uint32_t pages)
{
    if (fail_alloc()) return 0;
    for (unsigned at=1;at+pages<=HOST_PAGES;at++) {
        unsigned i; for (i=0;i<pages&&!allocated[at+i];i++);
        if (i!=pages) continue;
        for (i=0;i<pages;i++) { allocated[at+i]=true; pages_used++; }
        return at*PAGE_SIZE;
    }
    return 0;
}
uint32_t pmm_alloc_dma_run(uint32_t pages,uint32_t alignment)
{
    if (fail_alloc()) return 0;
    for (unsigned at=alignment;at+pages<=4096;at+=alignment) {
        unsigned i; for (i=0;i<pages&&!allocated[at+i];i++);
        if (i!=pages) continue;
        for (i=0;i<pages;i++) { allocated[at+i]=true; pages_used++; }
        return at*PAGE_SIZE;
    }
    return 0;
}
#include "../../../src/kernel/audio/stream.c"
#include "../../../src/kernel/audio/mixer.c"
#include "../../../src/kernel/drivers/audio_pci.c"
#include "../../../src/kernel/drivers/ac97.c"
#include "../../../src/kernel/drivers/es137x.c"
#include "../../../src/kernel/drivers/sb16.c"
#include "../../../src/kernel/drivers/via82xx.c"
#include "../../../src/kernel/audio/backend.c"
#include "../../../src/kernel/audio/audio_dev.c"
bool display_fullscreen_active(void) { return fullscreen_hook; }
void serial_buffered(bool enabled) { serial_is_buffered=enabled; }

struct mock_hw {
    unsigned bad_civ_reads;
    uint64_t tick;
    uint64_t busy_until, stop_after;
    uint16_t codec[128], posted_reg, posted_value;
    uint8_t civ, piv, cr, lvi;
    uint16_t sr, remaining;
    uint64_t zero_until; /* adversarial IRQ handoff: reload on the next time step */
    uint64_t silence_words;
    int16_t completed[128];
    unsigned completed_count;
    bool busy, posted, stuck, reject, stop_stuck;
    bool reset_stuck;
    bool any_pcm; /* the service publishes mixed or silent periods, not test_bdl's marker */
    unsigned post_delay;
    unsigned writes, lvi_writes;
    uint32_t global_control, global_status;
    uint64_t ready_at, power_ready_at;
    unsigned ready_delay, power_delay, volume_writes[128];
    bool ready_stuck, cold_stuck, release_stuck, power_stuck, sticky_mute, rate_stuck;
    bool qemu_model; /* v9.2.0: GC_CR/GC_WR writes ignored, PCR always set */
    unsigned unready_codec_reads;
    bool codec_model, vra_enable_stuck, vra_disable_stuck;
    bool cas_owned, zero_reads, mixer_zero, master_stuck, reset_defaults;
    uint16_t read_timeout_reg;
    unsigned codec_resets, codec_reads;
    uint64_t analog_wait_started;
    bool ich3, start_stuck, exhausted_start, bdbar_stuck, run_clear_stuck;
    unsigned reset_delay, start_delay, resets, starts;
    uint64_t reset_at, start_at;
    uint32_t bdbar, pci_address;
    uint16_t pci_command, input_sr, mic_sr, sis_gate;
    uint8_t modem_cr[2];
    uint16_t modem_sr[2];
    struct { uint16_t port; unsigned width; uint32_t value; } trace[1024];
    unsigned trace_count;
    struct ac97_device *device;
};
static bool mock_primary_ready(struct mock_hw *h)
{
    return (h->qemu_model || (h->global_control & 2)) &&
           !h->ready_stuck && h->tick >= h->ready_at;
}
static uint32_t mock_read(void *cookie, uint16_t port, unsigned width)
{
    struct mock_hw *h=cookie; (void)width;
    if(port==0xcfc) { CHECK((h->pci_address&0xfc)==4 && width==4);return h->pci_command; }
    if(port>=0x400 && port<0x480) {
        unsigned off=port-0x400;
        if((off&15)==11)return h->modem_cr[off/16];
        if((off&15)==6)return h->modem_sr[off/16];
        return 0;
    }
    if (port >= 0x100 && port < 0x200) {
        CHECK(width==2 && h->cas_owned && !h->posted);
        h->cas_owned=false;h->codec_reads++;
        /* Physical defensive model; upstream QEMU does NOT gate NAM on
         * GC_CR/PCR. Keep that independent model divergence explicit. */
        if(!h->qemu_model && ((h->global_control&0x0e)!=2 || !mock_primary_ready(h))) {
            h->unready_codec_reads++;return 0xffff;
        }
        unsigned reg=port-0x100;
        if(reg==h->read_timeout_reg)h->global_status|=1u<<15;
        if(h->zero_reads || (h->mixer_zero && (reg==2 || reg==0x18 || reg==0x26)))return 0;
        if(h->master_stuck && reg==2)return 0x8000;
        if(reg==0x26) return (h->codec[reg/2]&0xff00) | (!h->power_stuck && h->tick>=h->power_ready_at ? 0xf : 0);
        return h->codec[reg/2];
    }
    unsigned off=ac97_offset(h->device,port-0x200);
    if(off==0x4c) return h->sis_gate;
    if(off==0x76) return AC97_DCH;
    if(off==0x34) {
        CHECK(width==1);
        if(h->busy || h->cas_owned || h->tick < h->busy_until)return 1;
        if(h->posted) {
            uint16_t reg=h->posted_reg, value=h->posted_value;
            if(reg==0) {
                h->codec_resets++;
                if(h->reset_defaults) {
                    h->codec[2/2]=h->codec[4/2]=0x8000;h->codec[0x18/2]=0x8808;
                    h->codec[0x26/2]=h->codec[0x2a/2]=0;h->codec[0x2c/2]=48000;
                    h->codec[0x5e/2]=0x80;h->codec[0x60/2]=1;h->codec[0x68/2]=0;
                }
                value=h->codec[0]; /* reset is a command; capabilities are RO */
            }
            if(reg==0x26) { value&=0xff00;h->power_ready_at=h->tick+h->power_delay;h->analog_wait_started=h->tick; }
            if(h->rate_stuck && reg==0x2c) value=h->codec[reg/2];
            if(h->codec_model && reg==0x2a) {
                if(!(h->codec[0x28/2]&1) || h->vra_enable_stuck) value&=~1u;
                if(h->vra_disable_stuck && !(value&1)) value|=1;
            }
            if(h->codec_model && reg==0x2c && !(h->codec[0x2a/2]&1)) value=h->codec[reg/2];
            if(reg==2 || reg==4 || reg==0x18 || reg==0x0a) {
                if(h->sticky_mute && !h->volume_writes[reg/2] && !(value&0x8000)) value|=0x8000;
                h->volume_writes[reg/2]++;
            }
            if(!h->zero_reads && !(h->mixer_zero && (reg==2 || reg==0x18 || reg==0x26)))h->codec[reg/2]=value;
            h->posted=false;
        }
        h->cas_owned=true;
        return 0;
    }
    if(off==0x2c)return h->global_control;
    if(off==0x30)return h->global_status | (mock_primary_ready(h) ? 0x100 : 0);
    if(off==0x14) { if(h->bad_civ_reads) { h->bad_civ_reads--; return 32; } return h->civ; }
    if(off==0x10)return h->bdbar;
    if(off==0x15)return h->lvi;
    if(off==6)return h->input_sr;
    if(off==0x26)return h->mic_sr;
    if(off==0x1a)return h->piv;
    if(off==0x16) {
        if(h->ich3 && (h->cr&1) && !h->start_stuck && h->tick>=h->start_at &&
           (!(h->codec[0x2a/2]&1) || h->codec[0x2c/2]==48000)) {
            h->sr&=~AC97_DCH;h->remaining=ac97_units(h->device);h->piv=1;
            if(h->exhausted_start) { h->sr|=AC97_DCH|AC97_CELV|AC97_LVBCI;h->civ=h->lvi;h->remaining=0; }
        }
        if(!h->cr && !h->stop_stuck && h->tick >= h->stop_after)h->sr|=AC97_DCH;
        return h->sr;
    }
    if(off==0x18)return h->tick < h->zero_until ? 0 : h->remaining;
    if(off==0x1b) {
        if((h->cr&2) && !h->reset_stuck && h->tick>=h->reset_at)h->cr=0;
        return h->cr;
    }
    return 0;
}
static void mock_write(void *cookie, uint16_t port, uint32_t value, unsigned width)
{
    struct mock_hw *h=cookie; (void)width;h->writes++;
    if(h->trace_count<1024) {
        unsigned n=h->trace_count++;h->trace[n].port=port;h->trace[n].width=width;h->trace[n].value=value;
    }
    if(port==0xcf8) { CHECK(width==4);h->pci_address=value;return; }
    if(port==0xcfc) {
        CHECK((h->pci_address&0xfc)==4 && width==2);
        if(h->ich3 && (value&4)) {
            CHECK(h->bdbar==h->device->bdl_phys && h->bdbar && h->lvi==1);
            for(unsigned i=0;i<2;i++)CHECK(h->device->bdl[i].address && h->device->bdl[i].length==((1u<<31)|ac97_units(h->device)));
        }
        h->pci_command=value;return;
    }
    if(port>=0x100 && port<0x200) {
        CHECK(width==2 && h->cas_owned && !h->posted);
        CHECK(h->qemu_model || ((h->global_control&0x0e)==2 && mock_primary_ready(h)));
        h->cas_owned=false;
        h->posted=true;h->posted_reg=port-0x100;h->posted_value=h->reject ? value^1u : value;
        h->busy_until=h->tick+h->post_delay;return;
    }
    unsigned off=ac97_offset(h->device,port-0x200);
    if(off==0x4c) { CHECK(width==2); h->sis_gate=value; return; }
    if(off==0x2c) {
        if(h->cold_stuck) return;
        if(h->ich3) CHECK(!(h->pci_command&4));
        if(h->qemu_model) {
            if(!(value&6))h->global_control=value&0x3f;
            return;
        }
        if(h->release_stuck && (value&2))return;
        h->global_control=value;
        if(!(value&2)) { h->global_status&=~0x100u;h->cr=0;h->sr=AC97_DCH; }
        else h->ready_at=h->tick+h->ready_delay;
    }
    if(off==0x30) { CHECK(value==(1u<<15));h->global_status&=~value; }
    if(off==0x10 && !h->bdbar_stuck)h->bdbar=value;
    if(off==0x15) {
        unsigned slot=value;
        CHECK(h->device->bdl[slot].length==((1u<<31)|ac97_units(h->device)));
        if(!h->any_pcm)CHECK(h->device->pcm[slot][0].left==123);
        h->lvi=value;h->lvi_writes++;
    }
    if(off==0x16) { CHECK(!(value&~0x1cu));h->sr&=~value; }
    if(off==0x1b) {
        if(value&2) {
            CHECK(!(h->cr&1));h->resets++;h->reset_at=h->tick+h->reset_delay;
            h->civ=h->piv=h->lvi=0;h->remaining=0;h->sr=AC97_DCH;h->bdbar=0;h->cr=2;
        }
        else {
            if(!value && h->run_clear_stuck)return;
            h->cr=value;
            if(value&1) {
                h->starts++;h->start_at=h->tick+h->start_delay;
                if(h->ich3) {
                    CHECK(h->pci_command&4);CHECK(h->bdbar==h->device->bdl_phys && h->bdbar);
                    CHECK(!h->device->ready); /* admission follows this proof */
                    CHECK((h->tick>=h->power_ready_at && !h->power_stuck) ||
                          (h->device->analog_warning && h->tick>=h->analog_wait_started+5000));
                    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<AUDIO_QUANTUM;j++)
                        CHECK(!h->device->pcm[i][j].left && !h->device->pcm[i][j].right);
                } else { h->sr&=~AC97_DCH;h->remaining=ac97_units(h->device);h->piv=(h->civ+1)%32; }
            }
            else if(!h->stop_stuck&&h->tick>=h->stop_after)h->sr|=AC97_DCH;
        }
    }
}
static uint64_t mock_now(void *cookie) { return ((struct mock_hw *)cookie)->tick; }
static void mock_sleep(void *cookie,uint32_t ms) { ((struct mock_hw *)cookie)->tick+=ms; }
static struct mock_hw hw;
/* ICH/QEMU word-count boundary: CIV stays on LVI at exhaustion, and BUP=0
 * outputs silence while RUN remains set. IRQ acknowledgement never clears
 * DCH/CELV. Unlike the old boot fake, this engine cannot run past LVI. */
static void mock_advance(unsigned words)
{
    while ((hw.cr & 1) && words) {
        if (hw.sr & AC97_DCH) { hw.silence_words += words; return; }
        CHECK(hw.remaining && hw.remaining <= ac97_units(hw.device));
        unsigned used = words < hw.remaining ? words : hw.remaining;
        hw.remaining -= used; words -= used;
        if (hw.remaining) continue;
        struct ac97_bd *bd = &hw.device->bdl[hw.civ];
        CHECK(!(bd->length & (1u << 30))); /* BUP=0, never repeat the last sample */
        if (hw.completed_count < 128) hw.completed[hw.completed_count] = hw.device->pcm[hw.civ][0].left;
        hw.completed_count++;
        if (bd->length & (1u << 31)) hw.sr |= AC97_BCIS;
        if (hw.civ == hw.lvi) hw.sr |= AC97_DCH | AC97_CELV | AC97_LVBCI;
        else {
            hw.civ = (hw.civ + 1) & 31; hw.piv = (hw.civ + 1) & 31;
            hw.remaining = hw.device->bdl[hw.civ].length & 0xffff;
            hw.sr &= ~AC97_CELV;
        }
    }
}
static void hardware_setup(struct ac97_device *d)
{
    memset(d,0,sizeof(*d));memset(&hw,0,sizeof(hw));hw.device=d;
    hw.read_timeout_reg=0xffff;
    d->nam=0x100;d->nabm=0x200;d->pcm_unity=0x0808;
    d->software_master=true;hw.global_control=2;
    d->raw_ext_control=d->raw_rate=0xffff;
    d->io=(struct ac97_io){&hw,mock_read,mock_write,mock_now,mock_sleep};
    hw.sr=AC97_DCH;hw.codec[0x7c/2]=0x8384;hw.codec[0x7e/2]=0x7600;
    hw.input_sr=hw.mic_sr=AC97_DCH;
    hw.codec[0x26/2]=0xf;hw.codec[0x28/2]=1;hw.codec[0x2a/2]=1;hw.codec[0x2c/2]=48000;
    hw.codec[2/2]=0x8000;hw.codec[0x18/2]=0x8808;
}
static void test_codec(void)
{
    audio.master=(struct ciuki_mixer_master){.left=100,.right=100};
    struct ac97_device d;hardware_setup(&d);
    CHECK(!ac97_codec_prepare(&d));
    CHECK(d.codec_id==0x83847600 && hw.codec[2/2]==0 && hw.codec[0x18/2]==0x0808);
    CHECK(hw.codec[0x2a/2]==1 && !hw.posted);
    CHECK(!ac97_set_mute(&d,true) && hw.codec[2/2]==0x8000 && hw.codec[0x18/2]==0x8808);
    CHECK(!ac97_codec_restore(&d) && hw.codec[0x18/2]==0x8808 && hw.codec[0x2a/2]==1);
    hardware_setup(&d);d.pcm_unity=0;CHECK(!ac97_codec_prepare(&d) && hw.codec[0x18/2]==0);
    CHECK(!ac97_set_mute(&d,true) && hw.codec[0x18/2]==0x8000);
    struct ac97_device layout={.pci={.vendor=0x8086,.device=0x2415,.bar={0x1001,0x2001}}};
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
    hardware_setup(&d);hw.codec[0x2c/2]=44100;hw.rate_stuck=true;
    CHECK(!ac97_codec_prepare(&d) && d.codec_changed && !d.quarantine);
    CHECK(d.rate==48000 && d.raw_rate==44100 && !(d.raw_ext_control&1));
    CHECK(!ac97_codec_restore(&d));
    hardware_setup(&d);hw.reject=true;
    CHECK(ac97_codec_prepare(&d)==-EIO && d.quarantine);
    puts("audio codec: posted CAS/readback, 100ms timeout without writes, rollback/mute PASS");
}
static unsigned trace_at(uint16_t port, uint32_t value)
{
    for(unsigned i=0;i<hw.trace_count;i++) if(hw.trace[i].port==port && hw.trace[i].value==value) return i;
    CHECK(false);return 0;
}
static void test_physical_codec(void)
{
    struct ac97_device d;hardware_setup(&d);d.software_master=false;
    audio.master=(struct ciuki_mixer_master){.version=1,.size=32,.left=50,.right=50};
    hw.ready_delay=12;hw.power_delay=25;hw.sticky_mute=true;
    hw.codec[0x7c/2]=0x4352;hw.codec[0x7e/2]=0x5931;hw.codec[0]=0x10;
    hw.codec[4/2]=0x8000;hw.codec[0x0a/2]=0x8000;
    hw.codec[0x26/2]=0xff00;hw.codec[0x2a/2]=0x0405;hw.codec[0x68/2]=0x8000;
    d.activation_deadline=500;
    CHECK(!ac97_controller_reset(&d) && hw.tick==13 && d.global_status==0x100);
    CHECK(hw.trace[0].port==0x22c && hw.trace[0].value==0 && hw.trace[0].width==4);
    CHECK(hw.trace[1].port==0x22c && hw.trace[1].value==2 && hw.trace[1].width==4);
    CHECK(!ac97_codec_prepare(&d) && hw.tick>=38 && !d.quarantine);
    CHECK(d.codec_id==0x43525931 && d.power==0xf && hw.codec[0x26/2]==0);
    CHECK(hw.codec[2/2]==0x0404 && hw.codec[4/2]==0x0404 && hw.codec[0x18/2]==0x0808);
    CHECK(hw.codec[0x0a/2]==0 && hw.codec[0x68/2]==0 && hw.codec[0x2a/2]==0x0401 && d.rate==48000);
    CHECK(hw.volume_writes[2/2]==3 && hw.volume_writes[4/2]==2 && hw.volume_writes[0x18/2]==2 && hw.volume_writes[0x0a/2]==2);
    CHECK(trace_at(0x126,0)<trace_at(0x102,0x0404));
    CHECK(trace_at(0x12a,0x0401)<trace_at(0x12c,48000));
    struct ciuki_mixer_master m={.left=25,.right=100};
    CHECK(!ac97_set_master(&d,&m) && hw.codec[2/2]==0x0800 && hw.codec[4/2]==0x0800);
    m.mute=1;CHECK(!ac97_set_master(&d,&m) && hw.codec[2/2]==0x8800 && hw.codec[0x18/2]==0x8808);
    CHECK(!ac97_codec_restore(&d) && hw.codec[2/2]==0x8000 && hw.codec[4/2]==0x8000 && hw.codec[0x26/2]==0xff00 && hw.codec[0x68/2]==0x8000);
    /* No VRA: never write 2Ch; absence of a secondary codec is normal. */
    hardware_setup(&d);hw.codec[0x28/2]=0;hw.codec[0x2a/2]=0;hw.codec[0x0a/2]=0xffff;
    CHECK(!ac97_controller_reset(&d) && !ac97_codec_prepare(&d) && d.rate==48000 && !d.beep_present);
    for(unsigned i=0;i<hw.trace_count;i++) CHECK(hw.trace[i].port!=0x12c && hw.trace[i].port!=0x104 && hw.trace[i].port!=0x168);
    /* AD1881A has standard EAPD, no Cirrus/vendor register guessing. */
    hardware_setup(&d);hw.codec[0x7c/2]=0x4144;hw.codec[0x7e/2]=0x5348;hw.codec[0x26/2]=0x8000;
    CHECK(!ac97_codec_prepare(&d) && d.codec_id==0x41445348 && !hw.codec[0x26/2]);
    for(unsigned i=0;i<hw.trace_count;i++) CHECK(hw.trace[i].port!=0x168);
    hardware_setup(&d);hw.global_status=1u<<15;
    CHECK(!ac97_controller_reset(&d) && !(hw.global_status&(1u<<15)));
    hardware_setup(&d);hw.cold_stuck=true;CHECK(ac97_controller_reset(&d)==-EIO && hw.trace_count==1);
    hardware_setup(&d);hw.ready_stuck=true;d.activation_deadline=80;
    CHECK(ac97_controller_reset(&d)==-ETIMEDOUT && hw.tick==80 && hw.trace_count==2);
    hardware_setup(&d);hw.power_stuck=true;d.activation_deadline=60;
    CHECK(ac97_codec_prepare(&d)==-ETIMEDOUT && hw.tick==60 && d.quarantine);
    CHECK(!hw.volume_writes[2/2] && !hw.volume_writes[0x18/2]);
    hardware_setup(&d);hw.reject=true;CHECK(ac97_codec_prepare(&d)==-EIO && d.quarantine);
    hardware_setup(&d);d.pci.device=0x2485;
    struct pci_func modem={.vendor=0x8086,.device=0x2486,.class_code=7,.subclass=3,.bar={0,0x401}};
    CHECK(modem_idle(&d,&modem));
    modem.command=5;hw.modem_sr[0]=hw.modem_sr[1]=AC97_DCH;
    CHECK(modem_idle(&d,&modem));
    hw.modem_cr[1]=1;CHECK(!modem_idle(&d,&modem));
    hw.modem_cr[1]=0;hw.modem_sr[0]=AC97_DCH|AC97_BCIS;CHECK(!modem_idle(&d,&modem));
    hw.modem_sr[0]=AC97_DCH;modem.bar[1]=0x403;CHECK(!modem_idle(&d,&modem));
    modem.bar[1]=0x401;modem.command=4;CHECK(!modem_idle(&d,&modem));
    modem.command=0;modem.vendor=0x1234;CHECK(!modem_idle(&d,&modem));
    audio.master=(struct ciuki_mixer_master){.left=100,.right=100};
    puts("audio physical codec: cold reset/primary-ready/power/RO bits/EAPD/sticky mute/VRA/SPDIF and bounded failures PASS");
}
static void test_diagnostic_records(void)
{
    hardware_setup(&ac97);ac97.pci.vendor=0x8086;ac97.pci.device=0x2485;ac97.pci.irq_line=11;
    ac97.codec_id=0x43525931;ac97.global_status=0x100;ac97.power=0xf;
    ac97.master=0x0404;ac97.pcm_volume=0x0808;ac97.rate=48000;audio.reason="ready";
    audio_init_log(0);
    CHECK(!strcmp(audio_log,"[audio] controller=8086:2485 codec=43525931 ready=00000100 power=000f master=0404 pcm=0808 rate=48000 irq=11 error=0 reason=ready"));
    ac97.cursor=(struct ac97_cursor){.civ=3,.remaining=234};ac97.irqs=7;ac97.fetched=999;
    audio.ready=true;ac97.running=true;audio.error=0;audio.init_error=0;
    ac97.ready=true;hw.sr=AC97_BCIS;hw.civ=3;hw.remaining=234;
    CHECK(ac97_irq(11,"ac97") && ac97.irqs==8 && !(hw.sr&AC97_BCIS));
    CHECK(!ac97_irq(11,"ac97") && ac97.irqs==8);ac97.irqs=7;
    struct ciuki_audio_diagnostics info;
    memset(&info,0xff,sizeof(info));audio_diagnostics(&info);
    CHECK(info.codec_id==0x43525931 && info.controller==0x80862485 && info.flags==3 && info.irqs==7 && info.fetched_frames==999 && info.civ==3 && info.picb==234 && !info.reserved);
    vm_active_hook=true;audio_diagnostics(&info);CHECK(info.flags & CIUKI_AUDIO_DIAG_VM_ACTIVE);
    vm_active_hook=false;audio_diagnostics(&info);CHECK(!(info.flags & CIUKI_AUDIO_DIAG_VM_ACTIVE));
    first_playback=true;playback_logged=false;playback_start=playback_next=100;g_ticks=100;g_boot.flags=CBI_F_VERBOSE;
    playback_log(false);
    CHECK(!strcmp(audio_log,"[audio] play=running civ=3 picb=234 irqs=7 fetched=999 elapsed_ms=0"));
    unsigned count=audio_log_count;g_ticks=200;playback_log(false);CHECK(audio_log_count==count);
    g_ticks=1100;playback_log(false);CHECK(strstr(audio_log,"play=complete") && playback_logged);
    first_playback=true;playback_logged=false;playback_next=2000;playback_log(true);CHECK(strstr(audio_log,"play=failed"));
    CHECK(audio_boot_tone_enabled(CBI_F_VERBOSE) && !audio_boot_tone_enabled(0) && !audio_boot_tone_enabled(CBI_F_VERBOSE|CBI_F_SAFE_MODE) && !audio_boot_tone_enabled(CBI_F_VERBOSE|CBI_F_TEST_REQUEST));
    CHECK(boot_sample(0)==0 && boot_sample(AUDIO_RATE-1)==0 && boot_sample(108)==8192 && boot_sample(132)==-8192);
    for(unsigned i=96;i<AUDIO_RATE-144;i++) CHECK(boot_sample(i)==boot_sample(i+48));
    g_boot.flags=0;first_playback=playback_logged=false;audio.ready=false;memset(&ac97,0,sizeof(ac97));
    puts("audio diagnostics: init/play record grammar, snapshot counts and L-only exact 1kHz boot waveform PASS");
}
static void cs4297a_setup(struct ac97_device *d, bool vra)
{
    hardware_setup(d);hw.codec_model=true;d->software_master=false;
    hw.codec[0x7c/2]=0x4352;hw.codec[0x7e/2]=0x5936;
    hw.codec[0]=0x10;hw.codec[4/2]=0x8000;hw.codec[0x68/2]=0x8000;
    hw.codec[0x28/2]=0x0200 | vra;hw.codec[0x2a/2]=0;
    hw.codec[0x2c/2]=vra?44100:48000;
    audio.master=(struct ciuki_mixer_master){.left=50,.right=50};
}
static void test_cs4297a_codec(void)
{
    struct ac97_device d;
    for(unsigned vra=0;vra<2;vra++) {
        /* VRA=1 is a synthetic compatibility case, not real CS4297A silicon. */
        cs4297a_setup(&d,vra);
        CHECK(!ac97_codec_prepare(&d) && d.codec_id==0x43525936 && d.rate==48000);
        CHECK(d.raw_rate==48000 && d.raw_ext_control==vra && !d.quarantine);
        CHECK(hw.codec[4/2]==0x0404 && hw.codec[0x68/2]==0);
        if(vra) CHECK(trace_at(0x12a,1)<trace_at(0x12c,48000));
        else for(unsigned i=0;i<hw.trace_count;i++)CHECK(hw.trace[i].port!=0x12c);
    }
    cs4297a_setup(&d,false);hw.ready_delay=40;hw.power_delay=70;hw.post_delay=2;
    CHECK(!ac97_controller_reset(&d) && !ac97_codec_prepare(&d) && hw.tick>=111 && hw.tick<500);
    CHECK(d.power==0xf && d.rate==48000);
    /* Optional/RO register returns zero: effective fixed output is still 48k. */
    cs4297a_setup(&d,false);hw.codec[0x2c/2]=0;
    CHECK(!ac97_codec_prepare(&d) && d.rate==48000 && !d.raw_rate && !d.raw_ext_control);
    cs4297a_setup(&d,true);hw.rate_stuck=true;
    CHECK(!ac97_codec_prepare(&d) && d.rate==48000 && d.raw_rate==44100 && !d.raw_ext_control);
    CHECK(trace_at(0x12a,1)<trace_at(0x12c,48000) && trace_at(0x12c,48000)<trace_at(0x12a,0));
    cs4297a_setup(&d,true);hw.vra_enable_stuck=true;
    CHECK(!ac97_codec_prepare(&d) && d.rate==48000 && !d.raw_ext_control);
    for(unsigned i=0;i<hw.trace_count;i++)CHECK(hw.trace[i].port!=0x12c);
    cs4297a_setup(&d,true);hw.rate_stuck=hw.vra_disable_stuck=true;
    CHECK(ac97_codec_prepare(&d)==-EIO && !strcmp(d.failure.stage,"codec_rate"));
    CHECK(d.failure.ext_control==1 && d.failure.rate==44100 && !d.rate);
    cs4297a_setup(&d,false);hw.codec[0x2c/2]=0xffff;
    CHECK(ac97_codec_prepare(&d)==-EIO && !d.rate && !d.codec_changed && d.quarantine);
    puts("audio CS4297A: fixed48/read-only rate, synthetic VRA enable/readback/fallback, slow ready and invalid reads PASS");
}
static void ich3_setup(void)
{
    static struct ac97_bd bdl[32];static struct audio_frame buffers[32][256];
    cs4297a_setup(&ac97,false);hw.ich3=hw.any_pcm=true;
    memset(bdl,0,sizeof(bdl));memset(buffers,0xa5,sizeof(buffers));
    ac97.bdl=bdl;ac97.bdl_phys=0x4000;
    for(unsigned i=0;i<32;i++) { ac97.pcm[i]=buffers[i];bdl[i].address=0x8000+i*1024; }
    ac97.pci=(struct pci_func){.vendor=0x8086,.device=0x2485,.command=5,.irq_line=11};
    ac97.command=5;hw.pci_command=5;
    for(unsigned i=0;i<12;i++)ac97.claims[i]=-1;
}
static void test_ich3_bringup(void)
{
    /* Reset-state DCH requires neither CIV!=LVI nor a completion IRQ. */
    ich3_setup();CHECK(!hw.bdbar && !hw.remaining && hw.civ==hw.lvi);
    CHECK(!ac97_pause(&ac97) && hw.sr==AC97_DCH && !hw.resets && !hw.starts);
    for(unsigned variant=0;variant<4;variant++) {
        ich3_setup();
        if(variant==1) { hw.codec[0x28/2]|=1;hw.codec[0x2c/2]=44100; }
        if(variant==2) { hw.ready_delay=40;hw.power_delay=50;hw.reset_delay=4;hw.start_delay=6;hw.post_delay=2;hw.stop_after=150; }
        if(variant==3)hw.exhausted_start=true;
        CHECK(!controller_activate(&ac97) && !ac97.quarantine && !ac97.running && !ac97.stopping);
        CHECK(hw.starts==1 && hw.resets==2 && hw.pci_command==5 && hw.bdbar==0x4000 && hw.sr==AC97_DCH);
        CHECK(!ac97.published && !ac97.retired && !ac97.fetched && ac97.rate==48000 && hw.tick<500);
        CHECK(trace_at(0x21b,2)<trace_at(0x210,0x4000));
        CHECK(trace_at(0x12a,variant==1?1:0)<trace_at(0x215,1));
        CHECK(trace_at(0x215,1)<trace_at(0xcfc,5) && trace_at(0xcfc,5)<trace_at(0x21b,1));
        for(unsigned i=0;i<hw.trace_count;i++) if(hw.trace[i].port==0x21b)CHECK(!(hw.trace[i].value&0x1c));
    }
    /* Sample requests absent (zero-rate VRA) cannot prove a start. */
    ich3_setup();hw.pci_command=1;ac97.activation_deadline=30;
    CHECK(!channel_reset(&ac97,30));transport_publish(&ac97);hw.pci_command=5;
    hw.codec[0x2a/2]=1;hw.codec[0x2c/2]=0;
    CHECK(transport_prove(&ac97)==-ETIMEDOUT && hw.tick==30 && hw.resets==1);
    CHECK(!strcmp(ac97.failure.stage,"dma_start") && ac97.failure.status[1]==AC97_DCH);
    for(unsigned fault=0;fault<4;fault++) {
        ich3_setup();
        if(fault==0)hw.start_stuck=true;
        if(fault==1)hw.stop_stuck=true;
        if(fault==2)hw.reset_stuck=true;
        if(fault==3)hw.run_clear_stuck=true;
        int err=controller_activate(&ac97);
        CHECK(err==(fault==0?-ETIMEDOUT:-EIO) && hw.tick>=500 && hw.tick<510 && ac97.failure.stage);
        CHECK(!strcmp(ac97.failure.stage,fault==0?"dma_start":fault==2?"dma_reset":"dma_pause"));
        unsigned resets=hw.resets;uint32_t canary=ac97.bdl[0].address;
        /* A start timeout can still prove a safe stop within cleanup's budget. */
        if(fault==0) { CHECK(!ac97_pause(&ac97) && !ac97.quarantine); }
        else { ac97_shutdown();CHECK(ac97.quarantine && audio.quarantined && hw.resets==resets && ac97.bdl[0].address==canary); }
        audio.quarantined=false;
    }
    ich3_setup();hw.bdbar_stuck=true;
    CHECK(controller_activate(&ac97)==-EIO && !strcmp(ac97.failure.stage,"dma_bdbar") && !hw.starts && !(hw.pci_command&4));
    /* The captured T23 power=0000 case now reaches the silent DMA proof. */
    ich3_setup();hw.power_stuck=true;hw.input_sr=AC97_DCH|AC97_BCIS;hw.mic_sr=AC97_DCH|AC97_LVBCI;
    CHECK(!controller_activate(&ac97) && hw.tick>=5000 && hw.tick<5010 && hw.starts==1);
    CHECK(ac97.analog_warning && !ac97.power && !ac97.failure.stage && !ac97.quarantine && !ac97.running);
    audio.quarantined=false;memset(&ac97,0,sizeof(ac97));
    puts("audio ICH3: reset/default DCH, valid silent BDL before BME, bounded start/stop, failures retained and analog warning PASS");
}
static void test_codec_power_policy(void)
{
    ich3_setup();hw.power_stuck=hw.reset_defaults=true;hw.post_delay=2;
    hw.global_control=0x0a; /* BIOS link off; cold reset must clear it. */
    hw.codec[0x26/2]=0xff00;hw.codec[0x5e/2]=0xffff;hw.codec[0x60/2]=0;
    hw.codec[2/2]=hw.codec[0x18/2]=0; /* the physical capture's zeros */
    unsigned warnings=analog_warning_logs, accepted=response_warning_logs;
    CHECK(!controller_activate(&ac97) && ac97.analog_warning && !ac97.quarantine);
    CHECK(hw.codec_resets==1 && hw.starts==1 && hw.resets==2 && !hw.cas_owned && !hw.posted);
    CHECK(hw.tick>=5000 && hw.tick<5200 && !ac97.ready && !ac97.running && !ac97.failure.stage);
    CHECK(analog_warning_logs==warnings+1 && response_warning_logs==accepted+1);
    CHECK(!ac97.power && ac97.master==0x0404 && ac97.pcm_volume==0x0808 && ac97.codec_valid==7);
    CHECK(hw.codec[0x5e/2]==0x80 && hw.codec[0x60/2]==1 && !hw.codec[0x68/2]);
    CHECK(ac97.saved_master==0x8000 && ac97.saved_pcm==0x8808 && !ac97.saved_power);
    CHECK(trace_at(0x100,0)<trace_at(0x126,0));
    CHECK(trace_at(0x126,0)<trace_at(0x102,0x8101) && trace_at(0x102,0x8101)<trace_at(0x102,0x8000));
    CHECK(trace_at(0x102,0x8000)<trace_at(0x21b,1));
    CHECK(strstr(audio_log,"stage=dma_proved reg26=0000 reg02=0404 reg18=0808 valid=07"));
    CHECK(!ac97_codec_restore(&ac97) && hw.codec[2/2]==0x8000 && hw.codec[0x18/2]==0x8808);

    /* Calibration beyond the old 500 ms cap, still below five seconds. */
    ich3_setup();hw.power_delay=750;
    CHECK(!controller_activate(&ac97) && !ac97.analog_warning && hw.tick>=750 && hw.tick<800 && hw.starts==1);
    /* The warning never grants admission if DMA cannot start. */
    ich3_setup();hw.power_stuck=hw.start_stuck=true;
    CHECK(controller_activate(&ac97)==-ETIMEDOUT && !ac97.ready && ac97.analog_warning);
    CHECK(!strcmp(ac97.failure.stage,"dma_start") && hw.tick>=5500 && hw.tick<5510);

    /* Entire codec absent, including ID, despite the controller ready flag. */
    ich3_setup();hw.zero_reads=true;
    CHECK(controller_activate(&ac97)==-EIO && !hw.starts && !hw.codec_resets);
    CHECK(!strcmp(ac97.failure.stage,"codec_identity") && !strcmp(ac97.codec_error,"identity_invalid"));
    CHECK(strstr(audio_log,"stage=codec_identity") && strstr(audio_log,"access=identity_invalid"));
    /* Plausible ID/rate cannot excuse stale/ignored analog-register writes. */
    ich3_setup();hw.mixer_zero=true;
    accepted=response_warning_logs;
    CHECK(controller_activate(&ac97)==-EIO && !hw.starts && !ac97.ready);
    CHECK(!strcmp(ac97.failure.stage,"codec_response") && strstr(audio_log,"access=write_mismatch"));
    CHECK(response_warning_logs==accepted && !ac97.power && !ac97.master && !ac97.pcm_volume);
    ich3_setup();hw.master_stuck=true;
    CHECK(controller_activate(&ac97)==-EIO && !hw.starts && !strcmp(ac97.failure.stage,"codec_response"));

    /* Timeout with zero data is never downgraded to an analog warning. RCS
     * clear must not consume unrelated W1C GPI/resume status. */
    ich3_setup();hw.read_timeout_reg=0x26;hw.power_stuck=true;hw.global_status=0xc01;
    CHECK(controller_activate(&ac97)==-EIO && !hw.starts && !ac97.analog_warning);
    CHECK(!strcmp(ac97.codec_error,"read_timeout") && !strcmp(ac97.failure.stage,"codec_reset"));
    CHECK((hw.global_status&0x8c01)==0xc01 && (ac97.global_status&(1u<<15)));
    ich3_setup();hw.busy=true;
    CHECK(controller_activate(&ac97)==-ETIMEDOUT && hw.tick==101 && !hw.codec_resets && !hw.starts);
    CHECK(!strcmp(ac97.codec_error,"cas_timeout"));
    uint16_t value;
    for(unsigned fault=0;fault<2;fault++) {
        ich3_setup();
        if(fault)hw.ready_stuck=true;else hw.global_control=0x0a;
        CHECK(ac97_codec_read(&ac97,2,&value,500)==-EIO && !hw.codec_reads && !hw.cas_owned && !hw.writes);
        CHECK(!strcmp(ac97.codec_error,fault?"primary_not_ready":"link_off_or_reset"));
    }
    /* QEMU-style codec identity/software gain still traverses the proof. */
    ich3_setup();hw.codec[0x7c/2]=0x8384;hw.codec[0x7e/2]=0x7600;
    ac97.software_master=true;ac97.pcm_unity=0;
    CHECK(!controller_activate(&ac97) && !ac97.analog_warning && hw.starts==1 && hw.resets==2);
    CHECK(!ac97.master && !ac97.pcm_volume && ac97.rate==48000 && hw.tick<500);
    memset(&ac97,0,sizeof(ac97));
    puts("audio codec power: register reset, 5s warning with response+DMA proof, zero/stale reads, CAS ownership, RCS/link errors, QEMU model PASS");
}
static void qemu_setup(void)
{
    ich3_setup();hw.qemu_model=true;hw.global_control=0;
    ac97.pci.device=0x2415;ac97.software_master=true;ac97.pcm_unity=0;
    hw.codec[0x7c/2]=0x8384;hw.codec[0x7e/2]=0x7600;
    hw.codec[0x28/2]=0x0809;hw.codec[0x2a/2]=0x0009;
    hw.codec[0]=0;hw.reset_defaults=true;
}
static void test_controller_reset_models(void)
{
    uint32_t flags=g_boot.flags;
    uint16_t value;
    /* Prove the physical fake returns FFFF while reset or readiness blocks
     * the link, and that normal activation never performs those reads. */
    ich3_setup();hw.global_control=0;hw.cas_owned=true;
    CHECK(mock_read(&hw,0x12a,2)==0xffff);
    hw.global_control=2;hw.ready_at=750;hw.cas_owned=true;
    CHECK(mock_read(&hw,0x12c,2)==0xffff && hw.unready_codec_reads==2);
    ich3_setup();hw.ready_delay=750;
    unsigned cold=cold_reset_logs, qemu=qemu_reset_logs;
    CHECK(!controller_activate(&ac97) && hw.tick>=751 && hw.tick<1000);
    CHECK(!hw.unready_codec_reads && cold_reset_logs==cold+1 && qemu_reset_logs==qemu);
    CHECK(hw.global_control==2 && hw.starts==1 && hw.resets==2);

    /* Exact upstream divergence: zero persists after BOTH reset requests,
     * PCR is already set, and word mixer reads still return real data. */
    g_boot.flags=flags|CBI_F_SMBIOS_QEMU;qemu_setup();
    hw.pci_command=1; /* raw reset-model check also requires BME clear */
    mock_write(&hw,0x22c,2,4);mock_write(&hw,0x22c,4,4);
    CHECK(!hw.global_control && mock_read(&hw,0x230,4)==0x100);
    hw.cas_owned=true;CHECK(mock_read(&hw,0x17c,2)==0x8384);
    qemu_setup();cold=cold_reset_logs;qemu=qemu_reset_logs;
    CHECK(!controller_activate(&ac97) && !hw.global_control && !ac97.analog_warning);
    CHECK(qemu_reset_logs==qemu+1 && cold_reset_logs==cold && !hw.unready_codec_reads);
    CHECK(hw.codec_resets==1 && hw.starts==1 && hw.resets==2 && !hw.cas_owned && !hw.posted);
    CHECK(ac97.codec_id==0x83847600 && ac97.codec_valid==7 && ac97.rate==48000);
    CHECK(!ac97.master && !ac97.pcm_volume && hw.tick<500 && !ac97.failure.stage);
    CHECK(trace_at(0x22c,0)<trace_at(0x22c,2));

    /* A future compliant QEMU controller takes the ordinary cold path. */
    qemu_setup();hw.qemu_model=false;hw.global_control=2;hw.ready_delay=12;
    cold=cold_reset_logs;qemu=qemu_reset_logs;
    CHECK(!controller_activate(&ac97) && cold_reset_logs==cold+1 && qemu_reset_logs==qemu);
    CHECK(hw.global_control==2 && !hw.unready_codec_reads && hw.starts==1);

    /* The exception requires the boot flag AND both PCI identity fields.
     * Software attenuation by itself must never relax a physical reset. */
    for(unsigned fault=0;fault<3;fault++) {
        g_boot.flags=flags|CBI_F_SMBIOS_QEMU;qemu_setup();
        if(fault==0)g_boot.flags&=~CBI_F_SMBIOS_QEMU;
        if(fault==1)ac97.pci.device=0x2485;
        if(fault==2)ac97.pci.vendor=0x1039;
        CHECK(controller_activate(&ac97)==-EIO && !hw.codec_reads && !hw.starts);
        CHECK(!strcmp(ac97.failure.stage,"controller_reset"));
        CHECK(ac97_codec_read(&ac97,2,&value,500)==-EIO && !hw.codec_reads);
        CHECK(!strcmp(ac97.codec_error,"link_off_or_reset"));
    }
    g_boot.flags=flags|CBI_F_SMBIOS_QEMU;
    /* Even on the twin, missing readiness, bad reads, RCS and CAS cannot
     * be accepted; synthetic fault injection extends the upstream model. */
    qemu_setup();hw.ready_stuck=true;
    CHECK(controller_activate(&ac97)==-ETIMEDOUT && hw.tick==1000 && !hw.codec_reads && !hw.starts);
    qemu_setup();hw.codec[0x7c/2]=hw.codec[0x7e/2]=0xffff;
    CHECK(controller_activate(&ac97)==-EIO && !hw.starts && !strcmp(ac97.failure.stage,"codec_identity"));
    qemu_setup();hw.read_timeout_reg=0x26;
    CHECK(controller_activate(&ac97)==-EIO && !hw.starts && !strcmp(ac97.codec_error,"read_timeout"));
    qemu_setup();hw.busy=true;
    CHECK(controller_activate(&ac97)==-ETIMEDOUT && !hw.codec_reads && !hw.starts && hw.tick==101);
    qemu_setup();hw.cold_stuck=true;hw.global_control=8;
    CHECK(controller_activate(&ac97)==-EIO && !hw.codec_reads && !hw.starts);
    qemu_setup();hw.global_control=4;
    CHECK(ac97_codec_read(&ac97,2,&value,500)==-EIO && !hw.codec_reads);

    /* With the QEMU flag set, real ICH3 still rejects failed release and
     * retains its bounded primary-ready wait without accessing NAM. */
    ich3_setup();hw.release_stuck=true;
    CHECK(controller_activate(&ac97)==-EIO && !hw.codec_reads && !hw.starts);
    ich3_setup();hw.ready_stuck=true;
    CHECK(controller_activate(&ac97)==-ETIMEDOUT && hw.tick==1000 && !hw.codec_reads && !hw.starts);
    g_boot.flags=flags;memset(&ac97,0,sizeof(ac97));
    puts("audio reset models: physical gated NAM, QEMU ignored GC_CR/GC_WR, strict identity/readiness/access gates and both DMA proofs PASS");
}
static uint64_t boot_host_tick;
static unsigned boot_mixed_frames;
static bool boot_traffic_logged;
static struct ciuki_mixer_master boot_expected_master;
static uint16_t boot_expected_codec;
static unsigned boot_peak;
static void boot_host_worker(void)
{
    CHECK(!memcmp(&audio.master,&boot_expected_master,sizeof(audio.master))&&ac97.master==boot_expected_codec);
    unsigned elapsed=(unsigned)(g_ticks-boot_host_tick);boot_host_tick=g_ticks;hw.tick=g_ticks;
    if(ac97.running) {
        mock_advance(elapsed*96);
        if (hw.sr & (AC97_BCIS | AC97_LVBCI)) CHECK(ac97_irq(11,"ac97"));
    }
    audio_service();
    for(unsigned slot=0;slot<32;slot++)for(unsigned i=0;i<AUDIO_QUANTUM;i++) {
        int sample=ac97.pcm[slot][i].left;unsigned peak=(unsigned)(sample<0?-sample:sample);
        if(peak>boot_peak)boot_peak=peak;
    }
    if (slow_playback_log && audio.running && !boot_traffic_logged) {
        boot_traffic_logged=true; klog("[usb] verbose traffic during native playback");
        CHECK(serial_is_buffered);
    }
    if(audio.streams[0]) boot_mixed_frames=(unsigned)audio.streams[0]->consumed;
}
static void test_boot_transport_case(unsigned left,unsigned right,unsigned mute,bool software)
{
    static struct ac97_bd bdl[32];static struct audio_frame buffers[32][256];
    memset(buffers,0,sizeof(buffers));
    hardware_setup(&ac97);hw.any_pcm=true;ac97.software_master=software;ac97.ready=true;
    ac97.bdl=bdl;for(unsigned i=0;i<32;i++)ac97.pcm[i]=buffers[i];
    audio.ready=true;audio.error=0;audio.running=false;audio.output=0;audio.played_output=0;
    audio.master=(struct ciuki_mixer_master){.version=1,.size=32,.left=left,.right=right,.mute=mute,.save_state=CIUKI_MIXER_SAVED};
    audio.master_in_codec=!software;
    CHECK(!ac97_set_master(&ac97,&audio.master));
    uint16_t volume=ac97.master;unsigned pages=pages_used, heap=heap_blocks;
    boot_expected_master=audio.master;boot_expected_codec=volume;boot_peak=0;
    first_playback=playback_logged=false;idle_since=non_silent_end=0;
    g_ticks=boot_host_tick=5000;hw.tick=g_ticks;g_boot.flags=CBI_F_VERBOSE;audio.caps.drain_ms=20;
    slow_playback_log=true; boot_traffic_logged=false;
    on_schedule=boot_host_worker;audio_boot_tone();on_schedule=0;
    slow_playback_log=false;
    CHECK(!audio.error && boot_mixed_frames==AUDIO_RATE && ac97.fetched>=AUDIO_RATE && ac97.irqs>0);
    CHECK(boot_traffic_logged && serial_is_buffered);
    CHECK(strstr(audio_log,"boot_tone=1khz frames=48000 result=0 restore=0"));
    CHECK(!memcmp(&audio.master,&boot_expected_master,sizeof(audio.master))&&ac97.master==volume);
    CHECK(boot_peak==(mute ? 0u : software ? 8192u*left/100u : 8192u));
    CHECK(!audio.streams[0] && pages_used==pages && heap_blocks==heap && !audio.commitment_pages);
    if(left==20 && !mute && !software) {
        uint32_t frames=0; on_schedule=boot_host_worker;
        CHECK(!audio_check_duration(10,&frames) && frames==10*AUDIO_RATE);
        on_schedule=0;
        CHECK(strstr(audio_log,"transport check10s frames=480000 underruns=0"));
        CHECK(strstr(audio_log,"irq_mean_us=") && !audio.commitment_pages);
    }
    CHECK(!ac97_stop(&ac97));audio.running=false;
    /* No worker progress: startup remains bounded and restores the master. */
    uint64_t before=g_ticks;audio_boot_tone();
    CHECK(g_ticks-before==3000 && strstr(audio_log,"result=-110 restore=0"));
    CHECK(ac97.master==volume && audio.master.mute==mute && !audio.streams[0] && pages_used==pages && heap_blocks==heap && !audio.commitment_pages);
    audio.ready=audio.master_in_codec=false;
    first_playback=playback_logged=false;idle_since=non_silent_end=0;g_boot.flags=0;memset(&ac97,0,sizeof(ac97));
    audio.master=(struct ciuki_mixer_master){.left=100,.right=100};
    puts("audio boot transport: 48000 copied frames consumed/fetched, IRQs, restored mute/volume and stream cleanup PASS");
}
static void test_boot_transport(void)
{
    test_boot_transport_case(0,25,1,false);
    test_boot_transport_case(20,20,0,true);
    test_boot_transport_case(73,73,0,true);
    test_boot_transport_case(20,20,0,false);
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
static void test_cursor_handoff(void)
{
    struct ac97_device d; hardware_setup(&d);
    d.running=true; d.published=160; d.retired=152; d.last_progress=hw.tick;
    d.generation_fetched=152*AUDIO_QUANTUM+200; d.fetched=d.generation_fetched;
    hw.cr=1; hw.sr=0; hw.civ=24; hw.piv=25; hw.lvi=31; hw.remaining=0;
    uint64_t fetched=d.fetched;
    CHECK(ac97_progress(&d)==-EAGAIN && d.fetched==fetched && d.retired==152);
    /* PICB reloads before CIV changes, another documented handoff sample. */
    hw.remaining=512; hw.tick++;
    CHECK(ac97_progress(&d)==-EAGAIN && d.fetched==fetched && d.retired==152);
    hw.civ=25; hw.piv=26; hw.remaining=448; hw.tick++;
    CHECK(!ac97_progress(&d) && d.retired==153 && d.generation_fetched==153*AUDIO_QUANTUM+32 && !d.cursor_retrying);
    /* Ring wrap: zero PICB with a new CIV must not commit a full buffer. */
    d.retired=159; d.published=167; d.generation_fetched=159*AUDIO_QUANTUM+200;
    d.fetched=d.generation_fetched; d.last_progress=hw.tick;
    hw.civ=0; hw.piv=1; hw.lvi=6; hw.remaining=0; fetched=d.fetched;
    CHECK(ac97_progress(&d)==-EAGAIN && d.retired==159 && d.fetched==fetched);
    hw.remaining=448; hw.tick++;
    CHECK(!ac97_progress(&d) && d.retired==160 && d.generation_fetched==160*AUDIO_QUANTUM+32);
    /* A stuck zero count and a persistent regression still fail locally. */
    hw.remaining=0;
    CHECK(ac97_progress(&d)==-EAGAIN); hw.tick+=3;
    CHECK(ac97_progress(&d)==-EIO && d.deadline_misses==1);
    d.cursor_retrying=false; hw.remaining=512; d.last_progress=hw.tick;
    CHECK(ac97_progress(&d)==-EAGAIN); hw.tick+=3;
    CHECK(ac97_progress(&d)==-EIO && d.deadline_misses==2);
    /* A real DCH is never retried or replayed. */
    d.cursor_retrying=false; hw.remaining=0; hw.sr=AC97_DCH;
    CHECK(ac97_progress(&d)==-EIO && d.dma_halts==1);
    puts("audio physical cursor: zero/reload handoffs, wrap, bounded retries and genuine halt PASS");
}
static void test_formats(void)
{
    audio.ready=true;audio.master.left=audio.master.right=100;audio.linear_resampling=true;
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
    CHECK(!audio_stream_open(&s));
    struct audio_frame physical[3]={{10000,10000},{10000,10000},{10000,10000}}, played;
    CHECK(audio_stream_enqueue(s,physical,sizeof(physical))==sizeof(physical));
    audio.master_in_codec=true;audio.master.left=50;audio.master.right=0;
    audio_mix(&played,1);CHECK(played.left==10000 && !played.right); /* codec supplies master attenuation */
    audio.master.mute=1;audio_mix(&played,1);CHECK(!played.left && !played.right);
    audio.master_in_codec=false;audio.master.mute=0;audio.master.left=audio.master.right=100;audio_stream_close(s);
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
    audio.linear_resampling=false;
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
static void test_codec_failure_release(void)
{
    for (unsigned fault = 0; fault < 3; fault++) {
        unsigned before = pages_used;
        ich3_setup(); hw.post_delay = 200; /* reset write holds CAS beyond 100 ms */
        CHECK(controller_activate(&ac97) == -ETIMEDOUT);
        CHECK(!ac97.dma_enabled && !hw.starts && ac97.failure.stage &&
              !strcmp(ac97.failure.stage,"codec_reset") && !strcmp(ac97.codec_error,"cas_timeout"));
        ac97.activation_deadline=0;
        resource_releases=resource_quarantines=irq_removals=0;
        for (unsigned i=0;i<12;i++) {
            ac97.claims[i]=(int)i;ac97.claim_generation[i]=100+i;
            audio_resources[i]=(struct resource){.type=i==2 ? RES_IRQ : RES_MMIO,
                .start=i==2 ? 11u : i*4096u,.end=i==2 ? 12u : (i+1)*4096u,
                .shareable=i==2,.generation=100+i,.state=RS_ACTIVE};
        }
        ac97.bdl_phys=pmm_alloc();ac97.bdl=P2V(ac97.bdl_phys);
        for(unsigned i=0;i<8;i++)ac97.payload_pages[i]=pmm_alloc();
        audio.commitment_pages=9;audio.quarantined=false;audio.reason="codec_reset";
        unsigned resets=hw.resets, reads=hw.codec_reads;
        if(fault==1)hw.input_sr=0; /* another channel cannot prove stop */
        if(fault==2)ac97.dma_enabled=true; /* later DMA fault cannot use this rollback */
        ac97_shutdown();
        CHECK(hw.resets==resets && hw.codec_reads==reads && !hw.starts && !irq_removals);
        if(!fault) {
            CHECK(resource_releases==12 && !resource_quarantines && !ac97.quarantine && !audio.quarantined);
            CHECK(pages_used==before && !audio.commitment_pages && !(hw.pci_command&5));
            CHECK(!strcmp(audio.reason,"codec_reset") && strstr(audio_log,"leases=released"));
        } else {
            CHECK(resource_quarantines==12 && !resource_releases && ac97.quarantine && pages_used==before+9);
            pmm_free(ac97.bdl_phys);for(unsigned i=0;i<8;i++)pmm_free(ac97.payload_pages[i]);
            audio.commitment_pages=0;audio.quarantined=false;
        }
    }
    memset(&ac97,0,sizeof(ac97));
    puts("audio codec failure: pre-DMA CAS timeout releases IRQ/controller/pages; unproved input or prior BME retains quarantine PASS");
}
/* f4-05 hooks (weak in audio_dev.c; the VM sources are not linked here). */
static bool vm_attached_hook, vm_nonzero_hook;
bool audio_vm_active(void) { return vm_active_hook; }
bool audio_vm_playing(void) { return vm_active_hook; }
bool audio_vm_attached(void) { return vm_attached_hook; }
void audio_vm_mix(int64_t (*sums)[2], unsigned frames)
{
    modeled_cycles += modeled_mix_us;
    if (vm_nonzero_hook) for (unsigned i=0;i<frames;i++) { sums[i][0]+=12000; sums[i][1]+=12000; }
}
/* The idle tail with a DOS VM attached keeps AUDIO_LEAD periods. A worker that
 * runs late can find the DMA on the last valid period (published-retired 1)
 * or halted at it; ac97_publish refuses the CIV/PIV slots. Either way the tail
 * must end as a normal transport stop, never as a backend failure, also when
 * a VM sound arrives in that service, and the next sound restarts it
 * (QEMU campaign 13, image 22320ccc: the backend died between VMPLAY stages
 * 4 and 5, after_pages 17 -> 8, no OPL3 event consumed). */
static void test_idle_tail_drain(void)
{
    static struct ac97_bd bdl[32]; static struct audio_frame buffers[32][AUDIO_QUANTUM];
    hardware_setup(&ac97); hw.any_pcm=true;
    ac97.bdl=bdl; ac97.ready=true; for(unsigned i=0;i<32;i++) ac97.pcm[i]=buffers[i];
    for(unsigned i=0;i<12;i++) ac97.claims[i]=-1;
    audio.ready=true; audio.error=0; audio.running=false; audio.output=0;
    audio.adaptive_lead=0; idle_since=non_silent_end=0;
    vm_attached_hook=vm_active_hook=true; g_ticks=hw.tick=1000;
    audio_service(); CHECK(audio.running && ac97.published==AUDIO_VM_LEAD);
    vm_active_hook=false; g_ticks++; hw.tick++; audio_service();
    CHECK(idle_since && ac97.published==AUDIO_TAIL_LEAD);
    /* Deep silence covers the measured 134 ms gap even with a paused VM. */
    mock_advance(134*96); g_ticks+=134; hw.tick+=134; audio_service();
    CHECK(!audio.error && audio.running && !audio.halts);
    /* Source arrival cuts only silence, without waiting for the deep tail. */
    vm_active_hook=true; audio_service();
    CHECK(!audio.error && audio.running && ac97.published-ac97.retired==audio.lead_periods);
    vm_active_hook=vm_attached_hook=false; audio.ready=audio.running=false;
    audio.adaptive_lead=0; idle_since=non_silent_end=0; memset(&ac97,0,sizeof(ac97));
    puts("audio idle tail: paused VM receives measured-gap cushion, source cuts proven silence PASS");
}
static void test_idle_tail_cursor_error(void)
{
    static struct ac97_bd bdl[32]; static struct audio_frame buffers[32][256];
    for(unsigned fault=0;fault<5;fault++) {
    hardware_setup(&ac97); hw.any_pcm=true;
    ac97.bdl=bdl; ac97.ready=true; for(unsigned i=0;i<32;i++) ac97.pcm[i]=buffers[i];
    for(unsigned i=0;i<12;i++) ac97.claims[i]=-1;
    audio.ready=true; audio.error=0; audio.running=false; audio.output=0; idle_since=0;
    audio.master=(struct ciuki_mixer_master){.left=100,.right=100};
    vm_attached_hook=vm_active_hook=true; g_ticks=1000;
    audio.adaptive_lead=0; non_silent_end=0;
    audio_service();
    hw.sr=0; hw.civ=1; hw.piv=2; hw.remaining=512;
    vm_nonzero_hook=fault!=4; g_ticks++; audio_service();
    CHECK(ac97.published==AUDIO_VM_LEAD+1 && buffers[AUDIO_VM_LEAD][0].left==(fault==4?0:12000));
    vm_nonzero_hook=vm_active_hook=false;
    /* Source becomes inactive after rendering silence; the earlier PCM is
     * still queued. Invalid CIV cannot establish that it was fetched. */
    ac97.touched=true;
    if(fault==0 || fault==4) hw.bad_civ_reads=2;
    if(fault==1) hw.piv=32;
    if(fault==2) ac97.generation_fetched=AUDIO_QUANTUM+200;
    if(fault==3) hw.tick=ac97.last_progress+101;
    g_ticks++; audio_service();
    if(fault==2) {
        CHECK(!audio.error && audio.ready); /* transient backwards count: retry without consuming sources */
        hw.tick+=3; g_ticks+=3; audio_service();
    }
    printf("idle cursor error with queued PCM: error=%d ready=%u running=%u published=%llu\n",audio.error,audio.ready,audio.running,(unsigned long long)ac97.published);
    if(fault<2 || fault==4) CHECK(audio.error==-EIO && !audio.ready);
    else CHECK(!audio.error && audio.ready && !audio.running); /* stop proof allows error recovery */
    vm_active_hook=vm_attached_hook=false; audio.ready=audio.running=false; idle_since=0;
    memset(&ac97,0,sizeof(ac97)); non_silent_end=0;
    }
    puts("audio idle tail: CIV/PIV/regression/timeout fail backend, zero PCM alone does not prove drain PASS");
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
static bool watch_source_read, source_urgent;
void __real_fs_lock_acquired(fs_lock *lock);
void __wrap_fs_lock_acquired(fs_lock *lock)
{
    __real_fs_lock_acquired(lock);
    const struct vfs_request *r=fs_request_current();
    if (watch_source_read && r && r->operation && !strcmp(r->operation,"read")) source_urgent=r->urgent_reader;
}
static void test_refill_demand(void)
{
    audio.ready=true; audio.error=0;
    int fd=file_open(client,"/dev/audio",O_WRONLY|O_NONBLOCK,0); CHECK(fd>=0);
    struct file_description *d=file_fd(client,fd); struct audio_stream *s=d->audio;
    struct ciuki_audio_command c={.version=1,.size=sizeof(c)};
    CHECK(!audio_refill_reader(client,4096)); /* open but unused */
    CHECK(!ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c)) && !audio_refill_reader(client,4096));
    s->started=true; s->queued=AUDIO_RING;
    CHECK(!ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c)) && !audio_refill_reader(client,4096));
    s->queued=AUDIO_RING*3/4;
    d->flags=O_RDONLY;
    CHECK(ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c))==-EACCES && !audio_refill_reader(client,4096));
    struct file_description probe={.node=d->node};
    CHECK(audio_device_open(&probe,O_RDONLY)==-EACCES && !probe.audio);
    d->flags=O_WRONLY|O_NONBLOCK;
    CHECK(!ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c)));
    struct task *saved=g_current, other={.generation=1,.prio=P_NORMAL,.state=T_RUNNING};
    g_current=&other; CHECK(!audio_refill_reader(client,4096)); g_current=saved;
    CHECK(audio_refill_reader(client,65536) && !audio_refill_reader(client,1));
    CHECK(!ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c))); g_ticks+=20;
    CHECK(!audio_refill_reader(client,1));
    CHECK(!ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c))); audio_stream_reset(s);
    s->started=true; CHECK(!audio_refill_reader(client,1)); /* generation withdrawn */
    int source=file_open(client,"/refill-demand.bin",O_CREAT|O_RDWR,0666); CHECK(source>=0);
    CHECK(!ctl(fd,AUDIO_REFILL_READ,&c,sizeof(c)));
    watch_source_read=true; source_urgent=false;
    CHECK(call(CIUKI_SYS_READ,source,USER,4096,0,0)==0 && source_urgent);
    g_ticks+=20; source_urgent=true;
    CHECK(call(CIUKI_SYS_READ,source,USER,4096,0,0)==0 && !source_urgent);
    watch_source_read=false;
    CHECK(!file_close(client,source) && !file_close(client,fd));
    puts("F1-87 C refill: writable started low-water stream, explicit reading thread, 64KiB/20ms bound, reset generation; native read urgency PASS");
}
static void test_stream_syscalls(void)
{
    audio.ready=false;CHECK(file_open(client,"/dev/audio",O_WRONLY,0)==-ENODEV);
    audio.ready=true;audio.error=0;audio.running=true;idle_since=0; /* fd-only fixture: transport already running */
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
    CHECK(status.queued_frames==2048 && status.transport_frames==1920 && status.latency_frames>=status.transport_frames);
    CHECK(!ctl(fd,AUDIO_GET_STATUS,&status,sizeof(status))); /* output field is not reserved input */
    unsigned drain=audio.caps.drain_ms; uint64_t played=audio.played_output;
    audio.caps.drain_ms=20; audio.played_output=1000; waiting_stream->last_output=1100;
    waiting_stream->queued=0; ac97.published=28; ac97.generation_fetched=0;
    CHECK(!ctl(fd,AUDIO_GET_STATUS,&status,sizeof(status)));
    CHECK(status.transport_frames==7168 && status.latency_frames==1060); /* excludes later idle silence */
    waiting_stream->queued=2048; waiting_stream->last_output=0;
    audio.caps.drain_ms=drain; audio.played_output=played;
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
    CHECK(audio.master.left==20&&audio.master.right==20&&!audio.master.mute);
    int mixer=file_open(client,"/dev/mixer",O_RDONLY,0);CHECK(mixer>=0);
    struct ciuki_audio_diagnostics info={.version=1,.size=64};
    ac97.codec_id=0x43525931;ac97.irqs=123;ac97.fetched=4567;
    CHECK(!ctl(mixer,MIXER_GET_DIAGNOSTICS,&info,sizeof(info)) && info.codec_id==0x43525931 && info.irqs==123 && info.fetched_frames==4567);
    audio.ready=false;audio.init_error=-ETIMEDOUT;
    CHECK(!ctl(mixer,MIXER_GET_DIAGNOSTICS,&info,sizeof(info)) && !(info.flags&CIUKI_AUDIO_DIAG_READY) && info.last_error==-ETIMEDOUT);
    info.reserved=1;CHECK(ctl(mixer,MIXER_GET_DIAGNOSTICS,&info,sizeof(info))==-EINVAL);
    info.reserved=0;info.version=2;CHECK(ctl(mixer,MIXER_GET_DIAGNOSTICS,&info,sizeof(info))==-EOPNOTSUPP);
    info.version=1;CHECK(ctl(mixer,MIXER_GET_DIAGNOSTICS,&info,60)==-EINVAL);
    audio.init_error=0;audio.ready=true;
    struct ciuki_mixer_master m={1,32,60,40,1,0,0,0};
    CHECK(ctl(mixer,MIXER_SET_MASTER,&m,32)==-EACCES);
    file_fd(client,mixer)->mixer_control=true; /* bootstrap authority is tested separately below */
    hardware_setup(&ac97);ac97.software_master=false;audio.ready=true;
    CHECK(!ctl(mixer,MIXER_SET_MASTER,&m,32)&&m.save_state==1);
    CHECK(hw.codec[2/2]==0x8305 && hw.codec[0x18/2]==0x8808);
    m=(struct ciuki_mixer_master){1,32,25,75,0,0,0,0};CHECK(!ctl(mixer,MIXER_SET_MASTER,&m,32));
    CHECK(hw.codec[2/2]==0x0802 && hw.codec[0x18/2]==0x0808);
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
#include "second_set_test.inc"
#include "late_refill_test.inc"
#include "vm_creation_test.inc"
#include "vm_paging_test.inc"
#include "thinkpad_ec_test.inc"
#include "deadline_test.inc"
#include "pipeline_test.inc"
#include "transient_test.inc"
static void registry_fixture(const char *text)
{
    static struct vfs_table table;CHECK(!vfs_table_init(&vfs,&table,2));
    int fd=vfs_open(&table,"C:\\System\\Registry\\System.reg",VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_NONE,0);
    CHECK(fd>=0);size_t n;CHECK(!vfs_write(&table,fd,text,strlen(text),&n)&&n==strlen(text));CHECK(!vfs_close(&table,fd));
    vfs_table_destroy(&table);
}
static void test_registry_master(void)
{
    CHECK(!make_dir("/System/Registry"));
    unsigned before=heap_blocks;
    registry_fixture("; ciuki-registry generation=2\n[System\\Identity]\nName=\"CiukiOS\"\n[system\\audio]\n\"mASTer\"=65561\n");
    audio_settings_load(&vfs);CHECK(audio.master.left==25&&audio.master.right==0&&audio.master.mute==1);
    CHECK(!strcmp(audio.settings_reason,"registry")&&heap_blocks==before);
    registry_fixture("[System\\Audio]\nMaster=19481\n");
    audio_settings_load(&vfs);CHECK(audio.master.left==25&&audio.master.right==76&&!audio.master.mute);
    const char *bad[]={"[System\\Audio]\nMaster=101\n", "[System\\Audio]\nMaster=-1\n",
        "[System\\Audio]\nMaster=131072\n", "[System\\Audio]\nMaster=\"20\"\n",
        "[System\\Audio]\nMaster=5140\n[System\\Other]\nBad=not_valid\n"};
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++) { registry_fixture(bad[i]);audio_settings_load(&vfs);CHECK(audio.master.left==25&&audio.master.right==75&&!audio.master.mute&&heap_blocks==before); }
    registry_fixture("[System\\Audio]\nMaster=5140\n");
    int fd=file_open(client,"/System/Registry/System.reg.txn",O_WRONLY|O_CREAT,0644);
    CHECK(fd>=0);CHECK(!file_close(client,fd));audio_settings_load(&vfs);CHECK(audio.master.left==25&&audio.master.right==75);
    CHECK(!remove_path_native("/System/Registry/System.reg.txn",false));
    audio_settings_load(&vfs);CHECK(audio.master.left==20&&audio.master.right==20);
    registry_fixture("[System\\Display\\Savage]\nEngineConfig=73\n");
    int32_t config=0;
    CHECK(kernel_registry_int(&vfs,"System\\Display\\Savage","EngineConfig",&config) && config==73);
    CHECK(!kernel_registry_int(&vfs,"System\\Display\\Savage","Missing",&config));
    CHECK(heap_blocks==before);
    puts("boot registry: shared production parser reads Savage EngineConfig integer, missing values release all storage PASS");
    CHECK(!remove_path_native("/System/Registry/System.reg",false));
    audio_settings_load(0);CHECK(audio.master.left==20&&audio.master.right==20&&!audio.master.mute);
    puts("audio registry: whole production grammar, case-folded packed master, precedence, malformed root/value and pending journal fallback PASS");
}
int main(int argc,char **argv)
{
    CHECK(argc==3);ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);
    controller.state=T_RUNNING;g_current=&controller;proc_init();
    if(getenv("CIUKI_AUDIO_COST")) { test_service_budget();test_cost_breakdown();test_cubic_cost();free(ram);return 0; }
    CHECK(!fake_open(&disk[0],argv[1])&&!fake_open(&disk[1],argv[2]));
    disk[0].undo_enabled=disk[1].undo_enabled=true;CHECK(!cache_init(&cache,0));vfs_init(&vfs);
    for(unsigned i=0;i<2;i++){CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true));CHECK(!vfs_attach(&vfs,i+2,&volumes[i]));}
    CHECK(!files_attach(&vfs,&space));client_thread=make_process(proc_supervisor(),&client);g_current=client_thread->task;g_current->state=T_RUNNING;
    client->cwd=space->root;px_retain(client->cwd);client->cwd_retain=px_retain;client->cwd_release=px_release;
    test_transient_transport();
    test_vm_creation_lead();
    test_vm_paging_replay();
    test_codec();test_physical_codec();test_diagnostic_records();test_cs4297a_codec();test_ich3_bringup();test_codec_power_policy();test_controller_reset_models();test_boot_transport();test_bdl();test_cursor_handoff();test_formats();test_shutdown();test_codec_failure_release();test_idle_tail_drain();test_idle_tail_cursor_error();test_t23_cursor_replay();test_late_refill();test_exhausted_recovery();test_publication_retry();test_recovery_failure_record();test_wide_failure_record();test_cheap_arithmetic();test_deferred_service_logs();test_fullscreen_refill_wait();test_service_budget();test_stream_syscalls();test_persistence();test_registry_master();test_thinkpad_ec();test_second_set();test_pipeline();
    test_refill_demand();
    all_closed();stop_collect(client);for(unsigned i=0;i<2;i++)CHECK(!vfs_detach(&vfs,i+2));vfs_destroy(&vfs);cache_destroy(&cache);
    for(unsigned i=0;i<2;i++){fake_reset(&disk[i]);fake_close(&disk[i]);}
    CHECK(!pages_used&&!audio.commitment_pages);printf("audio production: %u checks PASS\n",checks);free(ram);return 0;
}
