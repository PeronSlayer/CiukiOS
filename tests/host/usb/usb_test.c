/* Synthetic register/DMA controllers execute production packet engines.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/input.h>
#include <ciuki/storage.h>
struct storage *storage_get(void) { return NULL; }
int storage_add_removable(struct storage *s,struct blkdev *b) { (void)s;(void)b;assert(0);return -FS_EIO; }
int storage_remove_removable(struct storage *s,struct blkdev *b,bool surprise) { (void)s;(void)b;(void)surprise;assert(0);return -FS_EIO; }
int storage_remove_removable_locked(struct storage *s,struct blkdev *b,bool surprise) { return storage_remove_removable(s,b,surprise); }
struct ciuki_boot_info g_boot;
static struct resource leases[64];
static unsigned lease_count, generation;
static unsigned claims, writes, events, key_downs, key_ups, repeats, wheel_events;
static unsigned held[513], buttons[3];
static int last_x,last_y,last_wheel;
static unsigned connect_records,disconnect_records,reset_records,enable_records;
static unsigned descriptor_starts,descriptor_results,descriptor_failures;
static unsigned first_scans,cadence_records,retired_packets,service_entries,service_queues;
static unsigned hub_connects,hub_disconnects,hub_resets,hub_enumerations,hub_failures;
static unsigned usb_devices,usb_attaches,usb_detaches;
static uint64_t root_record_at[USB_PORT_MAX],hub_record_at[128][USB_PORT_MAX];
static unsigned root_records[USB_PORT_MAX],hub_records[128][USB_PORT_MAX];
static unsigned root_quarantine_records,hub_quarantine_records;
static unsigned hid_reports[128],hid_polls[128],hid_pipes[128],usb_key_records,f12_down_records,f12_up_records;
static int hid_poll_result;
#ifndef USB_LATENCY_QUEUE
void input_devices_snapshot(struct input_devices *out) { *out=(struct input_devices){.usb=usb_devices}; }
void input_devices_changed(enum input_source source,bool attached)
{
    assert(source==INPUT_USB);
    if(attached) {usb_devices++;usb_attaches++;}
    else {assert(usb_devices);usb_devices--;usb_detaches++;}
}
#endif
gen_t gen_alloc(void) { return ++generation; }
int registry_claim(enum res_type t,uint32_t s,uint32_t e,const char *o,bool share)
{
    assert(s<e && lease_count<64); claims++;
    leases[lease_count]=(struct resource){t,s,e,o,++generation,RS_CLAIMED,share};
    return (int)lease_count++;
}
const struct resource *registry_get(unsigned i) { return i<lease_count ? &leases[i] : NULL; }
int registry_activate(int h,gen_t g) { assert(leases[h].generation==g); leases[h].state=RS_ACTIVE; return 0; }
bool registry_valid(int h,gen_t g) { return leases[h].generation==g && leases[h].state==RS_ACTIVE; }
int registry_quarantine(int h,gen_t g) { assert(leases[h].generation==g); leases[h].state=RS_QUARANTINED; return 0; }
static void (*record_hook)(const char *);
void klog_periodic(const char *s, ...) __attribute__((alias("klog")));
void klog(const char *s,...)
{
    char record[256];va_list ap;va_start(ap,s);int len=kvsnprintf(record,sizeof(record),s,ap);va_end(ap);
    assert(len>=0 && len<=254);
    if(record_hook)record_hook(record);
    if(strstr(record,"[usb] port=")) {
        assert(strstr(record,"status=") && strstr(record,"tick=") && strstr(record,"result="));
        unsigned p;unsigned long long tick;
        assert(sscanf(record,"[usb] port=%u",&p)==1 && p && p<=USB_PORT_MAX);
        assert(sscanf(strstr(record,"tick="),"tick=%llu",&tick)==1);p--;
        if(root_records[p]) assert(tick-root_record_at[p]>=1000);
        root_records[p]++;root_record_at[p]=tick;
        if(strstr(record,"event=connect reason=quarantined")) root_quarantine_records++;
        if(strstr(record,"event=connect")) connect_records++;
        if(strstr(record,"event=disconnect")) disconnect_records++;
        if(strstr(record,"event=reset")) reset_records++;
        if(strstr(record,"event=enable")) enable_records++;
    }
    if(strstr(record,"[usb] addr0 get_descriptor")) {
        if(strstr(record,"phase=start")) descriptor_starts++;
        if(strstr(record,"result=")) {
            descriptor_results++;assert(strstr(record,"elapsed_ms="));
            if(strstr(record,"result=-")) descriptor_failures++;
        }
    }
    if(strstr(record,"first_scan_tick=")) first_scans++;
    if(strstr(record,"scans_per_second=")) {cadence_records++;assert(strstr(record,"elapsed_ms="));}
    if(strstr(record,"addr0 uhci_packet")) {
        assert(strstr(record,"td=") && strstr(record,"frame=") && strstr(record,"retire_result="));retired_packets++;
    }
    if(strstr(record,"service=started")) service_entries++;
    if(strstr(record,"service=queued")) service_queues++;
    unsigned addr=0;
    if(sscanf(record,"[usb] hid report addr=%u",&addr)==1) {
        assert(addr<128 && strstr(record,"len=") && strstr(record,"bytes=") && strstr(record,"interface="));hid_reports[addr]++;
    }
    if(sscanf(record,"[usb] hid poll addr=%u",&addr)==1) {
        assert(addr<128 && strstr(record,"toggle=") && strstr(record,"maxlen=") && strstr(record,"result="));hid_polls[addr]++;
        assert(sscanf(strstr(record,"result="),"result=%d",&hid_poll_result)==1);
    }
    if(sscanf(record,"[usb] hid pipe addr=%u",&addr)==1) {
        assert(addr<128 && strstr(record,"td=") && strstr(record,"token=") && strstr(record,"poll_frames=") && strstr(record,"retire_result="));hid_pipes[addr]++;
        if(hid_poll_result==-USB_EAGAIN) {
            unsigned poll_frames,frames,valid;
            int retired;
            assert(sscanf(strstr(record,"valid="),"valid=%u",&valid)==1 && valid);
            assert(sscanf(strstr(record,"poll_frames="),"poll_frames=%u frames=%u",&poll_frames,&frames)==2);
            assert(sscanf(strstr(record,"retire_result="),"retire_result=%d",&retired)==1);
            /* Preserve the old synchronous NAK retirement assertion on the
             * completed private-pipe snapshot, before the next poll arms. */
            assert(!retired && poll_frames>=2 && frames>=poll_frames+2);
        }
    }
    if(strstr(record,"[input] usb key")) {
        assert(strstr(record,"usage=") && strstr(record,"set1=") && strstr(record,"down="));usb_key_records++;
        if(strstr(record,"usage=45 set1=058 down=1")) f12_down_records++;
        if(strstr(record,"usage=45 set1=058 down=0")) f12_up_records++;
    }
    if(strstr(record,"[usb] hub=") && strstr(record," port=")) {
        assert(strstr(record,"status=") && strstr(record,"change=") && strstr(record,"tick=") && strstr(record,"result="));
        unsigned hub,p;unsigned long long tick;
        assert(sscanf(record,"[usb] hub=%u port=%u",&hub,&p)==2 && hub<128 && p && p<=USB_PORT_MAX);
        assert(sscanf(strstr(record,"tick="),"tick=%llu",&tick)==1);p--;
        if(hub_records[hub][p]) assert(tick-hub_record_at[hub][p]>=1000);
        hub_records[hub][p]++;hub_record_at[hub][p]=tick;
        if(strstr(record,"event=connect reason=quarantined")) hub_quarantine_records++;
        if(strstr(record,"event=connect")) hub_connects++;
        if(strstr(record,"event=disconnect")) hub_disconnects++;
        if(strstr(record,"event=reset_complete")) hub_resets++;
        if(strstr(record,"event=enumerate")) hub_enumerations++;
        if(strstr(record,"event=failed") || strstr(record,"result=-")) hub_failures++;
    }
}
#ifndef USB_LATENCY_QUEUE
void input_usb_event(uint16_t type,uint16_t code,int32_t value,gen_t gen,uint64_t tick)
{
    (void)tick; assert(gen); events++;
    if(type==INPUT_KEY) {
        assert(code<=512);
        if(value==1) {held[code]++;key_downs++;}
        else if(value==0) {assert(held[code]);held[code]--;key_ups++;}
        else {assert(value==2 && held[code]); repeats++;}
    } else if(type==INPUT_BTN) {assert(code<3); if(value) buttons[code]++; else {assert(buttons[code]);buttons[code]--;}}
    else {assert(type==INPUT_REL);if(code==INPUT_X) last_x=value;else if(code==INPUT_Y) last_y=value;else {assert(code==INPUT_WHEEL);last_wheel=value;wheel_events++;}}
}
#endif
static const uint8_t wheel_desc[]={
    0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,
    0x05,9,0x19,1,0x29,5,0x15,0,0x25,1,0x95,5,0x75,1,0x81,2,
    0x95,1,0x75,3,0x81,1,0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7f,0x75,8,0x95,3,0x81,6,0xc0,0xc0};
