/* Boot keyboard state diffing and bounded mouse report layouts.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/input.h>
static uint8_t leds;
/* USB keyboard usages -> public set-1 positions, never ASCII. */
static const uint16_t keycode[0x74] = {
    [4]=0x1e,[5]=0x30,[6]=0x2e,[7]=0x20,[8]=0x12,[9]=0x21,[10]=0x22,
    [11]=0x23,[12]=0x17,[13]=0x24,[14]=0x25,[15]=0x26,[16]=0x32,
    [17]=0x31,[18]=0x18,[19]=0x19,[20]=0x10,[21]=0x13,[22]=0x1f,
    [23]=0x14,[24]=0x16,[25]=0x2f,[26]=0x11,[27]=0x2d,[28]=0x15,[29]=0x2c,
    [30]=2,[31]=3,[32]=4,[33]=5,[34]=6,[35]=7,[36]=8,[37]=9,[38]=10,[39]=11,
    [40]=0x1c,[41]=1,[42]=0x0e,[43]=0x0f,[44]=0x39,[45]=0x0c,[46]=0x0d,
    [47]=0x1a,[48]=0x1b,[49]=0x2b,[50]=0x2b,[51]=0x27,[52]=0x28,
    [53]=0x29,[54]=0x33,[55]=0x34,[56]=0x35,[57]=0x3a,
    [58]=0x3b,[59]=0x3c,[60]=0x3d,[61]=0x3e,[62]=0x3f,[63]=0x40,
    [64]=0x41,[65]=0x42,[66]=0x43,[67]=0x44,[68]=0x57,[69]=0x58,
    [70]=0x137,[71]=0x46,[72]=INPUT_KEY_PAUSE,[73]=0x152,[74]=0x147,
    [75]=0x149,[76]=0x153,[77]=0x14f,[78]=0x151,[79]=0x14d,[80]=0x14b,
    [81]=0x150,[82]=0x148,[83]=0x45,[84]=0x135,[85]=0x37,[86]=0x4a,
    [87]=0x4e,[88]=0x11c,[89]=0x4f,[90]=0x50,[91]=0x51,[92]=0x4b,
    [93]=0x4c,[94]=0x4d,[95]=0x47,[96]=0x48,[97]=0x49,[98]=0x52,
    [99]=0x53,[100]=0x56,[101]=0x15d,
};
static const uint16_t modifiers[8] = {0x1d,0x2a,0x38,0x15b,0x11d,0x36,0x138,0x15c};
static uint16_t code(unsigned usage) { return usage < ARRAY_SIZE(keycode) ? keycode[usage] : 0; }
static bool has(const uint8_t *b, uint16_t c)
{
    for (unsigned i = 2; i < 8; i++) if (code(b[i]) == c && c) return true;
    return false;
}
static bool first(const uint8_t *b, unsigned i)
{
    for (unsigned j = 2; j < i; j++) if (code(b[j]) == code(b[i])) return false;
    return true;
}
static bool trace_enabled(void)
{ return g_boot.flags & (CBI_F_VERBOSE | CBI_F_TEST_REQUEST); }
static void drop_record(struct usb_device *d, struct usb_hid *h, const char *reason,
                        unsigned len, unsigned usage)
{
    if (trace_enabled())
        klog("[input] usb drop reason=%s addr=%u interface=%u protocol=%u len=%u usage=%02x",
             reason, d->address, h->interface, h->protocol, len, usage);
}
static void key(struct usb_device *d, struct usb_hid *h, unsigned usage,
                uint16_t c, unsigned value)
{
    if (!c) return;
    input_usb_event(INPUT_KEY, c, (int32_t)value, d->generation, d->host->io.now(d->host->io.cookie));
    if (trace_enabled() && h->key_records < USB_HID_TRACE_LIMIT) {
        h->key_records++;
        klog("[input] usb key usage=%02x set1=%03x down=%u addr=%u interface=%u",
             usage, c, value, d->address, h->interface);
    }
}
static void report_record(struct usb_device *d, struct usb_hid *h,
                          const uint8_t *b, unsigned len)
{
    if (!trace_enabled() || h->report_records >= USB_HID_TRACE_LIMIT) return;
    h->report_records++;
    static const char hex[]="0123456789abcdef";
    char bytes[USB_PACKET_MAX*3]; unsigned at=0;
    for (unsigned i=0;i<len && i<USB_PACKET_MAX;i++) {
        if (i) bytes[at++]=' ';
        bytes[at++]=hex[b[i]>>4]; bytes[at++]=hex[b[i]&15];
    }
    bytes[at]=0;
    klog("[usb] hid report addr=%u len=%u bytes=%s interface=%u",
         d->address,len,bytes,h->interface);
}
static void poll_record(struct usb_device *d, struct usb_hid *h, int result)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE) || (result == -USB_EAGAIN && h->poll_records >= USB_HID_TRACE_LIMIT)) return;
    if (h->poll_records < USB_HID_TRACE_LIMIT) h->poll_records++;
    const struct usb_packet_trace *t=&h->trace;
    /* This snapshot precedes LED/hub control or quarantine reusing DMA.
     * poll_frames excludes the two unlinked retirement frames. */
    klog_diag("[usb] hid poll addr=%u interface=%u ep=%u interval=%u toggle=%u maxlen=%u result=%d controller=%s root=%u parent=%u",
         d->address,h->interface,h->endpoint,h->interval,h->toggle,h->packet,result,
         d->host->owner,d->root_port+1,d->parent);
    klog_diag("[usb] hid pipe addr=%u valid=%u td=%08x token=%08x hc=%08x poll_frames=%u frames=%u elapsed_ms=%llu retire_result=%d actual=%d",
         d->address,t->valid,t->status,t->token,t->hc_status,t->poll_frames,t->frames,
         t->elapsed_ms,t->retired,t->actual);
}
/* Bounded HID short-item parser. Offsets count Input bits independently for
 * each ID; Output/Feature items never move an Input offset. Unsupported
 * layouts are discarded as a whole before selecting report protocol. */
