/* fx-21: a USB stick enumerated through the production UHCI path end to
 * end: usb_attach/start() builds the real schedule (interrupt QHs, control
 * QH, bulk QH, fx-17 terminating QH), the worker scans, debounces and resets
 * the root ports through PORTSC, enumerates at address 0 (GET_DESCRIPTOR,
 * SET_ADDRESS, descriptors, SET_CONFIGURATION), binds BOT over bulk and
 * mounts E:, then reads 64 KiB through the fx-17 reclamation loop. Every
 * model millisecond walks the controller's own frame list with the strict
 * UHCI 1.1 walker (uhci_model.h).
 *
 * The register model follows UHCI 1.1 section 2.1.7 / ICH3-M 11.2.7: PE is
 * written by software only, a port in reset or disabled forwards no SOF.
 * Devices follow USB 2.0 section 7.1.7.6: more than 3 ms of idle bus means
 * Suspend. The T23 stick of the f441c5bd trial (05dc:a81d), attached at
 * boot and configured by the BIOS, was reset, left 9 ms without PE (a 1 ms
 * task sleep stretched by boot load), and then ACKed every SETUP and NAKed
 * every data stage at address 0 for six attempts and a second port reset.
 * The model gives such a stick exactly that observed behaviour when it
 * suspends in the Default state before its first SOF; FreeBSD
 * uhci_portreset() documents such devices ("else some USB devices fail to
 * attach"). This is the hypothesis consistent with every T23 record: 35
 * boots with a 1-2 ms gap enumerated it, and a hot-plugged (never
 * configured) one survived a 34 ms gap in 445e91c7, so a hot-plugged stick
 * does not wedge here. Its control timing otherwise follows the e831e685
 * trial (data stage after 4 ms of NAKs, a 60 ms status stage on the first
 * request, 10 ms SET_ADDRESS status).
 * SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "storage_test.c"
#undef main

#include <ciuki/media_events.h>
static unsigned media_notifications;
void media_state_changed(void) { media_notifications++; }

uint32_t fs_host_clock_ms(void) { return (uint32_t)m.now; }

/* Production usb_core.c/usb_enumerate references outside this fixture. */
static struct resource leases[16];
static unsigned lease_count;
int registry_claim(enum res_type t,uint32_t s,uint32_t e,const char *o,bool share)
{
    assert(s<e && lease_count<ARRAY_SIZE(leases));
    leases[lease_count]=(struct resource){t,s,e,o,gen_alloc(),RS_CLAIMED,share};
    return (int)lease_count++;
}
const struct resource *registry_get(unsigned i) { return i<lease_count ? &leases[i] : NULL; }
int registry_activate(int h,gen_t g) { assert(leases[h].generation==g); leases[h].state=RS_ACTIVE; return 0; }
bool pci_irq_routed(const struct pci_func *p) { (void)p; return true; }
int irq_chain_add(unsigned irq,irq_chain_handler_t handler,const char *owner)
{ assert(irq==11 && handler && owner); return 0; }
bool drivers_input_firmware(void) { return false; }
int usb_hid_attach(struct usb_device *d,struct usb_hid *h,unsigned n) { (void)d;(void)h;(void)n;assert(!"no HID here");return -USB_EIO; }
int usb_hub_attach(struct usb_device *d) { (void)d;assert(!"no hub here");return -USB_EIO; }
int usb_hub_reset(struct usb_device *d,unsigned p) { (void)d;(void)p;assert(!"no hub here");return -USB_EIO; }
void usb_hub_scan(struct usb_device *d) { (void)d;assert(!"no hub here"); }
void usb_hid_outputs(struct usb_device *d) { (void)d; }
void usb_hid_recover(struct usb_device *d) { (void)d; }