enum kind {KBD=1,MOUSE,HUB};
struct peripheral {enum kind kind;unsigned addr;uint8_t report[8];unsigned report_len,leds,protocol,in_toggle,idle_requests,boot_requests;bool present;};
struct model {
    struct usb_host host;
    struct peripheral dev[2+USB_PORT_MAX];
    _Alignas(4096) uint8_t dma[USB_DMA_BYTES];
    unsigned regs[128],config[64],selected,setup[8],data_at,pending_addr;
    unsigned hub_change[USB_PORT_MAX],hub_status[USB_PORT_MAX];
    uint64_t now;
    uint32_t done_head;
    unsigned sleep_gap; /* scheduler overshoot, independent of the HC frame */
    bool check_sleep_lock;
    struct usb_host *sleep_host;
    unsigned frame_ms; /* Independent HC clock; zero keeps the 1 ms model. */
    uint64_t reset_at[2], recovery_at[2];
    unsigned root_resets[2], root_enables[2], root_acks[2], hub_acks[USB_PORT_MAX];
    bool ohci,ehci,host_error,freeze,reset_stuck,ownership_stuck,nak,silent,stall,bad_length,bad_dma,malformed;
    bool bulk_test;
    bool late_hid;
    unsigned latched_td,latched_ed,late_hid_acks;
    bool bulk_short;
    unsigned bulk_short_bytes;
    uint64_t bulk_ready_at;
    bool hub_stuck,hub_status_error;
    unsigned hub_power_requests,hub_reset_requests;
    unsigned bulk_in_toggle, bulk_out_toggle, bulk_packets;
    unsigned packets, setups, dma_checks, protocol_requests, led_requests;
};
static struct model m;
static void (*frame_hook)(struct model *);
static void (*report_ack_hook)(struct model *,struct peripheral *);
static uint8_t *dma_at(struct model *m,unsigned ptr,unsigned len)
{
    unsigned phys=m->host.dma_phys;
    assert(ptr>=phys && ptr-phys<USB_DMA_BYTES && len<=USB_DMA_BYTES-(ptr-phys));
    m->dma_checks++;
    return m->dma+(ptr-phys);
}
static struct peripheral *target(struct model *m,unsigned address)
{
    for(unsigned i=0;i<ARRAY_SIZE(m->dev);i++) {
        unsigned root=i<2 ? i:m->dev[0].kind==HUB ? 0:1;
        unsigned status=m->ohci ? m->regs[(0x54+4*root)/4] :
            (m->regs[0x10/4]>>(16*root))&0xffff;
        if(!(status&(m->ohci ? 2u:4u))) continue;
        if(i>=2 && !(m->hub_status[i-2]&2)) continue;
        if(m->dev[i].present && (address ? m->dev[i].addr==address : i==m->selected)) return &m->dev[i];
    }
    return NULL;
}
static unsigned descriptor(struct model *m,struct peripheral *d,uint8_t *b)
{
    unsigned type=m->setup[3];
    if(type==1) {
        uint8_t dd[18]={18,1,0x10,1,0,0,0,8,0x34,0x12,0,0,0,1,0,0,0,1};
        dd[4]=d->kind==HUB ? 9:0; dd[10]=(uint8_t)d->kind;
        if(d->kind==HUB) {dd[8]=9;dd[9]=4;dd[10]=0xaa;dd[11]=0x55;}
        memcpy(b,dd,18);return 18;
    }
    if(type==2) {
        uint8_t cfg[]={9,2,34,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,0,0,7,5,0x81,3,8,0,10};
        if(d->kind==HUB) {cfg[14]=9;cfg[15]=0;cfg[16]=0;cfg[2]=25;memcpy(b,cfg,18);memcpy(b+18,cfg+27,7);return 25;}
        cfg[16]=d->kind==KBD ? 1:2;
        cfg[31]=d->kind==KBD ? 8:4;
        cfg[25]=d->kind==MOUSE ? sizeof(wheel_desc):0;
        if(m->malformed) cfg[9]=0;
        memcpy(b,cfg,sizeof(cfg));return sizeof(cfg);
    }
    if(type==0x29) {uint8_t hub[]={10,0x29,8,0x0a,0,1,0,0,0,0xff};memcpy(b,hub,sizeof(hub));return sizeof(hub);}
    if(type==0x22) {memcpy(b,wheel_desc,sizeof(wheel_desc));return sizeof(wheel_desc);}
    assert(!"unexpected descriptor");return 0;
}
static int transaction(struct model *m,unsigned address,unsigned ep,unsigned pid,unsigned toggle,uint8_t *buf,unsigned len)
{
    struct peripheral *d=target(m,address);if(!d) return -USB_EIO;
    m->packets++;assert(toggle<2 && len<=64);
    if(m->silent)return -USB_EAGAIN;
    if(m->stall) return -USB_EPIPE;
    if(ep && m->bulk_test && (!m->host.devices[0].hid[0].active || ep!=1)) {
        assert(ep==(pid==USB_PID_IN ? (m->host.devices[0].hid[0].active ? 2u:1u):(m->host.devices[0].hid[0].active ? 3u:2u)));
        if(m->now<m->bulk_ready_at) return -USB_EAGAIN;
        unsigned *expected=pid==USB_PID_IN ? &m->bulk_in_toggle:&m->bulk_out_toggle;
        assert(toggle==*expected); *expected^=1; m->bulk_packets++;
        if(pid==USB_PID_IN && m->bulk_short) {
            assert(m->bulk_short_bytes<=len);len=m->bulk_short_bytes;m->bulk_short=false;
        }
        if(pid==USB_PID_IN) memset(buf,0x5a,len);
        else for(unsigned i=0;i<len;i++) assert(buf[i]==0xa7);
        return (int)len;
    }
    if(ep) {
        assert(ep==1 && pid==USB_PID_IN);
        if(m->nak || !d->report_len) return -USB_EAGAIN;
        assert(toggle==d->in_toggle);d->in_toggle^=1;
        if(report_ack_hook)report_ack_hook(m,d);
        unsigned n=d->report_len;assert(n<=len);memcpy(buf,d->report,n);d->report_len=0;return (int)n;
    }
    if(pid==USB_PID_SETUP) {
        assert(len==8 && !toggle);m->setups++;
        for(unsigned i=0;i<8;i++)m->setup[i]=buf[i];m->data_at=0;return 8;
    }
    assert(toggle==1 || m->data_at);
    unsigned req=m->setup[1],type=m->setup[0],value=m->setup[2]|m->setup[3]<<8,index=m->setup[4];
    if(!len) {
        if(req==5 && !type) d->addr=value;
        if(req==11 && type==0x21){d->protocol=value;m->protocol_requests++;if(!value)d->boot_requests++;}
        if(req==10 && type==0x21){assert(!value && !index);d->idle_requests++;}
        if(req==9 && !type)d->in_toggle=0; /* SET_CONFIGURATION */
        if(type==0x23) {
            assert(d->kind==HUB && index>=1 && index<=USB_PORT_MAX);
            if(req==3 && value==8) m->hub_power_requests++; /* no switching */
            else if(req==3 && value==4) {
                m->hub_reset_requests++;m->selected=index+1;
                m->hub_status[index-1]|=0x10;
                if(!m->hub_stuck) {
                    m->hub_status[index-1]&=~0x10u;m->hub_change[index-1]|=0x10;
                    if(m->dev[m->selected].present) {
                        m->dev[m->selected].addr=0;m->dev[m->selected].in_toggle=0;
                        if(!(m->hub_status[index-1]&2)) m->hub_change[index-1]|=2;
                        m->hub_status[index-1]|=2;
                    }
                }
            } else if(req==1 && value>=16 && value<=20) {
                m->hub_acks[index-1]++;m->hub_change[index-1]&=~(1u<<(value-16));
            }
            else {assert(req==1 && value==1);m->hub_status[index-1]&=~2u;}
        }
        return 0;
    }
    if(pid==USB_PID_IN) {
        uint8_t data[USB_CONFIG_MAX];unsigned n=0;
        if(req==6)n=descriptor(m,d,data);
        else if(req==0 && type==0xa3){assert(d->kind==HUB && index>=1 && index<=USB_PORT_MAX);if(m->hub_status_error)return -USB_EPIPE;unsigned s=m->hub_status[index-1],c=m->hub_change[index-1];data[0]=s;data[1]=s>>8;data[2]=c;data[3]=c>>8;n=4;}
        else assert(!"unexpected control IN");
        unsigned requested=m->setup[6]|m->setup[7]<<8;if(n>requested)n=requested;
        unsigned left=n>m->data_at ? n-m->data_at:0;if(left>len)left=len;
        memcpy(buf,data+m->data_at,left);m->data_at+=left;return (int)left;
    }
    assert(req==9 && type==0x21 && len==1);d->leds=buf[0];m->led_requests++;return 1;
}
static uint32_t read_reg(void *arg,unsigned off,unsigned width)
{
    struct model *m=arg;(void)width;
    unsigned frame=(unsigned)(m->now/(m->frame_ms ? m->frame_ms:1));
    if(m->ohci && off==0x3c)return m->freeze ? 0:frame&0xffff;
    if(!m->ohci && off==6)return m->freeze ? 0:frame&0x7ff;
    if(!m->ohci && off==2)return ((m->regs[0]&1)?0:0x20)|(m->host_error?8:0);
    if(m->ohci && off==0x0c && m->host_error)return 0x10;
    return m->regs[off/4] >> (8*(off&3));
}
static void write_reg(void *arg,unsigned off,uint32_t value,unsigned width)
{
    struct model *m=arg;writes++;
    if(!m->ohci && off==0 && (value&2) && !m->reset_stuck) {
        /* QEMU uhci_reset(): stopped, empty schedule, attached devices
         * reappear with CCS/CSC, but PE stays clear until software sets it. */
        m->regs[0]=m->regs[1]=m->regs[2]=0;m->regs[0x0c/4]=64;
        m->regs[0x10/4]=0;
        for(unsigned p=0;p<2;p++) {
            unsigned status=0x80|(m->dev[p].present ? 3u:0u)|(p==1 && m->dev[p].present && m->dev[p].kind!=HUB ? 0x100u:0u);
            m->regs[0x10/4]|=status<<(16*p);m->dev[p].addr=0;
        }
        return;
    }
    if(m->ohci) {
        if(off==0x0c){m->regs[off/4]&=~value;return;}
        if(off>=0x54 && off<0x5c) {
            unsigned p=(off-0x54)/4;
            if(value==0x10){m->selected=p;m->dev[p].addr=0;m->dev[p].in_toggle=0;m->regs[off/4]|=0x10002;return;}
            if(value==1){m->regs[off/4]&=~2u;return;}
            if(value & 0x1f0000){m->root_acks[p]++;m->regs[off/4]&=~(value & 0x1f0000);return;}
            return;
        }
    } else if(off==0x10 || off==0x12) {
        unsigned p=(off-0x10)/2;
        unsigned old=(m->regs[off/4]>>(8*(off&3)))&0xffff;
        if((value&0x200) && !(old&0x200)) {
            m->selected=p;m->dev[p].addr=0;m->dev[p].in_toggle=0;m->reset_at[p]=m->now;m->root_resets[p]++;
        }
        if(!(value&0x200) && (old&0x200)) {
            assert(m->now-m->reset_at[p]>=50);m->recovery_at[p]=m->now;
        }
        if((value&4) && !(old&4) && (old&1)) {
            assert(!(value&0x200) && m->now-m->recovery_at[p]>=10);m->root_enables[p]++;
        }
        /* QEMU preserves RO bits, accepts PE only for CCS, and clears
         * CSC/PEC only on an explicit write of one (uhci_port_write). */
        if(!(old&1)) value&=~4u;
        unsigned requested=value;
        if(requested&0x0a)m->root_acks[p]++;
        value=(old&0x1bb)|(value&0x1244);
        value&=~(requested&0x0au);
    }
    unsigned shift=(off&3)*8,mask=width==1 ? 255:width==2 ? 65535:UINT32_MAX;
    m->regs[off/4]=(m->regs[off/4]&~(mask<<shift))|(value&mask)<<shift;
}
static uint32_t config_read(void *arg,unsigned off,unsigned width)
{struct model *m=arg;(void)width;return m->config[off/4]>>(8*(off&3));}
static void config_write(void *arg,unsigned off,uint32_t value,unsigned width)
{
    struct model *m=arg;writes++;
    assert(off!=4 || width==2); /* PCI W1C status is never echoed. */
    if(off==0xc0){m->config[off/4]&=0x5040;return;}
    m->config[off/4]=value;
    if(off==4 && (value&4)) {
        assert(m->host.dma && leases[m->host.claims[2]].state==RS_ACTIVE);
        if(m->ohci) assert(m->regs[0x18/4]==m->host.dma_phys);
        else assert(m->regs[8/4]==m->host.dma_phys);
        volatile uint32_t *ed=(volatile uint32_t *)(m->dma+USB_ED);
        assert(m->ohci ? (ed[0]&(1u<<14))!=0 : ed[1]==1);
    }
}
static uint64_t now(void *arg){return ((struct model *)arg)->now;}
static void execute_ohci(struct model *m,unsigned ed_addr,bool periodic)
{
    for(unsigned i=0;ed_addr && i<=USB_DEVICE_MAX*USB_HID_MAX;i++) {
        volatile uint32_t *ed=(volatile uint32_t *)dma_at(m,ed_addr,16);
        unsigned next_ed=ed[3];
        if(!(ed[0]&(1u<<14)) && (ed[2]&~15u)!=ed[1] && !(ed[2]&1)) {
            unsigned td_addr=ed[2]&~15u;
            volatile uint32_t *td=(volatile uint32_t *)dma_at(m,td_addr,16);
            unsigned len=td[1] ? td[3]-td[1]+1:0;
            uint8_t *buf=len ? dma_at(m,td[1],len):NULL;
            assert(len<=64 && (td[2]&15)==0);
            if(m->bulk_test && !periodic) {
                assert((m->regs[1]&0x20) && !(m->regs[1]&0x10) && (m->regs[2]&4));
                assert(((ed[0]>>16)&0x7ff)==64);
            }
            unsigned dir=(td[0]>>19)&3;
            if(m->late_hid && ((ed[0]>>7)&15)==1 && target(m,ed[0]&127)->kind==KBD) {
                m->latched_td=td_addr;m->latched_ed=ed_addr;ed_addr=next_ed;continue;
            }
            int n=transaction(m,ed[0]&127,(ed[0]>>7)&15,dir==0?USB_PID_SETUP:dir==1?USB_PID_OUT:USB_PID_IN,((td[0]>>24)&3)&1,buf,len);
            if(n!=-USB_EAGAIN) {
                unsigned cc=n>=0?0:n==-USB_EPIPE?4:5;
                if(n>=0)td[1]=(unsigned)n==len?0:td[1]+(unsigned)n;
                if(m->bad_dma)td[1]=m->host.dma_phys-1;
                td[0]=(td[0]&0x0fffffff)|(cc<<28);ed[2]=td[2]|(cc ? 1:0);
                td[2]=m->done_head;m->done_head=td_addr; /* HC owns NextTD */
            }
        }
        ed_addr=next_ed;
    }
    assert(!ed_addr);
}
static void execute_uhci(struct model *m,unsigned qh_addr)
{
    for(unsigned i=0;i<=USB_DEVICE_MAX*USB_HID_MAX;i++) {
        volatile uint32_t *qh=(volatile uint32_t *)dma_at(m,qh_addr,8);
        if(!(qh[1]&1)) {
            volatile uint32_t *td=(volatile uint32_t *)dma_at(m,qh[1]&~15u,16);
            if(td[1]&(1u<<23)) {
                unsigned len=(((td[2]>>21)&0x7ff)+1)&0x7ff;
                uint8_t *buf=len ? dma_at(m,td[3],len):NULL;
                assert(len<=64);
                if(m->late_hid && ((td[2]>>15)&15)==1 && target(m,(td[2]>>8)&127)->kind==KBD) {
                    m->latched_td=qh[1]&~15u;m->latched_ed=qh_addr;
                    qh_addr=qh[0]&~15u;continue;
                }
                int n=transaction(m,(td[2]>>8)&127,(td[2]>>15)&15,td[2]&255,(td[2]>>19)&1,buf,len);
                if(n==-USB_EAGAIN)td[1]|=1u<<19;
                else {
                    td[1]&=~((1u<<23)|0x7ffu);
                    if(n<0)td[1]|=n==-USB_EPIPE ? 1u<<22:1u<<18;
                    else {if(m->bad_length)n=(int)len+1;td[1]|=((unsigned)n-1)&0x7ff;}
                    qh[1]=1;
                }
            }
        }
        if(qh[0]&1)return;
        assert(qh[0]&2);qh_addr=qh[0]&~15u;
    }
    assert(!"cyclic UHCI QH chain");
}
static void complete_late_hid(struct model *m)
{
    if(!m->latched_td)return;
    volatile uint32_t *ed=(volatile uint32_t *)dma_at(m,m->latched_ed,16);
    if(m->ohci ? !(ed[0]&(1u<<14)):!(ed[1]&1))return;
    volatile uint32_t *td=(volatile uint32_t *)dma_at(m,m->latched_td,16);
    unsigned address,ep,toggle,len,buffer;
    if(m->ohci) {
        address=ed[0]&127;ep=(ed[0]>>7)&15;toggle=(td[0]>>24)&1;
        buffer=td[1];len=td[3]-td[1]+1;
    } else {
        address=(td[2]>>8)&127;ep=(td[2]>>15)&15;toggle=(td[2]>>19)&1;
        buffer=td[3];len=(((td[2]>>21)&0x7ff)+1)&0x7ff;
    }
    int n=transaction(m,address,ep,USB_PID_IN,toggle,dma_at(m,buffer,len),len);
    assert(n>=0);
    if(m->ohci) {
        ed[2]=ed[1];td[0]&=0x0fffffffu;td[1]=(unsigned)n==len ? 0:buffer+(unsigned)n;
        td[2]=m->done_head;m->done_head=m->latched_td;
    } else td[1]=(td[1]&~((1u<<23)|0x7ffu))|(((unsigned)n-1)&0x7ff);
    m->latched_td=0;m->late_hid=false;m->late_hid_acks++;
}
static void sleep_ms(void *arg,unsigned ms)
{
    struct model *m=arg;
    if(m->check_sleep_lock) {
        struct usb_host *h=m->sleep_host ? m->sleep_host:&m->host;
        assert(!pthread_mutex_trylock(&h->transfer_lock));
        fs_lock_drop(&h->transfer_lock);
    }
    if(m->sleep_gap)ms+=m->sleep_gap;
    while(ms--) {
        uint64_t before=m->now++;
        if(m->ehci) {if(!m->ownership_stuck)m->config[0x40/4]&=~(1u<<16);continue;}
        if(m->frame_ms && before/m->frame_ms==m->now/m->frame_ms)continue;
        if(frame_hook)frame_hook(m);
        if(m->ohci) {
            if(!m->ownership_stuck && (m->regs[2]&8)) {m->regs[1]&=~0x100u;m->regs[2]&=~8u;}
            if(!m->reset_stuck)m->regs[2]&=~1u;
            if(!(m->config[1]&4))continue;
            if(m->regs[1]&4)execute_ohci(m,*(uint32_t *)(m->dma+((m->now/(m->frame_ms ? m->frame_ms:1))&31)*4),true);
            if(m->regs[1]&0x10)execute_ohci(m,m->regs[0x20/4],false);
            if(m->regs[1]&0x20)execute_ohci(m,m->regs[0x28/4],false);
            complete_late_hid(m);
            if(m->done_head && !(m->regs[0x0c/4]&2)) {
                *(uint32_t *)(m->dma+0x84)=m->done_head;m->done_head=0;m->regs[0x0c/4]|=2;
            }
        } else {
            if(!m->reset_stuck && (m->regs[0]&2))m->regs[0]&=~2u;
            if(!(m->config[1]&4) || !(m->regs[0]&1))continue;
            unsigned frame=((unsigned)(m->now/(m->frame_ms ? m->frame_ms:1))&1023)*4;
            execute_uhci(m,*(uint32_t *)dma_at(m,m->regs[2]+frame,4)&~15u);
            complete_late_hid(m);
        }
    }
}
static struct usb_io io(struct model *m)
{return (struct usb_io){m,read_reg,write_reg,config_read,config_write,now,sleep_ms};}
static void setup(bool ohci,bool hub)
{
    memset(&m,0,sizeof(m));memset(&g_boot,0,sizeof(g_boot));lease_count=0;
    connect_records=disconnect_records=reset_records=enable_records=0;
    descriptor_starts=descriptor_results=descriptor_failures=0;
    first_scans=cadence_records=retired_packets=service_entries=service_queues=0;
    hub_connects=hub_disconnects=hub_resets=hub_enumerations=hub_failures=0;
    usb_devices=usb_attaches=usb_detaches=0;
    memset(root_record_at,0,sizeof(root_record_at));memset(hub_record_at,0,sizeof(hub_record_at));
    memset(root_records,0,sizeof(root_records));memset(hub_records,0,sizeof(hub_records));
    root_quarantine_records=hub_quarantine_records=0;
    memset(hid_reports,0,sizeof(hid_reports));memset(hid_polls,0,sizeof(hid_polls));memset(hid_pipes,0,sizeof(hid_pipes));
    usb_key_records=f12_down_records=f12_up_records=0;
    memset(held,0,sizeof(held));memset(buttons,0,sizeof(buttons));
    m.ohci=ohci;m.pending_addr=0;
    m.dev[0]=(struct peripheral){.kind=hub?HUB:KBD,.present=true};
    m.dev[1]=(struct peripheral){.kind=MOUSE,.present=!hub};
    m.dev[2]=(struct peripheral){.kind=KBD,.present=hub};
    m.dev[3]=(struct peripheral){.kind=MOUSE,.present=hub};
    for(unsigned p=0;p<USB_PORT_MAX;p++) m.hub_status[p]=0x100;
    if(hub) {m.hub_status[0]|=1;m.hub_status[1]|=1;m.hub_change[0]=m.hub_change[1]=1;}
    if(ohci){m.regs[0]=0x110;m.regs[1]=0x100;m.regs[0x48/4]=2|(1u<<9);m.regs[0x54/4]=1;m.regs[0x58/4]=hub?0:0x201;}
    else m.regs[0x10/4]=0x80|1|((hub?0:0x181)<<16);
    m.config[0xc0/4]=0x2f00;
}
static int attach(void)
{
    struct pci_func p={.vendor=0x8086,.device=0x7020,.class_code=0x0c,.subclass=3,.prog_if=m.ohci?0x10:0,.irq_line=11,.irq_pin=1};
    p.bar[m.ohci?0:4]=m.ohci?0xf0000000:0xc001;
    struct usb_io callbacks=io(&m);
    return usb_attach(&m.host,&p,&callbacks,m.dma,0x100000);
}
static void model_step(void)
{
    usb_step(&m.host);
    for(unsigned i=0;i<20 && m.host.ready;i++) {
        sleep_ms(&m,1);
        fs_lock_take(&m.host.transfer_lock);usb_hid_service(&m.host);fs_lock_drop(&m.host.transfer_lock);
    }
    usb_step(&m.host); /* apply deferred quarantine and LED output */
}
static void basic(bool ohci,bool hub)
{
    setup(ohci,hub);assert(!attach());model_step();
    assert(m.host.ready && !m.host.quarantined && m.host.device_count==(hub?3u:2u));
    assert(connect_records==(hub?1u:2u) && first_scans==1);
    if(!ohci) assert(m.root_resets[0]==1 && m.root_enables[0]==1);
    assert(descriptor_starts==m.host.device_count && descriptor_results==descriptor_starts && !descriptor_failures);
    if(!ohci) assert(retired_packets);
    unsigned ki=hub?1:0,mi=hub?2:1;
    struct usb_device *kbd=&m.host.devices[ki],*mouse=&m.host.devices[mi];
    assert(kbd->hid[0].active && mouse->hid[0].active && mouse->hid[0].wheel);
    assert(usb_input_ready() && usb_devices==2 && usb_attaches==2);
    if(hub) assert(m.host.devices[0].hub_ports==8 && m.host.devices[0].vendor==0x0409 &&
        m.host.devices[0].product==0x55aa && m.hub_power_requests==8 &&
        hub_connects==2 && hub_disconnects==6 && m.hub_reset_requests==2 && !hub_failures);
    assert(kbd->address!=mouse->address && kbd->vendor==0x1234 && mouse->low_speed==!hub);
    unsigned kp=hub?2:0,mp=hub?3:1;
    m.dev[kp].report[0]=2;m.dev[kp].report[2]=4;m.dev[kp].report_len=8;
    m.dev[mp].report[0]=1;m.dev[mp].report[1]=0xfd;m.dev[mp].report[2]=7;m.dev[mp].report[3]=0xff;m.dev[mp].report_len=4;
    m.now+=20;model_step();
    assert(held[INPUT_KEY_A]==1 && held[0x2a]==1 && buttons[0]==1);
    assert(last_x==-3 && last_y==7 && last_wheel==-1 && wheel_events);
    unsigned old=repeats;m.now+=INPUT_REPEAT_DELAY_MS;model_step();assert(repeats>old);
    memset(m.dev[kp].report,0,8);m.dev[kp].report[2]=1;m.dev[kp].report_len=8;
    m.now+=20;model_step();assert(held[INPUT_KEY_A]==1); /* rollover holds */
    memset(m.dev[kp].report,0,8);m.dev[kp].report[2]=0x39;m.dev[kp].report_len=8;
    unsigned initial_led=m.dev[kp].leds;
    m.now+=20;model_step();assert(!held[INPUT_KEY_A] && !held[0x2a]);
    model_step();assert(m.dev[kp].leds==(initial_led^2) && m.led_requests>=2);
    usb_detach(kbd,"test");assert(!held[0x3a]);
    usb_detach(mouse,"test");assert(!buttons[0]);
    assert(!usb_input_ready() && !usb_devices && usb_detaches==2);
    assert(m.dma_checks>20 && m.protocol_requests>=3 && events && key_downs && key_ups);
    for(unsigned i=0;i<513;i++)assert(!held[i]);
    printf("PASS USB %s %s enumeration/HID/LED/repeat/rollover/detach/DMA\n",ohci?"OHCI":"UHCI",hub?"hub":"root");
}
static void errors(bool ohci)
{
    setup(ohci,false);assert(!attach());model_step();
    struct usb_device *d=&m.host.devices[0];
    m.nak=true;model_step();assert(d->hid[0].trace.result==-USB_EAGAIN && m.host.ready);
    m.stall=true;model_step();assert(d->quarantined && !d->hid[0].active);
    setup(ohci,false);m.reset_stuck=true;uint64_t begin=m.now;assert(attach()<0 && m.host.quarantined && m.now-begin<=600);
    for(unsigned i=0;i<lease_count;i++)assert(leases[i].state==RS_QUARANTINED);
    setup(ohci,false);assert(!attach());m.silent=true;begin=m.now;
    model_step();assert(m.host.devices[0].quarantined && m.host.root_blocked[0] && m.now-begin<1500);
    assert(descriptor_failures==2 && descriptor_results==descriptor_starts);
    setup(ohci,false);assert(!attach());m.malformed=true;model_step();assert(m.host.devices[0].quarantined);
    setup(ohci,false);assert(!attach());model_step();m.freeze=true;
    model_step();model_step();assert(m.host.quarantined);
    setup(ohci,false);assert(!attach());model_step();
    m.dev[0].report[2]=4;m.dev[0].report_len=8;
    if(ohci)m.bad_dma=true;else m.bad_length=true;
    model_step();assert(m.host.quarantined);
    setup(ohci,false);assert(!attach());model_step();m.host_error=true;
    model_step();assert(m.host.quarantined && !(m.config[1]&4));
    printf("PASS USB %s NAK/STALL/reset timeout/malformed config/frame stall/DMA corruption/host errors\n",ohci?"OHCI":"UHCI");
}
static void hotplug(bool ohci)
{
    setup(ohci,false);assert(!attach());model_step();
    struct usb_device *d=&m.host.devices[0];unsigned old_addr=d->address;
    uint8_t report[8]={0,0,4,4,0,0,0,0};
    assert(!usb_hid_report(d,&d->hid[0],report,8) && held[INPUT_KEY_A]==1);
    if(ohci)m.regs[0x54/4]=0x10000;else m.regs[0x10/4]=(m.regs[0x10/4]&0xffff0000)|0x82;
    m.now+=1100;model_step();assert(!d->present && !held[INPUT_KEY_A]);
    assert(disconnect_records==1);
    if(ohci)m.regs[0x54/4]=0x10001;else m.regs[0x10/4]|=3;
    m.now+=300;model_step();
    assert(m.host.device_count==3 && m.host.devices[2].address>old_addr && m.host.devices[2].hid[0].active);
    /* Interrupt/endpoint validation is bounded even on hostile descriptors. */
    uint8_t cfg[USB_CONFIG_MAX];m.setup[3]=2;unsigned len=descriptor(&m,&m.dev[0],cfg);
    struct usb_device parsed={.host=&m.host,.ep0=8};
    cfg[31]=65;assert(usb_parse_config(&parsed,cfg,len)==-USB_EPROTO);
    cfg[31]=8;cfg[33]=0;assert(usb_parse_config(&parsed,cfg,len)==-USB_EPROTO);
    cfg[33]=10;for(unsigned at=0;at<len;at++) {uint8_t saved=cfg[at];cfg[at]^=0x80;memset(parsed.hid,0,sizeof(parsed.hid));(void)usb_parse_config(&parsed,cfg,len);cfg[at]=saved;}
    setup(ohci,true);m.dev[2].kind=HUB;assert(!attach());model_step();
    assert(m.host.devices[1].quarantined && m.host.devices[0].hub_blocked[0]);
    setup(ohci,false);assert(!attach());m.host.device_count=USB_DEVICE_MAX;
    assert(usb_enumerate(&m.host,0,0,0,false)==-ENOSPC);
    printf("PASS USB %s hotplug/duplicate keys/descriptor bounds/nested hub/capacity\n",ohci?"OHCI":"UHCI");
}
static void qemu_hub(bool ohci)
{
    /* QEMU's auto-inserted topology: keyboard at root 1, 0409:55aa hub
     * at root 2. Use port 8 to exercise its two-byte removable bitmap. */
    setup(ohci,false);m.dev[1].kind=HUB;
    m.dev[9]=(struct peripheral){.kind=MOUSE,.present=true};
    m.hub_status[7]|=1;m.hub_change[7]=1;
    if(ohci) m.regs[0x58/4]=1;
    assert(!attach() && !usb_input_ready());model_step();
    struct usb_device *hub=&m.host.devices[1],*mouse=&m.host.devices[2];
    assert(m.host.ready && m.host.device_count==3 && hub->hub_ports==8 && hub->hub_child[7]==3);
    assert(mouse->parent==2 && mouse->root_port==1 && mouse->port==7 && mouse->hid[0].active);
    assert(hub_connects==1 && hub_disconnects==7 && !hub_failures);
    assert(m.hub_power_requests==8 && m.hub_reset_requests==1 && !m.hub_change[7]);
    assert(usb_devices==2 && usb_input_ready());
    m.dev[0].report[2]=4;m.dev[0].report_len=8;
    m.dev[9].report[0]=1;m.dev[9].report[1]=3;m.dev[9].report[2]=5;m.dev[9].report[3]=1;m.dev[9].report_len=4;
    m.now+=20;model_step();assert(held[INPUT_KEY_A] && buttons[0] && last_x==3 && last_y==5 && last_wheel==1);
    gen_t old=mouse->generation;
    m.dev[9].present=false;m.hub_status[7]=0x100;m.hub_change[7]=3;
    m.now+=300;model_step();
    assert(!mouse->present && !buttons[0] && held[INPUT_KEY_A] && usb_devices==1 && usb_input_ready());
    m.dev[9].present=true;m.hub_status[7]|=1;m.hub_change[7]=1;
    m.now+=300;model_step();
    assert(m.host.device_count==4 && hub->hub_child[7]==4 && m.host.devices[3].generation>old && usb_devices==2);
    unsigned resets=m.hub_reset_requests,enums=hub_enumerations;
    m.now+=300;model_step();assert(m.hub_reset_requests==resets && hub_enumerations==enums);
    if(ohci) m.regs[0x58/4]=0x10000;else m.regs[0x10/4]=(m.regs[0x10/4]&0xffffu)|(0x82u<<16);
    m.now+=300;model_step();assert(!hub->present && !m.host.devices[3].present && usb_devices==1);
    if(ohci) m.regs[0x54/4]=0x10000;else m.regs[0x10/4]=(m.regs[0x10/4]&0xffff0000u)|0x82;
    m.now+=300;model_step();assert(!usb_input_ready() && !held[INPUT_KEY_A] && usb_attaches==usb_detaches);
    printf("PASS USB %s QEMU eight-port hub at root 2: port 8 mouse, merged input, change clearing, hotplug and recursive detach\n",ohci?"OHCI":"UHCI");
    for(unsigned mode=0;mode<3;mode++) {
        setup(ohci,true);assert(!attach());
        if(mode==0) m.hub_stuck=true;
        else if(mode==1) m.hub_status_error=true;
        else m.hub_status[0]|=8;
        uint64_t begin=m.now;model_step();
        assert(m.host.devices[0].quarantined && m.host.root_blocked[0] && !usb_input_ready() && m.now-begin<2500);
        unsigned count=m.host.device_count;m.now+=300;model_step();assert(m.host.device_count==count);
    }
    printf("PASS USB %s hub reset timeout/status STALL/overcurrent: bounded quarantine, no retry\n",ohci?"OHCI":"UHCI");
}
static void delayed_frame_hid(bool ohci)
{
    /* Root-port keyboard plus QEMU-style hub/mouse. The HC gets its next
     * service opportunity after the old two-software-ms packet deadline. */
    setup(ohci,false);m.dev[1].kind=HUB;
    m.dev[2]=(struct peripheral){.kind=MOUSE,.present=true};
    m.hub_status[0]|=1;m.hub_change[0]=1;
    if(ohci)m.regs[0x58/4]=1;
    g_boot.flags=CBI_F_VERBOSE;
    assert(!attach());model_step();
    struct usb_device *kbd=&m.host.devices[0],*mouse=&m.host.devices[2];
    assert(kbd->hid[0].active && mouse->parent==2 && mouse->hid[0].active);
    assert(m.dev[0].boot_requests==1 && m.dev[0].idle_requests==1 && !m.dev[0].protocol);
    assert(m.dev[2].boot_requests==1 && m.dev[2].idle_requests==1 && m.dev[2].protocol==1);
    m.frame_ms=4;m.now=(m.now+3)&~UINT64_C(3);
    m.host.next_scan=m.now+1000;kbd->hid[0].next_poll=mouse->hid[0].next_poll=0;
    m.dev[0].report[2]=0x45;m.dev[0].report_len=8;
    m.dev[2].report[0]=1;m.dev[2].report[1]=3;m.dev[2].report[2]=5;m.dev[2].report_len=4;
    model_step();
    assert(buttons[0] && last_x==3 && last_y==5);
    assert(held[0x58]==1 && !m.dev[0].report_len && !m.dev[2].report_len);
    assert(hid_reports[kbd->address]==1 && hid_reports[mouse->address]==1 && f12_down_records==1);
    assert(kbd->hid[0].toggle==1 && mouse->hid[0].toggle==1 && m.host.ready);
    memset(m.dev[0].report,0,8);m.dev[0].report_len=8;
    memset(m.dev[2].report,0,8);m.dev[2].report_len=4;
    g_boot.flags=CBI_F_TEST_REQUEST; /* Test boots enable the same diagnostics. */
    m.now+=20;model_step();
    assert(!held[0x58] && !buttons[0] && !kbd->hid[0].toggle && !mouse->hid[0].toggle);
    assert(f12_up_records==1);
    /* An idle keyboard must not steal the mouse's packet or change its
     * toggle; repeat NAK diagnostics stop after a bounded prefix. */
    for(unsigned i=0;i<USB_HID_TRACE_LIMIT+2;i++) {
        m.dev[2].report[1]=2;m.dev[2].report_len=4;
        m.now+=20;model_step();
        assert(!kbd->hid[0].toggle && !m.dev[2].report_len && last_x==2);
    }
    assert(hid_reports[mouse->address]==USB_HID_TRACE_LIMIT &&
        hid_polls[kbd->address]==USB_HID_TRACE_LIMIT && hid_pipes[kbd->address]==USB_HID_TRACE_LIMIT);
    /* More reports and translations remain bounded too. */
    for(unsigned i=0;i<USB_HID_TRACE_LIMIT+2;i++) {
        m.dev[0].report[2]=i&1 ? 0:0x45;m.dev[0].report_len=8;
        m.now+=20;model_step();
    }
    assert(hid_reports[kbd->address]==USB_HID_TRACE_LIMIT && usb_key_records==USB_HID_TRACE_LIMIT);
    unsigned records=usb_key_records;
    unsigned reports=hid_reports[kbd->address],polls=hid_polls[mouse->address];
    kbd->hid[0].key_records=kbd->hid[0].report_records=mouse->hid[0].poll_records=0;
    g_boot.flags=0;m.dev[0].report[2]=0x45;m.dev[0].report_len=8;
    m.now+=20;model_step();
    assert(held[0x58]==1 && usb_key_records==records && hid_reports[kbd->address]==reports && hid_polls[mouse->address]==polls);
    /* Frame counters wrap without losing a pending release. */
    m.now=(uint64_t)(ohci ? 0xffffu:0x7ffu)*m.frame_ms;
    m.host.next_scan=m.now+1000;
    memset(m.dev[0].report,0,8);m.dev[0].report_len=8;
    kbd->hid[0].next_poll=0;model_step();assert(!held[0x58]);
    usb_detach(kbd,"test");usb_detach(mouse,"test");
    printf("PASS USB %s root F12 and hub mouse with independent frame clock\n",ohci?"OHCI":"UHCI");
}
static void late_hid(bool ohci)
{
    setup(ohci,false);assert(!attach());model_step();
    struct usb_device *d=&m.host.devices[0];
    m.late_hid=true;m.dev[0].report[2]=0x45;m.dev[0].report_len=8;
    model_step();assert(m.late_hid_acks==1 && held[0x58]==1 && d->hid[0].toggle==1);
    assert(!m.dev[0].report_len && m.host.ready);
    model_step();assert(held[0x58]==1 && d->hid[0].toggle==1 && m.late_hid_acks==1);
    usb_detach(d,"late_ack_end");assert(!held[0x58]);
    printf("PASS USB %s HID ACK during two-frame retirement: report delivered once, toggle advanced once\n",ohci ? "OHCI":"UHCI");
}
static void ehci(void)
{
    for(unsigned timeout=0;timeout<2;timeout++) {
        setup(false,false);m.ehci=true;m.ownership_stuck=timeout;
        memset(m.regs,0,sizeof(m.regs));m.regs[0]=0x20;
        m.regs[1]=0x1002;m.regs[2]=0x4000;m.regs[0x24/4]=1u<<12;
        m.regs[0x60/4]=1;m.config[0x40/4]=1|(1u<<16);
        struct pci_func p={.class_code=0xc,.subclass=3,.prog_if=0x20,.bar={0xf0000000},.irq_line=11,.irq_pin=1};
        struct usb_io callbacks=io(&m);int err=usb_attach(&m.host,&p,&callbacks,NULL,0);
        if(timeout) assert(err==-USB_ETIMEDOUT && m.host.quarantined && !m.host.owned && (m.config[0x40/4]&(1u<<16)));
        else assert(!err && m.host.owned && !m.host.ready && m.regs[0x60/4]==0 && !m.host.dma && !(m.config[1]&4));
    }
    puts("PASS USB EHCI companion routing/ownership timeout, no schedule/DMA");
}
static void policy(void)
{
    for(unsigned flags=0;flags<3;flags++) {
        setup(false,false);if(flags==0)g_boot.input_policy=CBI_INPUT_FIRMWARE;
        else g_boot.flags=flags==1?CBI_F_INPUT_FORCED:CBI_F_SAFE_MODE;
        unsigned before=writes,c=claims;assert(attach()==-ENOSYS && writes==before && claims==c);
    }
    setup(false,false);struct usb_io callbacks=io(&m);
    struct pci_func p={.class_code=0xc,.subclass=3,.bar={[4]=0xc001}};
    unsigned before=writes;assert(usb_attach(&m.host,&p,&callbacks,m.dma,0xfffff000)==-EINVAL && writes==before);
    setup(true,false);m.ownership_stuck=true;assert(attach()==-USB_ETIMEDOUT && m.host.quarantined);
    assert(m.regs[1]&0x100); /* never force firmware-owned IR */
    puts("PASS USB firmware-first/safe/invalid DMA/OHCI handoff guards");
}
static void bulk_engine(bool ohci)
{
    setup(ohci,false);assert(!attach());assert(!m.host.ops->port_reset(&m.host,0));m.bulk_test=true;
    struct usb_device *d=&m.host.devices[0];*d=(struct usb_device){.host=&m.host,.present=true,.configured=true,.ep0=64};
    uint8_t cfg[]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,7,5,0x81,2,64,0,0,7,5,2,2,64,0,0};
    assert(!usb_parse_config(d,cfg,sizeof(cfg)) && d->storage.bound && d->storage.in==1 && d->storage.out==2);
    uint8_t data[512];unsigned n;memset(data,0xa7,sizeof(data));
    usb_host_take(&m.host);
    assert(!usb_bulk(d,false,data,31,&n) && n==31);
    assert(!usb_bulk(d,true,data,13,&n) && n==13);
    assert(!usb_bulk(d,true,data,sizeof(data),&n) && n==sizeof(data));
    for(unsigned i=0;i<sizeof(data);i++) assert(data[i]==0x5a);
    memset(data,0xa7,sizeof(data));assert(!usb_bulk(d,false,data,sizeof(data),&n) && n==sizeof(data));
    usb_host_drop(&m.host);assert(m.bulk_packets==18 && m.dma_checks);
    usb_host_take(&m.host);
    unsigned toggle=d->storage.in_toggle;
    memset(data,0xcc,sizeof(data));m.bulk_short=true;m.bulk_short_bytes=36;
    assert(!usb_bulk(d,true,data,sizeof(data),&n) && n==36 && d->storage.in_toggle==(toggle^1));
    for(unsigned i=0;i<sizeof(data);i++) assert(data[i]==(i<36 ? 0x5a : 0xcc));
    assert(d->packet_trace.valid && d->packet_trace.expected==64 && d->packet_trace.actual==36 && !d->packet_trace.retired);
    m.bulk_short=true;m.bulk_short_bytes=0;
    assert(!usb_bulk(d,true,data,64,&n) && !n && d->storage.in_toggle==toggle);
    assert(!d->packet_trace.actual && d->packet_trace.frames>=2);
    m.bulk_ready_at=m.now+750;
    assert(!usb_bulk(d,true,data,64,&n) && n==64 && d->storage.in_toggle==(toggle^1));
    assert(d->packet_trace.frames>=750 && d->packet_trace.elapsed_ms>=750);
    m.stall=true;
    assert(usb_bulk(d,true,data,64,&n)==-USB_EPIPE && !n && d->storage.in_toggle==(toggle^1));
    assert(d->packet_trace.result==-USB_EPIPE && d->packet_trace.actual==-1);
    m.stall=false;m.bulk_ready_at=m.now+USB_BULK_TIMEOUT_MS+100;
    assert(usb_bulk(d,true,data,64,&n)==-USB_ETIMEDOUT && !n && d->storage.in_toggle==(toggle^1));
    assert(d->packet_trace.frames>=USB_BULK_TIMEOUT_MS && d->packet_trace.elapsed_ms>=USB_BULK_TIMEOUT_MS);
    assert(!d->storage.bulk_deadline);
    d->storage.bulk_deadline=m.now;
    unsigned packets=m.packets;
    assert(d->host->ops->packet(d,1,USB_PID_IN,d->storage.in_toggle,data,64,false)==-USB_ETIMEDOUT);
    assert(m.packets==packets && !d->packet_trace.valid);
    d->storage.bulk_deadline=0;
    usb_host_drop(&m.host);
    if(ohci) assert(!m.regs[0x28/4] && !m.regs[0x2c/4] && m.regs[1]==0x80);
    cfg[30]=2;assert(usb_parse_config(d,cfg,sizeof(cfg))==-USB_EPROTO);cfg[30]=0;
    cfg[28]=3;assert(usb_parse_config(d,cfg,sizeof(cfg))==-USB_EPROTO);cfg[28]=2;
    cfg[27]=0x81;assert(usb_parse_config(d,cfg,sizeof(cfg))==-USB_EPROTO);cfg[27]=2;
    d->low_speed=true;assert(usb_parse_config(d,cfg,sizeof(cfg))==-USB_EPROTO);
    printf("PASS USB %s bulk: CBW/CSW-sized packets, 512-byte IN/OUT, 36-byte short IN, ZLP, NAK budget, STALL/timeout toggles, descriptor MPS, retired DMA, invalid endpoints/speed\n",ohci?"OHCI":"UHCI");
}
static void port_flood(bool ohci)
{
    for(unsigned storage_fault=0;storage_fault<2;storage_fault++) {
        setup(ohci,false);assert(!attach());usb_step(&m.host);
        struct usb_device *d=&m.host.devices[0];
        unsigned count=m.host.device_count,resets=m.root_resets[0];
        /* Acknowledge changes without accidentally changing port controls. */
        if(ohci)m.regs[0x54/4]|=0x1f0000;
        else m.regs[0x10/4]|=0x0a;
        unsigned acks=m.root_acks[0];m.host.ops->port_ack_changes(&m.host,0);
        assert(m.root_acks[0]==acks+1 && (m.host.ops->port_status(&m.host,0)&USB_PORT_ENABLED));
        if(storage_fault) {
            for(unsigned i=0;i<USB_HID_MAX;i++)usb_hid_release(d,&d->hid[i]);
            d->configured=false;d->storage.bound=true;d->storage.block.quarantined=true;
        } else usb_device_quarantine(d,"fixture_transfer");
        /* Exact T23 sticky CCS+CSC/line-status shape, plus PEC. */
        if(ohci)m.regs[0x54/4]=0x1f0001;
        else m.regs[0x10/4]=(m.regs[0x10/4]&0xffff0000)|0x9b;
        acks=m.root_acks[0];
        for(unsigned scan=0;scan<40;scan++) {m.now+=250;usb_step(&m.host);}
        assert(m.host.root_blocked[0] && m.root_acks[0]==acks+1 && root_quarantine_records==1);
        assert(m.host.device_count==count && m.root_resets[0]==resets);
        unsigned logged=root_records[0];
        /* A later disable (e.g. another interface's fault) must not queue an
         * enable record after the one quarantined-connect announcement. */
        m.host.ops->port_disable(&m.host,0);
        for(unsigned scan=0;scan<8;scan++) {m.now+=250;usb_step(&m.host);}
        assert(root_records[0]==logged && m.root_acks[0]==acks+1);
        if(ohci)m.regs[0x54/4]=0x10000;
        else m.regs[0x10/4]=(m.regs[0x10/4]&0xffff0000)|0x82;
        m.now+=250;usb_step(&m.host);
        assert(!m.host.root_child[0] && m.host.root_blocked[0] && root_records[0]==logged+1);
        assert(disconnect_records==1 && m.root_acks[0]==acks+2);
        for(unsigned scan=0;scan<8;scan++) {m.now+=250;usb_step(&m.host);}
        assert(root_records[0]==logged+1 && m.host.device_count==count && m.root_resets[0]==resets);
    }
    setup(ohci,true);assert(!attach());usb_step(&m.host);
    struct usb_device *hub=&m.host.devices[0],*child=&m.host.devices[1];
    usb_device_quarantine(child,"fixture_transfer");m.hub_change[0]=0x1f;
    unsigned acks=m.hub_acks[0],count=m.host.device_count,resets=m.hub_reset_requests;
    for(unsigned scan=0;scan<40;scan++) {m.now+=250;usb_step(&m.host);}
    assert(hub_quarantine_records==1 && m.hub_acks[0]==acks+5 && !m.hub_change[0]);
    assert(hub->hub_blocked[0] && m.host.device_count==count && m.hub_reset_requests==resets);
    unsigned logged=hub_records[hub->address][0];
    for(unsigned scan=0;scan<8;scan++) {m.now+=250;usb_step(&m.host);}
    assert(hub_records[hub->address][0]==logged && m.hub_acks[0]==acks+5);
    m.dev[2].present=false;m.hub_status[0]=0x100;m.hub_change[0]=3;
    for(unsigned scan=0;scan<6;scan++) {m.now+=250;usb_step(&m.host);}
    assert(!hub->hub_child[0] && hub->hub_blocked[0] && hub_records[hub->address][0]==logged+1);
    assert(m.hub_acks[0]==acks+7 && !m.hub_change[0] && m.hub_reset_requests==resets);
    printf("PASS USB %s F1-53 port flood: one quarantined connect, W1C once, silence until disconnect, retained block, no enumeration/reset; every root/hub record >=1 s apart\n",ohci?"OHCI":"UHCI");
}
int main(void)
{
    bulk_engine(false);bulk_engine(true);
    assert(usb_mouse_wheel_layout(wheel_desc,sizeof(wheel_desc)));
    for(unsigned i=0;i<sizeof(wheel_desc)-1;i++) {
        /* Truncation can omit final collection ends, but never inputs. */
        if(i<sizeof(wheel_desc)-4)assert(!usb_mouse_wheel_layout(wheel_desc,i));
    }
    uint8_t bad[sizeof(wheel_desc)];memcpy(bad,wheel_desc,sizeof(bad));bad[35]=0x32;
    assert(!usb_mouse_wheel_layout(bad,sizeof(bad)));
    policy();ehci();basic(false,false);basic(true,false);basic(false,true);basic(true,true);errors(false);errors(true);hotplug(false);hotplug(true);qemu_hub(false);qemu_hub(true);
    delayed_frame_hid(false);delayed_frame_hid(true);late_hid(false);late_hid(true);
    port_flood(false);port_flood(true);
    puts("PASS usb host suite (production UHCI/OHCI/core/hub/HID, ASan+UBSan)");return 0;
}
