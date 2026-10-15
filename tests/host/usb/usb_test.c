/* Synthetic register/DMA controllers execute production packet engines.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/input.h>
#include <ciuki/storage.h>
#include "uhci_model.h"
struct storage *storage_get(void) { return NULL; }
int storage_add_removable(struct storage *s,struct blkdev *b) { (void)s;(void)b;assert(0);return -FS_EIO; }
int storage_add_removable_bounded(struct storage *s,struct blkdev *b,
                                 bool (*cancel)(void *),void (*pause)(void *),void *ctx)
{ (void)cancel;(void)pause;(void)ctx;return storage_add_removable(s,b); }
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
static unsigned schedule_entries,schedule_qhs,schedule_tds,schedule_invalid;
static unsigned port_actions,recovery_records;
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
    if(strstr(record,"[usb] port_action")) {
        assert(strstr(record,"before=") && strstr(record,"write=") && strstr(record,"status=") && strstr(record,"count="));
        port_actions++;
    }
    if(strstr(record,"[usb] hid recovery")) recovery_records++;
    if(strstr(record,"[usb] schedule frame=")) {
        assert(strstr(record,"controller="));
        if(strstr(record,"entry=")) {assert(strstr(record,"flbase=") && strstr(record,"current="));schedule_entries++;}
        else if(strstr(record,"qh=")) {assert(strstr(record,"link=") && strstr(record,"element="));schedule_qhs++;}
        else if(strstr(record,"td=")) {assert(strstr(record,"link=") && strstr(record,"status=") && strstr(record,"token=") && strstr(record,"buffer="));schedule_tds++;}
        else {assert(strstr(record,"invalid_link="));schedule_invalid++;}
    }
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
            assert(poll_frames>=2);
            if(retired==-USB_EAGAIN) assert(frames==poll_frames); /* still linked */
            else assert(!retired && frames>=poll_frames+2);
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
enum kind {KBD=1,MOUSE,HUB,RECEIVER};
struct peripheral {
    enum kind kind;unsigned addr;uint8_t report[8];
    unsigned report_len,leds,protocol,in_toggle,idle_requests,boot_requests;
    unsigned descriptor_failures,descriptor_attempts,reset_series,hid_errors,clear_halts;
    uint8_t mouse_report[8];unsigned mouse_length,mouse_toggle,error_endpoint,last_clear;
    uint64_t descriptor_at[6],address_at;
    bool present,second_reset,babble,clear_fail,crc;
};
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
    unsigned bulk_block_after;
    bool bulk_record;
    unsigned bulk_recorded;
    uint32_t bulk_tokens[8];
    unsigned packets, setups, dma_checks, protocol_requests, led_requests;
    bool disable_on_babble,debounce_bounce;
    unsigned debounce_changes;
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
        if(m->dev[i].present && (address ? m->dev[i].addr==address : i==m->selected && !m->dev[i].addr)) return &m->dev[i];
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
        if(d->kind==RECEIVER) {dd[8]=0x6d;dd[9]=4;dd[10]=0x2b;dd[11]=0xc5;}
        memcpy(b,dd,18);return 18;
    }
    if(type==2) {
        uint8_t cfg[]={9,2,34,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,0,0,7,5,0x81,3,8,0,10};
        if(d->kind==RECEIVER) {
            cfg[2]=59;cfg[4]=2;cfg[33]=8;memcpy(b,cfg,34);
            memcpy(b+34,cfg+9,25);b[36]=1;b[41]=2;b[54]=0x82;b[56]=8;b[58]=2;
            return 59;
        }
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
        if(m->now<m->bulk_ready_at || (m->bulk_block_after && m->bulk_packets>=m->bulk_block_after))
            return -USB_EAGAIN;
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
        assert((ep==1 || (ep==2 && d->kind==RECEIVER)) && pid==USB_PID_IN);
        if(d->hid_errors && (!d->error_endpoint || ep==d->error_endpoint)) {
            d->hid_errors--;
            if(d->babble && m->disable_on_babble) {
                unsigned p=(unsigned)(d-m->dev);assert(p<2);
                if(m->ohci)m->regs[(0x54+4*p)/4]=(m->regs[(0x54+4*p)/4]&~2u)|0x20000;
                else m->regs[0x10/4]=(m->regs[0x10/4]&~(4u<<(16*p)))|(8u<<(16*p));
            }
            return d->babble ? -75:d->crc ? -USB_ETIMEDOUT:-USB_EPIPE;
        }
        unsigned *report_len=ep==2 ? &d->mouse_length:&d->report_len;
        unsigned *in_toggle=ep==2 ? &d->mouse_toggle:&d->in_toggle;
        uint8_t *report=ep==2 ? d->mouse_report:d->report;
        if(m->nak || !*report_len) return -USB_EAGAIN;
        if(*report_len>len) return -75; /* legal suffix exceeds the HC's requested buffer */
        assert(toggle==*in_toggle);*in_toggle^=1;
        if(report_ack_hook)report_ack_hook(m,d);
        unsigned n=*report_len;assert(n<=len);memcpy(buf,report,n);*report_len=0;return (int)n;
    }
    if(pid==USB_PID_SETUP) {
        unsigned root=(unsigned)(d-m->dev);
        if(!address && root<2 && d->reset_series) {
            assert(m->now-(m->ohci ? m->reset_at[root]:m->recovery_at[root])>=10);
        }
        if(buf[1]==6 && buf[3]==1 && buf[6]==8 && !address) {
            assert(d->descriptor_attempts<6);
            d->descriptor_at[d->descriptor_attempts++]=m->now;
            if(d->descriptor_failures || (d->second_reset && d->reset_series<2)) {
                if(d->descriptor_failures)d->descriptor_failures--;
                return -USB_ETIMEDOUT;
            }
        }
        if(address && d->address_at) assert(m->now-d->address_at>=2);
        if(buf[0]==2 && buf[1]==1 && d->clear_fail)return -USB_EPIPE;
        assert(len==8 && !toggle);m->setups++;
        for(unsigned i=0;i<8;i++)m->setup[i]=buf[i];m->data_at=0;return 8;
    }
    assert(toggle==1 || m->data_at);
    unsigned req=m->setup[1],type=m->setup[0],value=m->setup[2]|m->setup[3]<<8,index=m->setup[4];
    if(!len) {
        if(req==5 && !type) { d->addr=value;d->address_at=m->now; }
        if(req==1 && type==2) {
            assert(!value && (index==0x81 || (index==0x82 && d->kind==RECEIVER)));
            d->clear_halts++;d->last_clear=index;
            if(index==0x82)d->mouse_toggle=0;else d->in_toggle=0;
        }
        if(req==11 && type==0x21){d->protocol=value;m->protocol_requests++;if(!value)d->boot_requests++;}
        if(req==10 && type==0x21){assert(!value && (!index || (index==1 && d->kind==RECEIVER)));d->idle_requests++;}
        if(req==9 && !type)d->in_toggle=d->mouse_toggle=0; /* SET_CONFIGURATION */
        if(type==0x23) {
            assert(d->kind==HUB && index>=1 && index<=USB_PORT_MAX);
            if(req==3 && value==8) m->hub_power_requests++; /* no switching */
            else if(req==3 && value==4) {
                m->hub_reset_requests++;m->selected=index+1;
                m->hub_status[index-1]|=0x10;
                if(!m->hub_stuck) {
                    m->hub_status[index-1]&=~0x10u;m->hub_change[index-1]|=0x10;
                    if(m->dev[m->selected].present) {
                        m->dev[m->selected].addr=0;m->dev[m->selected].in_toggle=0;m->dev[m->selected].reset_series++;
                        if(!(m->hub_status[index-1]&2)) m->hub_change[index-1]|=2;
                        m->hub_status[index-1]|=2;
                    }
                }
            } else if(req==3 && value==1) m->hub_status[index-1]|=2;
            else if(req==1 && value>=16 && value<=20) {
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
    if(!m->ohci && off==2)return (m->regs[0]>>16)|((m->regs[0]&1)?0:0x20)|(m->host_error?8:0);
    if(m->ohci && off==0x0c && m->host_error)return 0x10;
    return m->regs[off/4] >> (8*(off&3));
}
static void write_reg(void *arg,unsigned off,uint32_t value,unsigned width)
{
    struct model *m=arg;writes++;
    if(!m->ohci && off==2) {m->regs[0]&=~(value<<16);return;}
    if(!m->ohci && off==0 && (value&2) && !m->reset_stuck) {
        /* QEMU uhci_reset(): stopped, empty schedule, attached devices
         * reappear with CCS/CSC, but PE stays clear until software sets it. */
        m->regs[0]=m->regs[1]=m->regs[2]=0;m->regs[0x0c/4]=64;
        m->regs[0x10/4]=0;
        for(unsigned p=0;p<2;p++) {
            unsigned status=0x80|(m->dev[p].present ? 3u:0u)|(p==1 && m->dev[p].present && m->dev[p].kind!=HUB ? 0x100u:0u);
            m->regs[0x10/4]|=status<<(16*p); /* HCRESET does not reset the peripheral */
        }
        return;
    }
    if(m->ohci) {
        if(off==0x0c){m->regs[off/4]&=~value;return;}
        if(off>=0x54 && off<0x5c) {
            unsigned p=(off-0x54)/4;
            if(value==0x10){m->selected=p;m->dev[p].addr=0;m->dev[p].in_toggle=0;m->root_resets[p]++;m->reset_at[p]=m->now;
                if(m->root_resets[p]%5==1)m->dev[p].reset_series++;
                m->regs[off/4]|=0x100002;return;}
            if(value==2){m->root_enables[p]++;m->regs[off/4]|=2;return;}
            if(value==1){m->regs[off/4]&=~2u;return;}
            if(value & 0x1f0000){m->root_acks[p]++;m->regs[off/4]&=~(value & 0x1f0000);return;}
            return;
        }
    } else if(off==0x10 || off==0x12) {
        unsigned p=(off-0x10)/2;
        unsigned old=(m->regs[off/4]>>(8*(off&3)))&0xffff;
        if((value&0x200) && !(old&0x200)) {
            m->selected=p;m->dev[p].addr=0;m->dev[p].in_toggle=0;m->reset_at[p]=m->now;m->root_resets[p]++;m->dev[p].reset_series++;
        }
        if(!(value&0x200) && (old&0x200)) {
            assert(m->now-m->reset_at[p]>=50);m->recovery_at[p]=m->now;
        }
        if((value&4) && !(old&4) && (old&1)) {
            assert(!(value&0x200) && m->now-m->recovery_at[p]>=1);m->root_enables[p]++;
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
                unsigned cc=n>=0?0:n==-USB_EPIPE?4:n==-75?8:5;
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
static volatile uint32_t *uhci_dma(void *arg,uint32_t address,unsigned bytes)
{ return (volatile uint32_t *)dma_at(arg,address,bytes); }
static bool uhci_transaction(void *arg,uint32_t td_address,uint32_t qh_address,volatile uint32_t *td)
{
    struct model *m=arg;
    unsigned len=(((td[2]>>21)&0x7ff)+1)&0x7ff;
    uint8_t *buf=len ? dma_at(m,td[3],len):NULL;
    assert(len<=64);
    if(m->late_hid && ((td[2]>>15)&15)==1 && target(m,(td[2]>>8)&127)->kind==KBD) {
        m->latched_td=td_address;m->latched_ed=qh_address;return false;
    }
    int n=transaction(m,(td[2]>>8)&127,(td[2]>>15)&15,td[2]&255,(td[2]>>19)&1,buf,len);
    if(n==-USB_EAGAIN) {td[1]|=1u<<19;return false;}
    td[1]&=~((1u<<23)|(1u<<19)|0x7ffu);
    if(n<0) {td[1]|=n==-USB_EPIPE ? 1u<<22:n==-75 ? (1u<<20)|(1u<<22):(1u<<18)|(1u<<22);
        if(n==-75)td[1]=(td[1]&~0x7ffu)|2; /* T23 mouse babble: three bytes */
        if(n==-USB_ETIMEDOUT || n==-USB_EIO)td[1]&=~(3u<<27);
        m->regs[0]|=2u<<16;return false;}
    if(m->bad_length)n=(int)len+1;
    td[1]|=((unsigned)n-1)&0x7ff;
    if(m->bulk_record && (td[2]&255)==USB_PID_IN && ((td[2]>>15)&15)) {
        assert(m->bulk_recorded<ARRAY_SIZE(m->bulk_tokens));
        m->bulk_tokens[m->bulk_recorded++]=td[2];
    }
    return true;
}
static void execute_uhci(struct model *m,uint32_t link)
{
    uint32_t status=m->regs[0]>>16;
    struct uhci_model_io io={m,uhci_dma,uhci_transaction,&status};
    assert(!uhci_model_frame(&io,link));
    m->regs[0]|=status<<16;
    if(m->bulk_record && m->bulk_recorded) {
        volatile uint32_t *td=uhci_dma(m,m->host.dma_phys+USB_TD,16);
        if(!(td[1]&(1u<<23)))
            assert(uhci_dma(m,m->host.dma_phys+USB_UHCI_BULK_QH,8)[1]==1);
    }
}
/* Independent small schedules test the decision table, not the production
 * builder. Physical offsets: QHs 0/16/32, TDs 64/80/96. */
static struct uhci_rules {
    _Alignas(16) uint32_t words[32];
    uint32_t status, nak, error;
    unsigned count, order[8], fetches;
} rules;
static volatile uint32_t *rules_dma(void *arg,uint32_t address,unsigned bytes)
{
    struct uhci_rules *r=arg;
    assert(address>=0x100000 && address-0x100000<=sizeof(r->words)-bytes);
    assert(!(address&15));r->fetches++;
    return r->words+(address-0x100000)/4;
}
static bool rules_transaction(void *arg,uint32_t address,uint32_t qh,volatile uint32_t *td)
{
    struct uhci_rules *r=arg;(void)qh;
    assert(r->count<ARRAY_SIZE(r->order));r->order[r->count++]=address-0x100000;
    if(address==r->nak) {td[1]|=1u<<19;return false;}
    td[1]&=~((1u<<23)|(1u<<19)|0x7ffu);
    if(address==r->error) {td[1]|=1u<<22;return false;}
    return true; /* one-byte ACK */
}
static void rules_setup(unsigned depth)
{
    memset(&rules,0,sizeof(rules));
    rules.words[0]=0x100012;rules.words[1]=0x100044; /* element bit 2 ignored */
    rules.words[4]=1;rules.words[5]=0x100060;
    for(unsigned at=16;at<=24;at+=4) {
        rules.words[at]=1;rules.words[at+1]=0x188007ff;rules.words[at+2]=USB_PID_IN;
    }
    rules.words[16]=0x100050|(depth ? 4:0);
}
static void uhci_schedule_rules(void)
{
    struct uhci_model_io io={&rules,rules_dma,rules_transaction,&rules.status};
    for(unsigned depth=0;depth<2;depth++) {
        rules_setup(depth);assert(!uhci_model_frame(&io,0x100002));
        assert(rules.order[0]==64 && rules.order[rules.count-1]==96);
        if(depth) {assert(rules.count==3 && rules.order[1]==80 && rules.words[1]==1);}
        else {
            assert(rules.count==2 && rules.words[1]==0x100050 && (rules.words[21]&(1u<<23)));
            assert(!uhci_model_frame(&io,0x100002));
            assert(rules.count==3 && rules.order[2]==80 && rules.words[1]==1);
        }
    }
    /* T on a frame or horizontal link never falls through to adjacent RAM. */
    rules_setup(1);assert(!uhci_model_frame(&io,0x100003) && !rules.count && !rules.fetches);
    rules.words[1]=1;assert(!uhci_model_frame(&io,0x100002) && rules.count==1 && rules.order[0]==96);
    rules_setup(0);rules.words[0]=1;
    assert(!uhci_model_frame(&io,0x100002) && rules.count==1 && rules.order[0]==64);
    assert(rules.words[25]&(1u<<23)); /* unlinked QH/TD never executes */
    /* Inactive, NAK and error do not advance, even with a depth-first link. */
    for(unsigned failure=0;failure<3;failure++) {
        rules_setup(1);
        if(!failure) rules.words[17]=(1u<<24)|0x7ff; /* inactive + IOC */
        if(failure==1) rules.nak=0x100040;
        if(failure==2) rules.error=0x100040;
        assert(!uhci_model_frame(&io,0x100002));
        assert(rules.words[1]==0x100044 && (rules.words[21]&(1u<<23)));
        assert(rules.order[rules.count-1]==96 && rules.count==(failure ? 2u:1u));
        if(!failure) assert(rules.status==1);
        if(failure==1) assert(rules.words[17]==0x188807ff && !rules.status);
        if(failure==2) assert(!(rules.words[17]&(1u<<23)) && (rules.words[17]&(1u<<22)));
    }
    rules_setup(0);rules.words[17]|=1u<<24;
    assert(!uhci_model_frame(&io,0x100002) && rules.status==1 && rules.count==2);
    /* A short SPD IN stops this queue without advancing its element. */
    rules_setup(1);rules.words[17]|=1u<<29;rules.words[18]|=1u<<21;
    assert(!uhci_model_frame(&io,0x100002) && rules.status==1 && rules.count==2 && rules.words[1]==0x100044);
    /* The depth flag belongs to the TD link. A QH reached through an element
     * replaces the old context: the old horizontal branch is not revisited. */
    for(unsigned depth=0;depth<2;depth++) {
        rules_setup(0);rules.words[0]=1;rules.words[16]=0x100012|(depth ? 4:0);
        assert(!uhci_model_frame(&io,0x100002));
        assert(rules.count==(depth ? 2u:1u) && rules.words[1]==rules.words[16]);
        if(!depth) assert(!uhci_model_frame(&io,0x100002) && rules.count==2);
        assert(rules.order[1]==96);
    }
    rules_setup(0);rules.words[1]=0x100012;
    assert(!uhci_model_frame(&io,0x100002) && rules.count==1 && rules.order[0]==96);
    /* Direct TDs follow their own link outside a Q context; Vf is ignored. */
    rules_setup(1);assert(!uhci_model_frame(&io,0x100040) && rules.count==2 && rules.order[1]==80);
    rules_setup(0);assert(uhci_model_frame(&io,0x100006)==-USB_EPROTO && !rules.fetches);
    rules_setup(0);rules.words[0]|=4;assert(uhci_model_frame(&io,0x100002)==-USB_EPROTO);
    rules_setup(0);rules.words[16]|=8;assert(uhci_model_frame(&io,0x100002)==-USB_EPROTO && !rules.count);
    puts("PASS USB strict UHCI traversal: frame/QH/TD T/Q/Vf, breadth/depth, unreachable queues, inactive/IOC, NAK/error/SPD, reserved bits");
}
static void complete_late_hid(struct model *m)
{
    if(!m->latched_td)return;
    volatile uint32_t *ed=(volatile uint32_t *)dma_at(m,m->latched_ed,16);
    /* Complete a previously fetched TD just before software observes it.
     * NAKs no longer authorize unlinking a pending interrupt TD. */
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
        if(m->debounce_bounce && m->now<=50 && m->now%25==0) {
            if(m->ohci)m->regs[0x54/4]|=0x10000;
            else m->regs[0x10/4]|=2;
            m->debounce_changes++;
        }
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
            execute_uhci(m,*(uint32_t *)dma_at(m,m->regs[2]+frame,4));
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
    schedule_entries=schedule_qhs=schedule_tds=schedule_invalid=0;
    port_actions=recovery_records=0;
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
static void uhci_schedule_regression(void)
{
    const uint32_t quiet_flags[] = {0, CBI_F_VERBOSE, CBI_F_TEST_REQUEST};
    for (unsigned i=0;i<ARRAY_SIZE(quiet_flags);i++) {
        setup(false,false);g_boot.flags=quiet_flags[i];assert(!attach());model_step();
        assert(!schedule_entries && !schedule_qhs && !hid_polls[1] && !hid_pipes[1]);
        usb_detach(&m.host.devices[0],"test");usb_detach(&m.host.devices[1],"test");
    }
    setup(false,false);assert(!attach());
    for(unsigned frame=0;frame<1024;frame++) assert(((uint32_t *)m.dma)[frame]==0x101202);
    uint32_t link=((uint32_t *)m.dma)[0];
    for(unsigned i=0;i<USB_DEVICE_MAX*USB_HID_MAX;i++) {
        assert(link==(m.host.dma_phys+USB_HID_BASE+i*USB_HID_STRIDE+2));
        volatile uint32_t *qh=uhci_dma(&m,link&~15u,8);
        assert(qh[1]==1);link=qh[0];
    }
    assert(link==(m.host.dma_phys+USB_UHCI_CONTROL_QH+2));
    volatile uint32_t *control=uhci_dma(&m,link&~15u,8);
    assert(control[1]==1 && control[0]==(m.host.dma_phys+USB_UHCI_BULK_QH+2));
    volatile uint32_t *bulk=uhci_dma(&m,control[0]&~15u,8);assert(bulk[0]==1 && bulk[1]==1);
    model_step();assert(m.host.device_count==2 && !schedule_entries && !schedule_qhs);
    g_boot.flags=CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE;m.host.next_scan=m.now;model_step();
    assert(schedule_entries==4 && schedule_qhs==66 && schedule_tds==2 && !schedule_invalid);
    assert(m.host.schedule_dumped);model_step();assert(schedule_entries==4 && schedule_qhs==66);
    /* Inject a missing interrupt-tail -> control edge. This reproduces the
     * observable EAGAIN/addr0 timeout combination, but is NOT the F1-52
     * baseline: its original control QH was reachable (see validation). */
    volatile uint32_t *tail=uhci_dma(&m,m.host.dma_phys+USB_HID_BASE+63*USB_HID_STRIDE,8);
    uint32_t saved=tail[0];tail[0]=1;m.nak=true;
    m.dev[0].report[2]=4;m.dev[0].report_len=8;
    m.dev[1].report[1]=1;m.dev[1].report_len=4;
    struct usb_device zero={.host=&m.host,.ep0=8};uint8_t descriptor[8];
    usb_host_take(&m.host);
    assert(usb_control(&zero,0x80,6,0x100,0,descriptor,8)==-USB_ETIMEDOUT);
    usb_host_drop(&m.host);
    assert(zero.packet_trace.actual==-1 && zero.packet_trace.status==0x188007ff);
    assert(m.host.ready && m.dev[0].report_len==8 && m.dev[1].report_len==4);
    for(unsigned i=0;i<2;i++) {
        struct usb_device *d=&m.host.devices[i];struct usb_packet_trace *t=&d->hid[0].trace;
        for(unsigned j=0;j<20 && !t->valid;j++) {
            sleep_ms(&m,1);usb_host_take(&m.host);usb_hid_service(&m.host);usb_host_drop(&m.host);
        }
        assert(t->valid && t->result==-USB_EAGAIN && t->status==(0x188807ffu|(d->low_speed ? 1u<<26:0)));
    }
    tail[0]=saved;m.nak=false;
    zero.address=m.host.devices[0].address; /* a configured device no longer answers addr0 */
    usb_host_take(&m.host);
    assert(usb_control(&zero,0x80,6,0x100,0,descriptor,8)==8);
    usb_host_drop(&m.host);model_step();
    assert(held[INPUT_KEY_A]==1 && last_x==1 && !m.dev[0].report_len && !m.dev[1].report_len);
    /* Diagnostic reads fail safely for invalid DMA pointers and cycles. */
    m.host.schedule_dumped=false;((uint32_t *)m.dma)[0]=(m.host.dma_phys+USB_DMA_BYTES)|2;
    usb_uhci_dump_schedule(&m.host);assert(schedule_invalid==1);
    ((uint32_t *)m.dma)[0]=m.host.dma_phys+USB_HID_BASE+2;m.host.schedule_dumped=false;
    bulk[0]=m.host.dma_phys+USB_HID_BASE+2;usb_uhci_dump_schedule(&m.host);assert(schedule_invalid==2);bulk[0]=1;
    usb_detach(&m.host.devices[0],"test");usb_detach(&m.host.devices[1],"test");
    setup(true,false);g_boot.flags=CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE;assert(!attach());model_step();assert(!schedule_entries && !schedule_qhs);
    setup(false,false);g_boot.flags=CBI_F_TEST_REQUEST | CBI_F_DIAG_VERBOSE;assert(!attach());model_step();
    assert(schedule_entries==4 && schedule_qhs==66 && schedule_tds==2);
    usb_detach(&m.host.devices[0],"test");usb_detach(&m.host.devices[1],"test");
    puts("PASS USB UHCI periodic/control/bulk reachability, missing-tail fault reproduces NAK EAGAIN/addr0 timeout, restored ACK/input, once-only bounded verbose/test dump");
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
    model_step();assert(m.host.devices[0].quarantined && m.host.root_blocked[0] && m.now-begin<7500);
    assert(descriptor_failures==12 && descriptor_results==descriptor_starts);
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
    g_boot.flags=CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE;
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
    g_boot.flags=CBI_F_TEST_REQUEST | CBI_F_DIAG_VERBOSE; /* Accepted fw_cfg selector. */
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
    uint8_t large[1024];
    assert(!usb_bulk(d,true,large,sizeof(large),&n) && n==sizeof(large));
    struct usb_storage_trace *t=&d->storage.trace;
    assert(t->packet_count==16 && t->packet_index==15 && t->packet_offset==960);
    for(unsigned i=0;i<USB_BULK_TRACE_LIMIT;i++)
        assert(t->packets[i].valid && t->packets[i].actual==64 && t->packets[i].toggle==(toggle^(i&1)));
    for(unsigned i=0;i<sizeof(large);i++)assert(large[i]==0x5a);
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
    printf("PASS USB %s bulk: CBW/CSW-sized packets, 512-byte IN/OUT, bounded eight-packet prefix of 1024-byte IN, short IN/ZLP, NAK budget, STALL/timeout toggles, descriptor MPS, retired DMA, invalid endpoints/speed\n",ohci?"OHCI":"UHCI");
}
static void binding_eighth_packet(void)
{
    for(unsigned initial=0;initial<2;initial++) for(unsigned mode=0;mode<4;mode++) {
        setup(false,false);assert(!attach());assert(!m.host.ops->port_reset(&m.host,0));
        m.bulk_test=m.bulk_record=true;m.bulk_in_toggle=initial;
        struct usb_device *d=&m.host.devices[0];
        *d=(struct usb_device){.host=&m.host,.present=true,.configured=true,.ep0=64};
        d->storage=(struct usb_storage){.bound=true,.in=1,.out=2,.in_packet=64,.out_packet=64,
            .in_toggle=initial,.binding=true,.bind_deadline=m.now+USB_BIND_TIMEOUT_MS};
        /* The first packet becomes ready inside its 1 s budget. Seven ACKs
         * plus per-packet retirement reach the old whole-stage deadline.
         * The device has all remaining packets ready, including number 8. */
        uint64_t begin=m.now;
        m.bulk_ready_at=begin+(mode==1 ? USB_BIND_BULK_MS-20:0);
        if(mode==2)m.bulk_block_after=7; /* genuinely pending eighth TD */
        if(mode==3)d->storage.bind_deadline=begin+USB_BIND_FINISH_MS+21;
        uint8_t data[512];memset(data,0xcc,sizeof(data));unsigned actual;
        usb_host_take(&m.host);
        int e=usb_bulk(d,true,data,sizeof(data),&actual);
        usb_host_drop(&m.host);
        if(e && mode<2) fprintf(stderr,"F1-60 first_toggle=%u delayed=%u result=%d actual=%u ACKs=%u last_td=%08x last_result=%d elapsed_ms=%llu\n",
            initial,mode,e,actual,m.bulk_recorded,d->packet_trace.status,d->packet_trace.result,
            (unsigned long long)(m.now-begin));
        struct usb_storage_trace *t=&d->storage.trace;
        assert(t->packet_count==8 && t->packet_index==7 && t->packet_offset==448);
        unsigned acks=mode<2 ? 8:7;
        if(mode<2)assert(!e && actual==sizeof(data) && !strcmp(t->bulk_stop,"complete"));
        else {
            assert(e==-USB_ETIMEDOUT && actual==448 && t->packets[7].actual==-1);
            assert(t->packets[7].result==-USB_ETIMEDOUT && t->packets[7].toggle==(initial^1));
            assert(t->packets[7].valid==(mode==2));
            if(mode==2) {
                assert((t->packets[7].status&0x00880000)==0x00880000);
                assert(t->packets[7].elapsed_ms==USB_BIND_BULK_MS+2);
                assert(!strcmp(t->bulk_stop,"packet"));
            } else {
                assert(!t->packets[7].status && !t->packets[7].token);
                assert(t->bind_remaining_ms==USB_BIND_FINISH_MS);
                assert(!strcmp(t->bulk_stop,"bind_deadline"));
            }
        }
        assert(m.bulk_recorded==acks && d->storage.in_toggle==(initial^(acks&1)));
        assert(m.bulk_in_toggle==d->storage.in_toggle);
        for(unsigned i=0;i<acks;i++) {
            assert(((m.bulk_tokens[i]>>19)&1)==(initial^(i&1)));
            assert(((m.bulk_tokens[i]>>21)&0x7ff)==63);
            assert(t->packets[i].valid && t->packets[i].actual==64 && t->packets[i].result==64);
            assert(t->packets[i].toggle==(initial^(i&1)) && !t->packets[i].retired);
            assert(t->packets[i].token==m.bulk_tokens[i]);
        }
        for(unsigned i=0;i<sizeof(data);i++)assert(data[i]==(i<actual ? 0x5a:0xcc));
        assert(!d->storage.bulk_deadline && m.now<d->storage.bind_deadline);
    }
    puts("PASS F1-60 UHCI eight 64-byte IN TDs: DATA0/DATA1 starts, QH advance, delayed ACK progress, eighth retirement/NAK, aggregate deadline and eight packet snapshots");
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
static void enumeration_retries(bool ohci)
{
    for(unsigned failures=1;failures<=6;failures++) {
        setup(ohci,false);m.dev[1].present=false;
        if(ohci)m.regs[0x58/4]=0;
        m.dev[0].addr=17; /* BIOS address survives HCRESET, not port reset. */
        m.dev[0].descriptor_failures=failures;
        assert(!attach() && m.dev[0].addr==17);
        if(failures==1) {
            m.now++;
            assert(!m.host.ops->port_enable(&m.host,0));
            struct usb_device probe={.host=&m.host,.ep0=8};uint8_t bytes[8];
            assert(usb_control(&probe,0x80,6,0x100,0,bytes,sizeof(bytes))==-USB_EIO);
            assert(!m.root_resets[0] && m.dev[0].addr==17); /* ignores addr0 until a physical reset */
        }
        model_step();
        struct usb_device *d=&m.host.devices[0];
        unsigned attempts=failures==6 ? 6:failures+1;
        assert(m.host.device_count==1 && m.dev[0].descriptor_attempts==attempts);
        assert(m.dev[0].reset_series==(failures>=3 ? 2u:1u));
        assert(descriptor_starts==attempts && descriptor_failures==failures);
        for(unsigned i=1;i<attempts;i++) {
            unsigned delay=i==3 ? 60:100;
            assert(m.dev[0].descriptor_at[i]-m.dev[0].descriptor_at[i-1]>=delay);
        }
        assert(port_actions && d->quarantined==(failures==6));
        if(failures<6) assert(d->configured && d->hid[0].active && d->address==1);
        else {
            unsigned resets=m.root_resets[0];
            for(unsigned scan=0;scan<10;scan++){m.now+=250;model_step();}
            assert(m.host.root_blocked[0] && m.root_resets[0]==resets && m.host.device_count==1);
        }
    }
    setup(ohci,false);m.dev[0].second_reset=true;assert(!attach());model_step();
    assert(m.host.devices[0].configured && m.dev[0].descriptor_attempts==4 && m.dev[0].reset_series==2);
    if(!ohci) {
        setup(false,false);assert(!attach());m.debounce_bounce=true;
        model_step();assert(m.debounce_changes==2 && m.reset_at[0]>=150);
    }
    printf("PASS USB %s BIOS address, first SETUP timeout, 2x3 descriptor attempts, spacing/reset/recovery, bounded quarantine and debounce\n",ohci?"OHCI":"UHCI");
}
static void live_port(bool ohci)
{
    setup(ohci,false);assert(!attach());model_step();
    struct usb_device *d=&m.host.devices[0];
    unsigned count=m.host.device_count,resets=m.root_resets[0],addr=d->address;
    m.dev[0].report[2]=4;m.dev[0].report_len=8;model_step();assert(held[INPUT_KEY_A]);
    if(ohci)m.regs[0x54/4]|=0x30000;
    else m.regs[0x10/4]|=0x0a;
    m.now+=1100;model_step();
    assert(m.host.device_count==count && m.root_resets[0]==resets && d->address==addr && d->hid[0].active);
    assert(m.host.ops->port_reset(&m.host,0)==-USB_EAGAIN);
    m.host.ops->port_disable(&m.host,0);
    assert(m.host.ops->port_status(&m.host,0)&USB_PORT_ENABLED);
    unsigned enables=m.root_enables[0];
    /* Controller PEDC with CCS still set. Re-enable without PR/address loss. */
    if(ohci)m.regs[0x54/4]=(m.regs[0x54/4]&~2u)|0x20000;
    else m.regs[0x10/4]=(m.regs[0x10/4]&~4u)|8;
    m.now+=300;model_step();
    assert(m.root_enables[0]==enables+1 && m.root_resets[0]==resets && d->address==addr);
    assert(held[INPUT_KEY_A] && usb_devices==2 && !usb_detaches);
    memset(m.dev[0].report,0,8);m.dev[0].report_len=8;model_step();assert(!held[INPUT_KEY_A]);
    printf("PASS USB %s live CCS+CSC/PEDC: retained input/address, no reset/disable, explicit PE recovery\n",ohci?"OHCI":"UHCI");
}
static void hid_recovery(bool ohci)
{
    for(unsigned babble=0;babble<2;babble++) for(unsigned persistent=0;persistent<2;persistent++) {
        setup(ohci,false);assert(!attach());model_step();
        struct usb_device *d=&m.host.devices[0];struct usb_hid *p=&d->hid[0];
        m.dev[0].report[2]=4;m.dev[0].report_len=8;model_step();assert(p->toggle==1 && held[INPUT_KEY_A]);
        m.dev[0].babble=babble;m.dev[0].hid_errors=persistent ? 2:1;m.disable_on_babble=true;
        unsigned resets=m.root_resets[0];
        model_step(); /* retired error; general owner clears halt */
        for(unsigned i=0;i<3;i++)model_step();
        assert(m.dev[0].clear_halts==1 && recovery_records==1 && m.root_resets[0]==resets);
        if(persistent) assert(d->quarantined && !p->active && !held[INPUT_KEY_A]);
        else {
            assert(!d->quarantined && p->active && p->toggle==0 && held[INPUT_KEY_A]);
            memset(m.dev[0].report,0,8);m.dev[0].report_len=8;model_step();
            assert(!held[INPUT_KEY_A] && p->toggle==1 && !p->recovery_tried);
        }
    }
    setup(ohci,false);assert(!attach());model_step();
    m.dev[0].hid_errors=1;m.dev[0].clear_fail=true;
    for(unsigned i=0;i<3;i++)model_step();
    assert(m.host.devices[0].quarantined && !m.dev[0].clear_halts);
    setup(ohci,false);assert(!attach());model_step();
    m.dev[0].hid_errors=1;m.dev[0].crc=true;
    for(unsigned i=0;i<3;i++)model_step();
    assert(m.host.devices[0].quarantined && !m.dev[0].clear_halts && !recovery_records);
    printf("PASS USB %s interrupt STALL/babble: one clear-halt, DATA0 retry, PE recovery, persistent/clear-failure quarantine\n",ohci?"OHCI":"UHCI");
}
static void persistent_nak(bool ohci)
{
    setup(ohci,false);g_boot.flags=CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE;assert(!attach());model_step();
    struct usb_device *d=&m.host.devices[0];struct usb_hid *p=&d->hid[0];
    assert(p->pipe_state==1);
    uint32_t original[4];memcpy(original,m.dma+USB_HID_BASE+16,sizeof(original));
    unsigned records=hid_polls[d->address];
    for(unsigned i=0;i<60;i++) {m.now+=20;model_step();assert(p->pipe_state==1 && !p->toggle && !d->quarantined);}
    volatile uint32_t *td=(volatile uint32_t *)(m.dma+USB_HID_BASE+16);
    assert(ohci ? td[0]==original[0] && td[1]==original[1] && td[2]==original[2] :
                  td[0]==original[0] && td[2]==original[2] && td[3]==original[3] && (td[1]&(1u<<23)));
    assert(hid_polls[d->address]<=USB_HID_TRACE_LIMIT && hid_polls[d->address]>=records);
    m.dev[0].report[2]=4;m.dev[0].report_len=8;model_step();
    assert(held[INPUT_KEY_A] && p->toggle==1 && !m.dev[0].report_len);
    memset(m.dev[0].report,0,8);m.dev[0].report_len=8;model_step();assert(!held[INPUT_KEY_A]);
    printf("PASS USB %s >1 s idle NAK: same linked TD, bounded -11 records, later ACK/toggle/re-arm\n",ohci?"OHCI":"UHCI");
}
static void ohci_batches(void)
{
    for(unsigned slow=0;slow<2;slow++) {
        setup(true,false);g_boot.flags=CBI_F_TEST_REQUEST | CBI_F_DIAG_VERBOSE;assert(!attach());model_step();
        assert(!m.host.quarantined && m.dev[0].reset_series==1 && m.dev[1].reset_series==1);
        assert(!(m.regs[0x54/4]&0x1f0000)); /* independent CSC/PRSC acknowledged */
        if(slow) {m.frame_ms=4;m.sleep_gap=9;}
        for(unsigned round=0;round<3;round++) {
            m.dev[0].report[2]=0x45;m.dev[0].report_len=8;model_step();assert(held[0x58]);
            memset(m.dev[0].report,0,8);m.dev[0].report_len=8;model_step();assert(!held[0x58]);
            /* The runner's first pointer batch (batch 2) follows key down/up.
             * Leave NAK TDs linked across a long idle interval, then re-arm. */
            for(unsigned idle=0;idle<10;idle++)model_step();
            unsigned reports=hid_reports[m.host.devices[1].address];
            for(unsigned batch=0;batch<4;batch++) {
                m.dev[1].report[0]=batch==2;m.dev[1].report[1]=batch<2 ? 0x81:0;
                m.dev[1].report[2]=batch<2 ? 0x81:0;m.dev[1].report_len=4;
                model_step();
                assert(!m.dev[1].report_len && m.host.ready && usb_devices==2);
                if(batch<2)assert(last_x==-127 && last_y==-127);
                else assert(buttons[0]==(batch==2));
            }
            assert(hid_reports[m.host.devices[1].address]==reports+4);
            volatile uint32_t *ed=(volatile uint32_t *)(m.dma+USB_HID_BASE+USB_HID_MAX*USB_HID_STRIDE);
            assert(ed[3]==m.host.dma_phys+USB_HID_BASE+(USB_HID_MAX+1)*USB_HID_STRIDE);
        }
    }
    puts("PASS USB OHCI PRSC reset, runner batch 2 after F12, repeated ED/TD re-arm and button release, independent/delayed clocks");
}
static void t23_receiver(bool disabled)
{
    setup(false,false);g_boot.flags=CBI_F_TEST_REQUEST;
    m.dev[0].kind=RECEIVER;m.dev[1].present=false;
    assert(!attach());model_step();
    struct usb_device *d=&m.host.devices[0];
    assert(d->vendor==0x046d && d->product==0xc52b && usb_devices==2);
    assert(d->hid[0].endpoint==1 && d->hid[1].endpoint==2 && d->hid[1].interface==1);
    m.dev[0].report[2]=4;m.dev[0].report_len=8;
    unsigned wheels=wheel_events;
    m.dev[0].mouse_report[0]=1;m.dev[0].mouse_report[1]=3;m.dev[0].mouse_report[3]=0x7f;
    m.dev[0].mouse_length=8;
    model_step();assert(held[INPUT_KEY_A] && buttons[0] && last_x==3 && wheel_events==wheels);
    unsigned resets=m.root_resets[0],address=d->address;
    m.regs[0x10/4]=0x9b; /* CCS/CSC/PEDC/D+, PE dropped on the live receiver */
    m.now+=300;model_step();
    assert(m.root_resets[0]==resets && m.host.device_count==1 && d->address==address);
    assert((m.regs[0x10/4]&0xffff)==0x95 && usb_devices==2);
    /* The supplied T23 babble snapshot still has PE=1 (PORTSC 0x95).
     * The separate recovery fixture covers HC-driven PE loss on babble. */
    m.dev[0].hid_errors=1;m.dev[0].error_endpoint=2;m.dev[0].babble=true;m.disable_on_babble=disabled;
    model_step();model_step();
    assert(m.dev[0].last_clear==0x82 && m.dev[0].clear_halts==1);
    assert(d->hid[0].toggle==1 && d->hid[1].toggle==0 && m.dev[0].in_toggle==1 && !m.dev[0].mouse_toggle);
    assert(!d->quarantined && held[INPUT_KEY_A] && buttons[0] && usb_devices==2 && !usb_detaches);
    memset(m.dev[0].report,0,8);m.dev[0].report_len=8;
    memset(m.dev[0].mouse_report,0,4);m.dev[0].mouse_report[2]=2;m.dev[0].mouse_length=3;
    model_step();
    assert(!held[INPUT_KEY_A] && !buttons[0] && last_y==2 && usb_devices==2 && m.root_resets[0]==resets);
    printf("PASS USB T23 composite 046d:c52b: legal 8-byte boot mouse suffix, CCS/CSC/PEDC recovery, endpoint 2 three-byte babble, clear 0x82 only, keyboard/mouse survive, shared_PE_loss=%u\n",disabled);
}
int main(void)
{
    if(getenv("USB_TEST_F1_60_ONLY")) {binding_eighth_packet();return 0;}
    binding_eighth_packet();
    enumeration_retries(false);enumeration_retries(true);
    live_port(false);live_port(true);hid_recovery(false);hid_recovery(true);
    persistent_nak(false);persistent_nak(true);
    ohci_batches();
    t23_receiver(false);t23_receiver(true);
    uhci_schedule_rules();uhci_schedule_regression();
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