enum { FX_PORTS=2, FX_STICK=1 };
struct fx_dev {
    bool present, stick, in_reset, post_reset, suspended, wedged, wedge_on_post_reset_suspend;
    bool bios_configured; /* attached at boot: POST enumerated and configured it */
    unsigned address, pending_address, config, idle_ms, setups, nak_tokens;
    unsigned slow_ms; /* fx-21b: every standard request's data/status ready after this */
    uint8_t setup[8], data[64];
    unsigned stage, at, length; /* stage: 0 idle, 1 data, 2 status */
    bool local; /* standard request answered by this layer, not storage_test.c */
    uint64_t data_ready_at, status_ready_at;
    unsigned frame_ep0_tokens, max_ep0_tokens_frame, requests;
};
static struct fx {
    unsigned cmd, sts, intr, flbase, sofmod, legsup, pci_command;
    int frame_base;
    unsigned portsc[FX_PORTS];
    struct fx_dev dev[FX_PORTS];
    /* Trial load: the worker's next sleep after the stick's reset release
     * overshoots by this many ms (f441c5bd: release tick 2387, PE 2396). */
    unsigned release_overshoot, pending_overshoot, releases;
    uint64_t released_at;
    unsigned release_to_enable_ms;
    bool enabled_after_release;
    unsigned frame_bulk_tokens, max_bulk_tokens_frame;
    bool count_bulk;
    uint64_t quiesce_at; /* fx-21b: usb_power_quiesce() raises the host flag then */
} fx;

static unsigned fx_frame(void) { return (unsigned)((int64_t)m.now+fx.frame_base)&0x7ff; }
static unsigned fx_port(unsigned p)
{
    unsigned v=fx.portsc[p]|0x80;
    if((v&1) && !(v&0x200)) v|=0x10; /* full-speed J: D+ high */
    return v;
}
static uint32_t fx_read(void *c,unsigned off,unsigned width)
{
    (void)c;
    switch(off) {
    case 0: assert(width==2);return fx.cmd;
    case 2: assert(width==2);return fx.sts|((fx.cmd&1) ? 0:0x20);
    case 4: assert(width==2);return fx.intr;
    case 6: assert(width==2);return fx_frame();
    case 8: assert(width==4);return fx.flbase;
    case 0x0c: assert(width==1);return fx.sofmod;
    case 0x10: case 0x12: assert(width==2);return fx_port((off-0x10)/2);
    }
    assert(!"unexpected UHCI register read");return 0;
}
static void fx_write(void *c,unsigned off,uint32_t value,unsigned width)
{
    (void)c;
    switch(off) {
    case 0:
        assert(width==2);
        if(value&2) {
            /* HCRESET: stopped, ports disabled, attached devices reappear
             * with CCS/CSC. The devices themselves are not reset. */
            fx.cmd=0;fx.sts=0;fx.intr=0;
            for(unsigned p=0;p<FX_PORTS;p++) fx.portsc[p]=fx.dev[p].present ? 3u:0u;
            return;
        }
        fx.cmd=value&0xffu;return;
    case 2: assert(width==2);fx.sts&=~value;return;
    case 4: assert(width==2);fx.intr=value;return;
    case 6: assert(width==2);fx.frame_base=(int)value-(int)m.now;return;
    case 8: assert(width==4);fx.flbase=value;return;
    case 0x0c: assert(width==1);fx.sofmod=value;return;
    case 0x10: case 0x12: {
        assert(width==2 && !(value&0xe000));
        unsigned p=(off-0x10)/2, old=fx.portsc[p];
        struct fx_dev *d=&fx.dev[p];
        unsigned v=old&~(value&0x80au); /* W1C */
        if((value&0x200) && !(old&0x200)) {
            v|=0x200;v&=~4u;d->in_reset=true;d->suspended=false; /* a reset wakes a suspended device */
            d->address=0;d->config=0;d->stage=0;
        }
        if(!(value&0x200) && (old&0x200)) {
            v&=~0x200u;d->in_reset=false;d->post_reset=true;d->idle_ms=0;
            if(p==FX_STICK) {
                fx.released_at=m.now;fx.enabled_after_release=false;
                if(!fx.releases++) fx.pending_overshoot=fx.release_overshoot;
            }
        }
        /* PE: software only, for a connected port out of reset. */
        if((value&4) && (v&1) && !(v&0x200)) {
            if(!(old&4) && p==FX_STICK && !fx.enabled_after_release) {
                fx.release_to_enable_ms=(unsigned)(m.now-fx.released_at);fx.enabled_after_release=true;
            }
            v|=4;
        } else if(!(value&4)) v&=~4u;
        v=(v&~0x1040u)|(value&0x1040u); /* RD/suspend as written */
        fx.portsc[p]=v;return;
    }
    }
    assert(!"unexpected UHCI register write");
}
static uint32_t fx_config_read(void *c,unsigned off,unsigned width)
{
    (void)c;
    if(off==4 && width==2) return fx.pci_command;
    if(off==4 && width==4) return fx.pci_command; /* status: no capability list */
    if(off==0xc0 && width==2) return fx.legsup;
    assert(!"unexpected PCI config read");return 0;
}
static void fx_config_write(void *c,unsigned off,uint32_t value,unsigned width)
{
    (void)c;assert(width==2);
    if(off==4) {fx.pci_command=value;return;}
    if(off==0xc0) {fx.legsup=value&0x20bf;return;}
    assert(!"unexpected PCI config write");
}