struct mouse_globals { uint32_t page, size, count, id; int32_t min, max; };
static uint32_t mouse_usage(uint32_t page, uint32_t v, unsigned bytes)
{ return bytes==4 ? v : (page<<16)|v; }
bool usb_mouse_parse(struct usb_hid *hid, const uint8_t *b, unsigned len)
{
    struct mouse_globals g={0}, stack[4]; unsigned sp=0;
    uint32_t usages[32], low=0, high=0; unsigned used=0;
    bool range=false, have_high=false, mouse[8]={0}, ids=false, unnumbered=false;
    unsigned depth=0, reports=0;
    struct usb_mouse_report layouts[USB_MOUSE_REPORTS]={0};
    if (!hid || !b || !len || len>USB_HID_DESCRIPTOR_MAX) return false;
    hid->mouse_reports=0; hid->wheel=false; hid->report_ids=false;
    for (unsigned at=0;at<len;) {
        unsigned item=b[at++], n=item&3; if (n==3) n=4;
        if (item==0xfe || n>len-at) return false;
        uint32_t v=0; for (unsigned i=0;i<n;i++) v|=(uint32_t)b[at+i]<<(8*i);
        int32_t sv=n==1 ? (int8_t)v : n==2 ? (int16_t)v : (int32_t)v;
        at+=n; unsigned tag=item&0xfc;
        if (tag==0x04) { if (v>0xffff) return false; g.page=v; }
        else if (tag==0x14) g.min=sv;
        else if (tag==0x24) g.max=g.min<0 ? sv : (int32_t)v;
        else if (tag==0x74) g.size=v;
        else if (tag==0x94) g.count=v;
        else if (tag==0x84) { if (!v || v>255) return false; g.id=v; ids=true; }
        else if (tag==0xa4) { if (n || sp==ARRAY_SIZE(stack)) return false; stack[sp++]=g; }
        else if (tag==0xb4) { if (n || !sp) return false; g=stack[--sp]; }
        else if (tag==0x08) { if (used==ARRAY_SIZE(usages)) return false; usages[used++]=mouse_usage(g.page,v,n); }
        else if (tag==0x18) { low=mouse_usage(g.page,v,n); range=true; }
        else if (tag==0x28) { high=mouse_usage(g.page,v,n); have_high=true; }
        else if (tag==0xa0) {
            if (depth==ARRAY_SIZE(mouse)) return false;
            bool parent=depth && mouse[depth-1];
            mouse[depth++]=parent || (v==1 && used && usages[0]==0x10002);
        } else if (tag==0xc0) { if (!depth || n) return false; depth--; }
        else if (tag==0x80) {
            if (!depth || !g.size || g.size>32 || !g.count || g.count>64 ||
                (range && (!have_high || high<low || (low>>16)!=(high>>16)))) return false;
            unsigned r=0; while (r<reports && layouts[r].id!=g.id) r++;
            if (r==reports) { if (reports==USB_MOUSE_REPORTS) return false; layouts[reports++].id=g.id; }
            struct usb_mouse_report *p=&layouts[r];
            unsigned bits=g.size*g.count;
            if (bits>64-p->bits || (g.id && bits>56-p->bits)) return false;
            if (!g.id) unnumbered=true;
            if (mouse[depth-1] && !(v&1)) {
                for (unsigned i=0;i<g.count;i++) {
                    uint32_t u=i<used ? usages[i] : range ? low+(i<=high-low ? i : high-low) : used ? usages[used-1] : 0;
                    struct usb_mouse_field *field=0;
                    if ((u>>16)==9 && (u&0xffff)>=1 && (u&0xffff)<=32) {
                        if ((v&7)!=2 || g.size!=1 || g.min!=0 || g.max!=1) return false;
                        unsigned button=u&0xffff;
                        if (button>p->button_count) p->button_count=button;
                        if (button<=3) field=&p->buttons[button-1];
                    } else if (u==0x10030 || u==0x10031 || u==0x10038 || u==0xc0238) {
                        if ((v&7)!=6 || g.size>16 || g.min>=0 || g.max<=0 ||
                            g.min<-(1<<(g.size-1)) || g.max>=(1<<(g.size-1))) return false;
                        field=u==0x10030 ? &p->x : u==0x10031 ? &p->y : u==0x10038 ? &p->wheel : &p->pan;
                    }
                    if (field) {
                        if (field->size) return false;
                        *field=(struct usb_mouse_field){.bit=p->bits+i*g.size,.size=g.size};
                    }
                }
            }
            p->bits+=bits;
        } else if ((item&0x0c)==8 && tag!=0x38 && tag!=0x48 && tag!=0x58 && tag!=0x78 && tag!=0x88 && tag!=0x98) return false;
        if ((item&0x0c)==0) { used=0; range=have_high=false; low=high=0; }
    }
    if (depth || sp || (ids && unnumbered)) return false;
    bool extended=false;
    for (unsigned i=0;i<reports;i++) {
        struct usb_mouse_report *p=&layouts[i];
        if ((p->x.size || p->y.size || p->wheel.size || p->pan.size || p->button_count) &&
            (!p->x.size || !p->y.size || !p->buttons[0].size)) return false;
        if (p->x.size && (p->wheel.size || p->button_count>3)) extended=true;
    }
    if (!extended) return false;
    memcpy(hid->mouse,layouts,sizeof(layouts)); hid->mouse_reports=reports;
    hid->report_ids=ids;
    hid->wheel=false;
    for (unsigned i=0;i<reports;i++) if (layouts[i].wheel.size) hid->wheel=true;
    return true;
}
bool usb_mouse_wheel_layout(const uint8_t *b, unsigned len)
{ struct usb_hid h={0}; return usb_mouse_parse(&h,b,len) && h.wheel; }
static void descriptor_record(struct usb_device *d, struct usb_hid *h, const uint8_t *b, unsigned len)
{
    static const char hex[]="0123456789abcdef";
    /* At most sixteen bounded lines, once per attach; no disk drain. */
    for (unsigned at=0;at<len;at+=32) {
        char bytes[65]; unsigned n=len-at; if (n>32) n=32;
        for (unsigned i=0;i<n;i++) { bytes[i*2]=hex[b[at+i]>>4]; bytes[i*2+1]=hex[b[at+i]&15]; }
        bytes[n*2]=0;
        klog("[usb] hid descriptor addr=%u interface=%u vid=%04x pid=%04x len=%u offset=%u bytes=%s",
             d->address,h->interface,d->vendor,d->product,len,at,bytes);
    }
}
static int32_t mouse_value(const uint8_t *b, struct usb_mouse_field f, bool sign)
{
    uint32_t v=0;
    for (unsigned i=0;i<f.size;i++) if (b[(f.bit+i)/8]&(1u<<((f.bit+i)%8))) v|=1u<<i;
    if (sign && f.size && (v&(1u<<(f.size-1)))) return (int32_t)v-(1<<f.size);
    return (int32_t)v;
}
int usb_mouse_decode(const struct usb_hid *h, const uint8_t *b, unsigned len,
                     struct usb_mouse_input *out)
{
    if (!h || !b || !out) return -EINVAL;
    *out=(struct usb_mouse_input){0};
    if (h->report_protocol) {
        if (!len) return -USB_EPROTO;
        unsigned id=h->report_ids ? b[0] : 0, r=0;
        while (r<h->mouse_reports && h->mouse[r].id!=id) r++;
        if (r==h->mouse_reports) return 0;
        const struct usb_mouse_report *p=&h->mouse[r];
        if (len<(p->bits+7)/8+(h->report_ids ? 1u : 0)) return -USB_EPROTO;
        if (!p->x.size) return 0;
        const uint8_t *payload=b+(h->report_ids ? 1:0);
        for (unsigned i=0;i<3;i++) if (mouse_value(payload,p->buttons[i],false)) out->buttons|=1u<<i;
        out->x=mouse_value(payload,p->x,true); out->y=mouse_value(payload,p->y,true);
        out->wheel=mouse_value(payload,p->wheel,true); out->pan=mouse_value(payload,p->pan,true);
    } else {
        if (len<(h->wheel ? 4u : 3u)) return -USB_EPROTO;
        out->buttons=b[0]&7; out->x=(int8_t)b[1]; out->y=(int8_t)b[2];
        if (h->wheel) out->wheel=(int8_t)b[3];
    }
    return 1;
}
int usb_hid_attach(struct usb_device *d, struct usb_hid *hid, unsigned report_len)
{
    if (hid->active) return -EINVAL;
    int err = usb_control(d, 0x21, 11, 0, hid->interface, 0, 0); if (err) return err;
    err = usb_control(d, 0x21, 10, 0, hid->interface, 0, 0);
    if (err && !(hid->protocol == 2 && err == -USB_EPIPE)) return err;
    if (hid->protocol == 2 && report_len && report_len <= USB_HID_DESCRIPTOR_MAX && hid->packet >= 4) {
        uint8_t b[USB_HID_DESCRIPTOR_MAX];
        err = usb_control(d, 0x81, 6, 0x2200, hid->interface, b, report_len);
        if (err>0) descriptor_record(d,hid,b,(unsigned)err);
        if (err == (int)report_len && usb_mouse_parse(hid,b,report_len)) {
            bool fits=true;
            for (unsigned i=0;i<hid->mouse_reports;i++)
                if ((hid->mouse[i].bits+7)/8+(hid->report_ids ? 1u : 0)>hid->packet) fits=false;
            if (fits) {
                err=usb_control(d,0x21,11,1,hid->interface,0,0);
                if (!err) hid->report_protocol=true;
                else if (err!=-USB_EPIPE) return err;
            }
            if (!hid->report_protocol) { hid->wheel=false; hid->mouse_reports=0; hid->report_ids=false; }
        } else if (err < 0 && err != -USB_EPIPE) return err;
    }
    hid->leds = 0xff;
    if (hid->protocol == 1) {
        uint8_t value=leds;
        err = usb_control(d, 0x21, 9, 0x200, hid->interface, &value, 1); if (err != 1) return err < 0 ? err : -USB_EPROTO;
        hid->leds = value;
    }
    hid->active = true;
    if (hid->protocol==2)
        klog("[usb] hid mouse protocol addr=%u interface=%u mode=%s reports=%u ids=%u",
             d->address,hid->interface,hid->report_protocol ? "report" : "boot",hid->mouse_reports,hid->report_ids);
    input_devices_changed(INPUT_USB, true);
    klog("[usb] hid %s attached addr=%u interface=%u wheel=%u", hid->protocol == 1 ? "keyboard" : "mouse", d->address, hid->interface, hid->wheel);
    return 0;
}
int usb_hid_report(struct usb_device *d, struct usb_hid *h, const uint8_t *b, unsigned len)
{
    if (!h->active || !b) {
        drop_record(d,h,!h->active ? "inactive" : "missing_report",len,0);
        return -EINVAL;
    }
    if (h->protocol == 1) {
        if (len != 8) { drop_record(d,h,"keyboard_length",len,0); return -USB_EPROTO; }
        /* HID F.4 requires ignoring the reserved second byte. */
        for (unsigned i=2;i<8;i++) if (b[i] && b[i]<=3) {
            drop_record(d,h,"keyboard_rollover",len,b[i]);
            return 0; /* retain last valid state, including modifiers */
        }
        unsigned translated=0; bool unsupported=false;
        for (unsigned i=0;i<8;i++) if ((b[0]^h->previous[0]) & (1u<<i)) {
            key(d,h,0xe0+i,modifiers[i],!!(b[0] & (1u<<i))); translated++;
        }
        for (unsigned i=2;i<8;i++) {
            uint16_t c=code(h->previous[i]);
            if (c && first(h->previous,i) && !has(b,c)) { key(d,h,h->previous[i],c,0); translated++; }
        }
        if (h->repeat_key && !has(b,h->repeat_key)) h->repeat_key=0;
        for (unsigned i=2;i<8;i++) {
            uint16_t c=code(b[i]);
            if (b[i] && !c) { drop_record(d,h,"unsupported_usage",len,b[i]); unsupported=true; }
            if (c && first(b,i) && !has(h->previous,c)) {
                key(d,h,b[i],c,1); translated++;
                if (c==0x3a) leds^=2; else if (c==0x45) leds^=1; else if (c==0x46) leds^=4;
                else if (c!=INPUT_KEY_PAUSE) { h->repeat_key=c; h->repeat_at=d->host->io.now(d->host->io.cookie)+INPUT_REPEAT_DELAY_MS; }
            }
        }
        if (!translated && !unsupported) drop_record(d,h,"unchanged",len,0);
        memcpy(h->previous,b,8);
    } else {
        struct usb_mouse_input mouse;
        int parsed=usb_mouse_decode(h,b,len,&mouse);
        if (parsed<=0) {
            if (parsed<0) drop_record(d,h,"mouse_length",len,0);
            return parsed;
        }
        unsigned buttons=mouse.buttons;
        int32_t x=mouse.x,y=mouse.y,wheel=mouse.wheel;
        /* AC Pan is decoded independently. The public ABI has vertical
         * wheel only; horizontal pan must never become Y or wheel. */
        unsigned changed=(buttons^h->previous[0])&7;
        uint64_t tick=d->host->io.now(d->host->io.cookie);
        for (unsigned i=0;i<3;i++) if (changed&(1u<<i)) input_usb_event(INPUT_BTN,(uint16_t)i,!!(buttons&(1u<<i)),d->generation,tick);
        input_usb_event(INPUT_REL,INPUT_X,x,d->generation,tick);
        input_usb_event(INPUT_REL,INPUT_Y,y,d->generation,tick);
        if (h->wheel) input_usb_event(INPUT_REL,INPUT_WHEEL,wheel,d->generation,tick);
        if (wheel && trace_enabled() && h->key_records<USB_HID_TRACE_LIMIT) {
            h->key_records++;
            klog("[input] usb wheel addr=%u interface=%u value=%d",d->address,h->interface,wheel);
        }
        h->previous[0]=buttons;
    }
    return 0;
}
void usb_hid_release(struct usb_device *d, struct usb_hid *h)
{
    if (!h->active) return;
    if (d->host->ops->interrupt_stop) d->host->ops->interrupt_stop(d,h);
    if (h->protocol==1) {
        for (unsigned i=0;i<8;i++) if (h->previous[0] & (1u<<i)) key(d,h,0xe0+i,modifiers[i],0);
        for (unsigned i=2;i<8;i++) if (first(h->previous,i)) key(d,h,h->previous[i],code(h->previous[i]),0);
    } else for (unsigned i=0;i<3;i++) if (h->previous[0] & (1u<<i)) input_usb_event(INPUT_BTN,(uint16_t)i,0,d->generation,d->host->io.now(d->host->io.cookie));
    memset(h->previous,0,8); h->active=false; h->repeat_key=0;
    input_devices_changed(INPUT_USB, false);
}
void usb_hid_outputs(struct usb_device *d)
{
    for (unsigned i=0;i<USB_HID_MAX;i++) {
        struct usb_hid *h=&d->hid[i];
        if (h->active && h->protocol==1 && h->leds!=leds) {
            uint8_t value=leds;
            int err=usb_control(d,0x21,9,0x200,h->interface,&value,1);
            if (err!=1) { usb_device_quarantine(d,"hid_led_output"); return; }
            h->leds=value;
        }
    }
}
static void latency_window(struct usb_device *d, struct usb_hid *h, uint64_t tick)
{
    if (!h->latency_reports || tick-h->latency_window<1000) return;
    if (g_boot.flags & CBI_F_DIAG_VERBOSE)
        klog_diag("[usb] hid latency max addr=%u ms=%llu interface=%u reports=%u window_ms=%llu bound=upper",
             d->address,h->latency_max,h->interface,h->latency_reports,tick-h->latency_window);
    h->latency_reports=0; h->latency_max=0; h->latency_window=tick;
}
static void latency_record(struct usb_device *d, struct usb_hid *h)
{
    uint64_t tick=d->host->io.now(d->host->io.cookie), ms=tick-h->pending_at;
    latency_window(d,h,tick);
    if (!h->latency_reports) h->latency_window=tick;
    h->latency_reports++;
    if (ms>h->latency_max) h->latency_max=ms;
    if ((g_boot.flags & CBI_F_DIAG_VERBOSE) && h->latency_records<USB_HID_LATENCY_LIMIT) {
        h->latency_records++;
        klog_diag("[usb] hid latency addr=%u ms=%llu interface=%u observed_ms=%llu bound=upper",
             d->address,ms,h->interface,tick-h->observed_at);
    }
}
static void hid_fail(struct usb_device *d, const char *reason)
{
    if (!d->host->ops->interrupt) { usb_device_quarantine(d,reason); return; }
    /* The input worker must never issue hub control on the live general TD.
     * Stop input immediately; the scan owner disables/quarantines the port. */
    d->hid_fault=reason;
    d->host->next_scan=d->host->io.now(d->host->io.cookie);
    for (unsigned i=0;i<USB_HID_MAX;i++) usb_hid_release(d,&d->hid[i]);
}
void usb_hid_recover(struct usb_device *d)
{
    for (unsigned i=0;i<USB_HID_MAX && d->host->ready;i++) {
        struct usb_hid *h=&d->hid[i];
        if (!h->active || !h->recovery_pending) continue;
        int err=h->port_retry ? 0:usb_control(d,0x02,1,0,(uint16_t)(0x80u|h->endpoint),0,0);
        klog("[usb] hid recovery addr=%u interface=%u endpoint=%u fault=%d method=%s result=%d controller=%s",
             d->address,h->interface,h->endpoint,h->recovery_result,
             h->port_retry ? "port_enable":"clear_halt",err,d->host->owner);
        if (err) { hid_fail(d,"hid_clear_halt"); return; }
        if (!h->port_retry) h->toggle=0;
        h->port_retry=h->recovery_pending=false;
        h->next_poll=d->host->io.now(d->host->io.cookie);
    }
}
void usb_hid_poll(struct usb_device *d, struct usb_hid *h)
{
    if (!h->active || h->recovery_pending) return;
    struct usb_host *host=d->host;
    latency_window(d,h,host->io.now(host->io.cookie));
    bool async=host->ops->interrupt!=0;
    if (!async) usb_hid_outputs(d);
    if (!h->active) return;
    if (async || usb_expired(host,h->next_poll)) {
        uint8_t b[64]; int n;
        /* HID 1.11 Appendix B allows boot mouse suffixes up to eight
         * bytes. Request room for them; usb_hid_report uses the boot prefix. */
        if (async) n=host->ops->interrupt(d,h,b,h->packet<8 ? h->packet:8);
        else {
            h->pending_at=host->io.now(host->io.cookie);
            h->next_poll=h->pending_at+h->interval;
            n=host->ops->packet(d,h->endpoint,USB_PID_IN,h->toggle,b,h->packet,true);
            h->trace=d->packet_trace; h->trace.valid=true;
            h->observed_at=host->io.now(host->io.cookie);
        }
        if (n>=0) {
            h->recovery_tried=false;
            h->toggle^=1;
            if (usb_hid_report(d,h,b,(unsigned)n)) { hid_fail(d,"hid_report_length"); return; }
            latency_record(d,h);
            report_record(d,h,b,(unsigned)n);
        } else if (h->trace.valid || n!=-USB_EAGAIN) {
            poll_record(d,h,n);
            if (n!=-USB_EAGAIN) {
                bool babble=n==-USB_EIO && h->trace.valid &&
                    (host->pci.prog_if==0x20 ? (h->trace.status & 0x38)==0x10 :
                     host->pci.prog_if==0 ? (h->trace.status & 0x00360000u)==(1u<<20) :
                                           (h->trace.status>>28)==8);
                bool port_retry=false;
                if (n==-USB_EIO && h->trace.valid && host->ready && host->ops->port_status &&
                    (host->pci.prog_if==0x20 ? (h->trace.status & 0x38)==8 :
                     host->pci.prog_if==0 ? (h->trace.status & 0x00360000u)==(1u<<18) :
                                           (h->trace.status>>28)==5)) {
                    unsigned s=host->ops->port_status(host,d->root_port);
                    port_retry=(s & (USB_PORT_CONNECTED|USB_PORT_ENABLED|USB_PORT_OVERCURRENT))==USB_PORT_CONNECTED;
                }
                if ((n==-USB_EPIPE || babble || port_retry) && !h->recovery_tried && host->ready) {
                    h->recovery_pending=h->recovery_tried=true;h->recovery_result=n;
                    h->port_retry=port_retry;
                    host->next_scan=host->io.now(host->io.cookie);
                    return;
                }
                hid_fail(d,n==-USB_EPIPE ? "hid_stall" : "hid_transfer"); return;
            }
        }
    }
    if (h->repeat_key && usb_expired(host,h->repeat_at)) {
        unsigned usage=0;
        for (unsigned i=2;i<8;i++) if (code(h->previous[i])==h->repeat_key) { usage=h->previous[i]; break; }
        key(d,h,usage,h->repeat_key,2); h->repeat_at=host->io.now(host->io.cookie)+INPUT_REPEAT_INTERVAL_MS;
    }
}