static const uint8_t stick_device[18]={18,1,0x00,0x02,0,0,0,64,0xdc,0x05,0x1d,0xa8,0x00,0x01,1,2,3,1};
static const uint8_t stick_config[32]={9,2,32,0,1,1,0,0x80,50, 9,4,0,0,2,8,6,0x50,0,
    7,5,0x81,2,64,0,0, 7,5,0x02,2,64,0,0};
static const uint8_t vendor_device[18]={18,1,0x10,0x01,0,0,0,8,0x6d,0x04,0x2b,0xc5,0x00,0x01,0,0,0,1};
static const uint8_t vendor_config[18]={9,2,18,0,1,1,0,0x80,50, 9,4,0,0,0,0xff,0,0,0};

static struct fx_dev *fx_target(unsigned address)
{
    for(unsigned p=0;p<FX_PORTS;p++) {
        struct fx_dev *d=&fx.dev[p];
        if((fx.portsc[p]&4) && d->present && !d->in_reset && !d->suspended && d->address==address) return d;
    }
    return NULL;
}
/* Standard requests at this layer; class requests and bulk go to the
 * storage_test.c BOT medium (packet()). -USB_EAGAIN is a NAK. */
static int fx_control(struct fx_dev *d,unsigned pid,unsigned toggle,uint8_t *b,unsigned n)
{
    if(pid==USB_PID_SETUP) {
        /* A device ACKs every SETUP (USB 2.0 section 8.5.3), wedged or not. */
        assert(n==8);memcpy(d->setup,b,8);d->setups++;d->requests++;
        unsigned type=b[0],req=b[1],value=b[2]|b[3]<<8,length=b[6]|b[7]<<8;
        d->local=!(type&0x60);
        d->at=0;d->stage=length ? 1:2;
        /* e831e685 trial timing: data after 4 ms of NAKs; the first
         * request's status stage NAKed 60 ms; SET_ADDRESS status 10 ms. */
        d->data_ready_at=m.now+(d->stick ? 4:0);
        d->status_ready_at=m.now+(d->stick ? (d->requests==1 ? 60u : req==5 ? 10u : 1u) : 0u);
        if(d->slow_ms) d->data_ready_at=d->status_ready_at=m.now+d->slow_ms;
        if(d->local) {
            if(req==6 && (value>>8)==1) {
                d->length=length<18 ? length:18;memcpy(d->data,d->stick ? stick_device:vendor_device,18);
            } else if(req==6 && (value>>8)==2) {
                unsigned size=d->stick ? sizeof(stick_config):sizeof(vendor_config);
                d->length=length<size ? length:size;memcpy(d->data,d->stick ? stick_config:vendor_config,size);
            } else if(req==5) {assert(!length && value && value<128);d->pending_address=value;}
            else if(req==9) {assert(!length);}
            else if(req==1 && d->stick) {d->local=false;} /* CLEAR_FEATURE(HALT): BOT recovery */
            else assert(!"unexpected standard request");
        }
        if(!d->local) {assert(d->stick);return packet(device(),0,pid,toggle,b,n,false);}
        return 8;
    }
    if(d->wedged) {d->nak_tokens++;return -USB_EAGAIN;}
    if(!d->local) return packet(device(),0,pid,toggle,b,n,false);
    if(d->stage==1) {
        assert(pid==USB_PID_IN);
        if(m.now<d->data_ready_at) {d->nak_tokens++;return -USB_EAGAIN;}
        unsigned take=d->length-d->at;if(take>n)take=n;
        memcpy(b,d->data+d->at,take);d->at+=take;
        if(d->at==d->length || take<n) d->stage=2;
        return (int)take;
    }
    assert(d->stage==2 && !n);
    assert(pid==((d->setup[0]&0x80) ? USB_PID_OUT:USB_PID_IN));
    if(m.now<d->status_ready_at) {d->nak_tokens++;return -USB_EAGAIN;}
    d->stage=0;
    if(d->setup[1]==5) d->address=d->pending_address; /* after the status stage, USB 2.0 9.4.6 */
    if(d->setup[1]==9) d->config=d->setup[2];
    return 0;
}
static volatile uint32_t *fx_dma(void *c,uint32_t address,unsigned bytes)
{
    (void)c;assert(address>=m.h.dma_phys && address-m.h.dma_phys<=USB_DMA_BYTES-bytes);
    return (volatile uint32_t *)(m.dma+(address-m.h.dma_phys));
}
static bool fx_transaction(void *c,uint32_t address,uint32_t qh,volatile uint32_t *td)
{
    (void)c;(void)address;(void)qh;
    unsigned token=td[2],pid=token&255,addr=(token>>8)&127,ep=(token>>15)&15,toggle=(token>>19)&1;
    unsigned n=(((token>>21)&0x7ff)+1)&0x7ff;
    assert(n<=64 && pid!=0 && addr!=127);
    struct fx_dev *d=fx_target(addr);
    int got;
    if(!d) got=-USB_ETIMEDOUT;
    else if(!ep) {
        d->frame_ep0_tokens++;
        got=fx_control(d,pid,toggle,n ? (uint8_t *)fx_dma(NULL,td[3],n):NULL,n);
    } else {
        assert(d->stick && d->config);
        if(fx.count_bulk) fx.frame_bulk_tokens++;
        got=d->wedged ? -USB_EAGAIN : packet(device(),ep,pid,toggle,(uint8_t *)fx_dma(NULL,td[3],n),n,false);
    }
    if(got==-USB_EAGAIN) {td[1]|=1u<<19;return false;}
    td[1]&=~(0xffu<<16|0x7ffu);
    if(got==-USB_EPIPE) {td[1]|=1u<<22;return false;}
    if(got<0) {td[1]=(td[1]&~(3u<<27))|(1u<<22)|(1u<<18);return false;} /* C_ERR exhausted */
    td[1]|=((unsigned)got-1)&0x7ff;
    return true;
}
/* One model millisecond: SOF to every enabled port, the frame's schedule,
 * then the USB 2.0 idle/suspend rule for every disabled port. */
static void fx_tick(void)
{
    m.now++;
    if(fx.quiesce_at && m.now>=fx.quiesce_at) m.h.quiescing=true;
    bool running=(fx.cmd&1) && (fx.pci_command&4);
    for(unsigned p=0;p<FX_PORTS;p++) {
        struct fx_dev *d=&fx.dev[p];
        if(!d->present || d->in_reset) continue;
        if(running && (fx.portsc[p]&4)) {d->idle_ms=0;d->post_reset=false;d->suspended=false;}
    }
    if(running) {
        for(unsigned p=0;p<FX_PORTS;p++) fx.dev[p].frame_ep0_tokens=0;
        fx.frame_bulk_tokens=0;
        uint32_t status=0;
        struct uhci_model_io io={NULL,fx_dma,fx_transaction,&status,NULL,
            m.no_reclamation ? m.h.dma_phys+USB_UHCI_TERM_QH : 0};
        assert(fx.flbase==m.h.dma_phys);
        assert(!uhci_model_frame(&io,((uint32_t *)m.dma)[fx_frame()&1023]));
        fx.sts|=status;
        for(unsigned p=0;p<FX_PORTS;p++) {
            struct fx_dev *d=&fx.dev[p];
            if(d->frame_ep0_tokens>d->max_ep0_tokens_frame) d->max_ep0_tokens_frame=d->frame_ep0_tokens;
        }
        if(fx.frame_bulk_tokens>fx.max_bulk_tokens_frame) fx.max_bulk_tokens_frame=fx.frame_bulk_tokens;
        if(m.h.irq_attached && (fx.sts&fx.intr&0x0f)) assert(usb_host_irq(&m.h));
    }
    for(unsigned p=0;p<FX_PORTS;p++) {
        struct fx_dev *d=&fx.dev[p];
        if(!d->present || d->in_reset || (running && (fx.portsc[p]&4))) continue;
        if(++d->idle_ms>3 && !d->suspended) {
            d->suspended=true;
            if(d->post_reset && d->wedge_on_post_reset_suspend && d->bios_configured) d->wedged=true;
        }
    }
}
static void fx_sleep(void *c,unsigned ms)
{
    (void)c;
    ms+=fx.pending_overshoot;fx.pending_overshoot=0;
    while(ms--) fx_tick();
}
static uint64_t fx_now(void *c) { (void)c; return m.now; }

static unsigned record_count(const char *needle)
{
    unsigned n=0;
    for(const char *at=records;(at=strstr(at,needle));at++) n++;
    return n;
}
static void fx_begin(const char *path,unsigned overshoot)
{
    setup(path,false); /* image, MBR-less FAT, storage_setup(); host fields reset below */
    fs_lock_destroy(&m.h.transfer_lock);
    memset(&m.h,0,sizeof(m.h));memset(&fx,0,sizeof(fx));lease_count=0;
    m.storage_slot=1; /* port 1's device enumerates first */
    fx.dev[0]=(struct fx_dev){.present=true,.suspended=true};
    fx.dev[1]=(struct fx_dev){.present=true,.stick=true,.suspended=true,.wedge_on_post_reset_suspend=true,.bios_configured=true};
    fx.portsc[0]=fx.portsc[1]=3;fx.pci_command=0x0005;fx.legsup=0x2f00;fx.release_overshoot=overshoot;
    struct pci_func p={.vendor=0x8086,.device=0x2484,.class_code=0x0c,.subclass=3,.prog_if=0,
        .irq_line=11,.irq_pin=2,.command=0x0005,.dev=0x1d,.fn=1};
    p.bar[4]=0x1821;
    struct usb_io io={NULL,fx_read,fx_write,fx_config_read,fx_config_write,fx_now,fx_sleep};
    assert(!usb_attach(&m.h,&p,&io,m.dma,0x100000));
    /* usb_core.c keeps USB_HOST_MAX IRQ slots for the process lifetime:
     * the first four fixtures run in IRQ mode (as the T23), later ones poll. */
    static unsigned attaches;
    attaches++;
    assert(m.h.ready && (m.h.irq_attached || attaches>USB_HOST_MAX) && fx.flbase==m.h.dma_phys);
    /* start() built the fx-17 tail: bulk QH -> terminating QH, whose element
     * is the inactive pseudo TD (fx-21: IN to address 127) and whose
     * horizontal link terminates while no bulk stage runs. */
    volatile uint32_t *bulk=fx_dma(NULL,m.h.dma_phys+USB_UHCI_BULK_QH,8);
    volatile uint32_t *term=fx_dma(NULL,m.h.dma_phys+USB_UHCI_TERM_QH,8);
    volatile uint32_t *tail=fx_dma(NULL,m.h.dma_phys+USB_TAIL,16);
    assert(bulk[0]==((m.h.dma_phys+USB_UHCI_TERM_QH)|2) && term[0]==1 && term[1]==m.h.dma_phys+USB_TAIL);
    assert(tail[0]==1 && !(tail[1]&(1u<<23)));
    fx_tick();
}
static void fx_end(void) { cleanup(); }
/* The worker's cadence: a scan is due every 250 ms. */
static void fx_scan(unsigned ms)
{
    for(unsigned elapsed=0;elapsed<ms;elapsed+=10) {fx_sleep(NULL,10);usb_step(&m.h);}
}
static uint64_t record_tick(const char *needle)
{
    const char *at=strstr(records,needle);assert(at);
    const char *tick=strstr(at,"tick=");assert(tick && tick<strchr(at,'\n'));
    return strtoull(tick+5,NULL,10);
}
/* Physical removal and insertion on port 2 (CCS/CSC as UHCI 1.1 2.1.7). A
 * re-plugged stick is freshly powered: not BIOS-configured, not wedged. */
static void fx_unplug(void)
{
    fx.dev[FX_STICK].present=false;fx.portsc[FX_STICK]=(fx.portsc[FX_STICK]&~5u)|2u;
}
static void fx_plug_healthy(bool csc_only)
{
    fx.dev[FX_STICK]=(struct fx_dev){.present=true,.stick=true,.suspended=true};
    fx.portsc[FX_STICK]=csc_only ? (fx.portsc[FX_STICK]|3u) : 3u;
    m.stage=0;m.cbw_at=0;m.in_toggle=m.out_toggle=0;
}
static void assert_mounted(unsigned slot)
{
    m.storage_slot=slot;struct usb_device *stick=device();
    assert(stick->configured && stick->storage.online && stick->storage.published && !stick->quarantined);
    struct storage *s=storage_get();assert(s->vfs.volumes[4] && s->volumes[4].part.parent==&stick->storage.block);
}
/* fx-21b 1: a slow but valid stick. Each standard request of its enumeration
 * answers after 1.5 s: within Linux's 5 s per message, beyond the former
 * 500 ms per stage (which gave six -110 and a quarantine). */
static void slow_stick(const char *path)
{
    fx_begin(path,0);fx.dev[FX_STICK].slow_ms=1500;
    uint32_t before=usb_unrecognized_devices();
    usb_step(&m.h);
    assert(fx.dev[FX_STICK].address==2 && fx.dev[FX_STICK].config==1);
    assert(strstr(records,"port=2 parent=0 attempt=1 tick=") && !record_count("result=-110"));
    unsigned long long elapsed=0;const char *line=strstr(records,"port=2 parent=0 attempt=1 tick=");
    assert(line && sscanf(strstr(line,"elapsed_ms="),"elapsed_ms=%llu",&elapsed)==1 && elapsed>=1500 && elapsed<5000);
    assert_mounted(1);
    assert(usb_unrecognized_devices()==before && !record_count("[usb] unrecognized"));
    printf("fx-21b slow stick: first GET_DESCRIPTOR(8) %llu ms, enumerated and mounted\n",elapsed);
    fx_end();
}
/* fx-21b 2/3: a dead stick (SETUP ACKed, every data stage NAKed, as the
 * f441c5bd stick). Worst case to quarantine: 2 series x 3 attempts of one
 * 5000 ms message (+ <=20 ms two-frame retirement each), 2 x 2 pauses of
 * 100 ms and one 60 ms root reset: 6 x 5020 + 400 + 60 = 30580 ms from the
 * first attempt (plus the debounce, at most 2 s, and the first 60 ms reset).
 * Exactly one unrecognized count; a re-plug re-enumerates the port. */
static void dead_stick(const char *path,bool fast_replug)
{
    fx_begin(path,0);fx.dev[FX_STICK].wedged=true;
    unsigned notifications=media_notifications;
    uint32_t namespace_before[26];memcpy(namespace_before,storage_get()->vfs.generation,sizeof(namespace_before));
    uint32_t before=usb_unrecognized_devices();
    usb_step(&m.h);
    assert(record_count("port=2 parent=0 attempt=")==12 && record_count("result=-110")==6);
    uint64_t first=record_tick("port=2 parent=0 attempt=1 series=1 operation=1 tick=");
    uint64_t quarantined=record_tick("[usb] unrecognized controller=");
    assert(quarantined-first<=30580);
    assert(media_notifications==notifications+1);
    assert(!memcmp(namespace_before,storage_get()->vfs.generation,sizeof(namespace_before)));
    assert(strstr(records,"[usb] quarantine device addr=0 reason=enumeration_transfer stage=enumeration"));
    assert(record_count("[usb] unrecognized ")==1 && usb_unrecognized_devices()==before+1);
    assert(strstr(records,"reason=enumeration_transfer count=") && strstr(records,"notify=desktop"));
    assert(m.h.devices[FX_STICK].quarantined && m.h.root_blocked[FX_STICK]);
    printf("fx-21b dead stick: quarantined %llu ms after the first attempt (bound 30580)\n",(unsigned long long)(quarantined-first));
    /* Still connected: no second notification however long it stays. */
    fx_scan(3000);
    assert(media_notifications==notifications+1); /* quarantined port stays quiet */
    assert(record_count("[usb] unrecognized ")==1 && usb_unrecognized_devices()==before+1);
    assert(record_count("port=2 parent=0 attempt=")==12 && m.h.root_blocked[FX_STICK]);
    if(fast_replug) {
        /* Unplugged and plugged back between two 250 ms scans: CCS stays 1,
         * CSC reports the connect change. */
        fx_plug_healthy(true);
        fx_scan(300);
        assert(strstr(records,"[usb] release port=2") && strstr(records,"reason=reconnect"));
    } else {
        fx_unplug();fx_scan(300);
        assert(strstr(records,"[usb] release port=2") && strstr(records,"reason=disconnect"));
        assert(!m.h.root_blocked[FX_STICK] && !m.h.root_child[FX_STICK]);
        fx_plug_healthy(false);
    }
    fx_scan(3000);
    assert(m.h.device_count==3 && fx.dev[FX_STICK].address==2);
    assert_mounted(2);
    assert(record_count("[usb] unrecognized ")==1 && usb_unrecognized_devices()==before+1);
    fx_end();
}
/* fx-21b: a shutdown during a dead device's 5 s wait. usb_power_quiesce()
 * flags the host first; the message ends at once and the device is not
 * counted as unrecognized (the 2 s quiesce reservation bound still holds). */
static void quiesce_abort(const char *path)
{
    fx_begin(path,0);fx.dev[FX_STICK].wedged=true;
    uint32_t before=usb_unrecognized_devices();
    fx.quiesce_at=m.now+1500; /* debounce and the first attempt are under way */
    uint64_t start=m.now;
    usb_step(&m.h);
    assert(m.now-start<fx.quiesce_at-start+500);
    assert(m.h.devices[FX_STICK].quarantined && usb_unrecognized_devices()==before && !record_count("[usb] unrecognized"));
    m.h.quiescing=false;fx_end();
}

/* The stick enumerates, binds and mounts; the 64 KiB read keeps fx-17's
 * in-frame bulk reclamation. `overshoot` is the trial's scheduler load. */
static void enumerate_stick(const char *path,unsigned overshoot)
{
    fx_begin(path,overshoot);
    usb_step(&m.h);
    struct usb_device *vendor=&m.h.devices[0],*stick=device();
    /* Port 1's fast device answered at address 0 on the same schedule. */
    assert(m.h.device_count==2 && vendor->address==1 && vendor->configured && fx.dev[0].address==1);
    /* Before fx-21 the trial replay ends here: "[usb] addr0 uhci_packet ...
     * pid=69 ... td=198807ff", six "result=-110", "[usb] quarantine device
     * addr=0 reason=enumeration_transfer stage=enumeration". */
    assert(!fx.dev[FX_STICK].wedged && !record_count("result=-110"));
    assert(stick->address==2 && stick->configured && fx.dev[FX_STICK].address==2 && fx.dev[FX_STICK].config==1);
    assert(stick->storage.online && stick->storage.published && !stick->quarantined);
    assert(strstr(records,"[usb] device addr=2 vid:pid=05dc:a81d class=00 speed=full"));
    struct storage *s=storage_get();assert(s->vfs.volumes[4] && s->volumes[4].part.parent==&stick->storage.block);
    /* fx-21: PE was written in the reset-release section; the scheduler
     * overshoot landed after it, in TRSTRCY, where the port sends SOFs. */
    assert(fx.releases==1 && fx.enabled_after_release && fx.release_to_enable_ms==0);
    assert(strstr(records,"[usb] port_reset port=2") && strstr(records,"release_to_enable_frames=0 connected=1 result=0"));
    /* The pseudo TD is Linux's term_td (null IN to address 127); the HC
     * never executed it (fx_transaction refuses address 127). */
    volatile uint32_t *tail=fx_dma(NULL,m.h.dma_phys+USB_TAIL,16);
    assert(tail[2]==((0x7ffu<<21)|(0x7fu<<8)|USB_PID_IN) && !(tail[1]&(0xffu<<16)));
    /* No reclamation loop during control transfers (Linux 630aa3cfd5f0, "No
     * FSBR until device is configured"): the NAKed control TD is retried
     * once per frame, as on e831e685. */
    assert(fx.dev[FX_STICK].nak_tokens && fx.dev[FX_STICK].max_ep0_tokens_frame==1);
    assert(fx.dev[0].max_ep0_tokens_frame==1);
    volatile uint32_t *term=fx_dma(NULL,m.h.dma_phys+USB_UHCI_TERM_QH,8);assert(term[0]==1);
    /* fx-17 throughput through the same schedule. */
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    uint64_t frames[2];
    for(unsigned loop=0;loop<2;loop++) {
        m.no_reclamation=!loop;m.nak_every=8;m.nak_seen=0;fx.count_bulk=true;fx.max_bulk_tokens_frame=0;
        uint64_t start=m.now;
        assert(!block()->read(block(),256,128,buf) && !memcmp(buf,m.image+256*512,BLKDEV_READ_BYTES));
        frames[loop]=m.now-start;
        if(loop) assert(fx.max_bulk_tokens_frame>1);
        assert(term[0]==1); /* the loop is closed only inside a bulk stage */
    }
    m.no_reclamation=false;m.nak_every=0;fx.count_bulk=false;free(buf);
    printf("fx-21 64 KiB READ(10) after enumeration, NAK every 8 packets: %llu frames without reclamation, %llu with the fx-17 loop\n",
           (unsigned long long)frames[0],(unsigned long long)frames[1]);
    assert(frames[1]*10<=frames[0]*6 && frames[1]<=80);
    assert(!stick->storage.block.quarantined && !m.resets);
    fx_end();
}

int main(int argc,char **argv)
{
    assert(argc==2);
    /* f441c5bd trial load: the worker's next sleep after the reset release
     * overshoots by 8 ms. Before fx-21 PE followed that sleep (9 ms without
     * SOF): the stick suspended in the Default state, ACKed every SETUP and
     * NAKed every data stage (six -110 attempts, quarantine). */
    enumerate_stick(argv[1],8);
    puts("PASS fx-21 T23 f441c5bd replay: a 9 ms scheduler stretch after the reset release no longer leaves the port without SOF; the stick enumerates and mounts");
    /* Unloaded boot, as on e831e685. */
    enumerate_stick(argv[1],0);
    puts("PASS fx-21 stick enumerated through the full UHCI schedule (addr 0, SET_ADDRESS, descriptors, configuration), BOT bound and E: mounted, 64 KiB read with the fx-17 reclamation loop");
    slow_stick(argv[1]);
    puts("PASS fx-21b slow stick: every enumeration request answered after 1.5 s (Linux 5 s per message) enumerates and mounts, no notification");
    dead_stick(argv[1],false);
    puts("PASS fx-21b dead stick: quarantined within the 30.58 s bound with exactly one unrecognized count; unplug, replug: enumerated and mounted");
    dead_stick(argv[1],true);
    puts("PASS fx-21b dead stick, fast re-plug (connect change between scans): port released, enumerated and mounted");
    quiesce_abort(argv[1]);
    puts("PASS fx-21b power quiesce ends a 5 s enumeration wait at once without an unrecognized count");
    puts("PASS fx-21 UHCI enumeration suite (ASan/UBSan)");
    return 0;
}
