/* Boot HID state diffing and the fixed wheel extension.
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
static void key(struct usb_device *d, uint16_t c, unsigned value)
{ if (c) input_usb_event(INPUT_KEY, c, (int32_t)value, d->generation, d->host->io.now(d->host->io.cookie)); }
/* Deliberately narrow descriptor recognizer: no IDs, long items, global
 * stacks, arrays, padding beyond the first byte, or unknown input fields. */
bool usb_mouse_wheel_layout(const uint8_t *b, unsigned len)
{
    unsigned page=0, size=0, count=0, usage[8]={0}, usages=0, min=0, max=0, bits=0, depth=0, button_bits=0;
    int32_t logical_min=0, logical_max=0;
    bool buttons=false, xy=false, wheel=false, mouse=false;
    for (unsigned at=0; at<len;) {
        unsigned item=b[at++]; if (item == 0xfe) return false;
        unsigned n=item & 3; if (n==3) n=4;
        if (n > len-at) return false;
        uint32_t v=0; for (unsigned i=0;i<n;i++) v |= (uint32_t)b[at+i] << (8*i);
        int32_t sv=(int32_t)v;
        if (n==1) sv=(int8_t)v; else if (n==2) sv=(int16_t)v;
        at+=n;
        unsigned tag=item & 0xfc;
        if (tag==0x04) page=v;
        else if (tag==0x14) logical_min=sv;
        else if (tag==0x24) logical_max=logical_min < 0 ? sv : (int32_t)v;
        else if (tag==0x74) size=v;
        else if (tag==0x94) count=v;
        else if (tag==0x84 || tag==0xa4 || tag==0xb4) return false;
        else if (tag==0x08) { if (usages==8) return false; usage[usages++]=v; }
        else if (tag==0x18) min=v;
        else if (tag==0x28) max=v;
        else if (tag==0x80) {
            if (!size || !count || size>8 || count>8 || size*count > 32-bits) return false;
            if (v & 1) {
                if (bits!=button_bits || !button_bits || size*count!=8-button_bits) return false;
            } else if (bits==0 && page==9 && size==1 && count>=3 && count<=5 && min==1 && max==count && (v&7)==2) { buttons=true; button_bits=count; }
            else if (bits==8 && page==1 && size==8 && count==2 && usages==2 && usage[0]==0x30 && usage[1]==0x31 && (v&7)==6 && logical_min==-127 && logical_max==127) xy=true;
            else if (bits==8 && page==1 && size==8 && count==3 && usages==3 && usage[0]==0x30 && usage[1]==0x31 && usage[2]==0x38 && (v&7)==6 && logical_min==-127 && logical_max==127) { xy=true; wheel=true; }
            else if (bits==24 && page==1 && size==8 && count==1 && usages==1 && usage[0]==0x38 && (v&7)==6 && logical_min==-127 && logical_max==127) wheel=true;
            else return false;
            bits+=size*count;
            usages=min=max=0;
        } else if (tag==0xa0) {
            if (depth==4) return false;
            if (!depth && page==1 && usages==1 && usage[0]==2 && v==1) mouse=true;
            depth++; usages=min=max=0;
        } else if (tag==0xc0) {
            if (!depth) return false;
            depth--; usages=min=max=0;
        } else if ((item & 0x0c)==0) { usages=min=max=0; }
    }
    return mouse && !depth && buttons && xy && wheel && bits==32;
}
int usb_hid_attach(struct usb_device *d, struct usb_hid *hid, unsigned report_len)
{
    if (hid->active) return -EINVAL;
    int err = usb_control(d, 0x21, 11, 0, hid->interface, 0, 0); if (err) return err;
    err = usb_control(d, 0x21, 10, 0, hid->interface, 0, 0);
    if (err && !(hid->protocol == 2 && err == -USB_EPIPE)) return err;
    if (hid->protocol == 2 && report_len && report_len <= 256 && hid->packet >= 4) {
        uint8_t b[256];
        err = usb_control(d, 0x81, 6, 0x2200, hid->interface, b, report_len);
        if (err == (int)report_len && usb_mouse_wheel_layout(b, report_len)) {
            err = usb_control(d, 0x21, 11, 1, hid->interface, 0, 0); if (err) return err;
            hid->wheel = true;
        } else if (err < 0 && err != -USB_EPIPE) return err;
    }
    hid->leds = 0xff;
    if (hid->protocol == 1) {
        err = usb_control(d, 0x21, 9, 0x200, hid->interface, &leds, 1); if (err != 1) return err < 0 ? err : -USB_EPROTO;
        hid->leds = leds;
    }
    hid->active = true;
    input_devices_changed(INPUT_USB, true);
    klog("[usb] hid %s attached addr=%u interface=%u wheel=%u", hid->protocol == 1 ? "keyboard" : "mouse", d->address, hid->interface, hid->wheel);
    return 0;
}
int usb_hid_report(struct usb_device *d, struct usb_hid *h, const uint8_t *b, unsigned len)
{
    if (!h->active || !b) return -EINVAL;
    if (h->protocol == 1) {
        if (len != 8 || b[1]) return -USB_EPROTO;
        for (unsigned i=2;i<8;i++) if (b[i] && b[i]<=3) return 0; /* rollover: retain last valid state */
        for (unsigned i=0;i<8;i++) if ((b[0]^h->previous[0]) & (1u<<i)) key(d, modifiers[i], !!(b[0] & (1u<<i)));
        for (unsigned i=2;i<8;i++) {
            uint16_t c=code(h->previous[i]);
            if (c && first(h->previous,i) && !has(b,c)) key(d,c,0);
        }
        if (h->repeat_key && !has(b,h->repeat_key)) h->repeat_key=0;
        for (unsigned i=2;i<8;i++) {
            uint16_t c=code(b[i]);
            if (c && first(b,i) && !has(h->previous,c)) {
                key(d,c,1);
                if (c==0x3a) leds^=2; else if (c==0x45) leds^=1; else if (c==0x46) leds^=4;
                else if (c!=INPUT_KEY_PAUSE) { h->repeat_key=c; h->repeat_at=d->host->io.now(d->host->io.cookie)+INPUT_REPEAT_DELAY_MS; }
            }
        }
        memcpy(h->previous,b,8);
    } else {
        if (len < (h->wheel ? 4u : 3u)) return -USB_EPROTO;
        unsigned changed=(b[0]^h->previous[0]) & 7;
        for (unsigned i=0;i<3;i++) if (changed & (1u<<i)) input_usb_event(INPUT_BTN,(uint16_t)i,!!(b[0] & (1u<<i)),d->generation,d->host->io.now(d->host->io.cookie));
        input_usb_event(INPUT_REL,INPUT_X,(int8_t)b[1],d->generation,d->host->io.now(d->host->io.cookie));
        input_usb_event(INPUT_REL,INPUT_Y,(int8_t)b[2],d->generation,d->host->io.now(d->host->io.cookie));
        if (h->wheel) input_usb_event(INPUT_REL,INPUT_WHEEL,(int8_t)b[3],d->generation,d->host->io.now(d->host->io.cookie));
        h->previous[0]=b[0];
    }
    return 0;
}
void usb_hid_release(struct usb_device *d, struct usb_hid *h)
{
    if (!h->active) return;
    if (h->protocol==1) {
        for (unsigned i=0;i<8;i++) if (h->previous[0] & (1u<<i)) key(d,modifiers[i],0);
        for (unsigned i=2;i<8;i++) if (first(h->previous,i)) key(d,code(h->previous[i]),0);
    } else for (unsigned i=0;i<3;i++) if (h->previous[0] & (1u<<i)) input_usb_event(INPUT_BTN,(uint16_t)i,0,d->generation,d->host->io.now(d->host->io.cookie));
    memset(h->previous,0,8); h->active=false; h->repeat_key=0;
    input_devices_changed(INPUT_USB, false);
}
void usb_hid_poll(struct usb_device *d, struct usb_hid *h)
{
    if (!h->active) return;
    struct usb_host *host=d->host;
    if (h->protocol==1 && h->leds!=leds) {
        int err=usb_control(d,0x21,9,0x200,h->interface,&leds,1);
        if (err!=1) { usb_device_quarantine(d,"hid_led_output"); return; }
        h->leds=leds;
    }
    if (usb_expired(host,h->next_poll)) {
        h->next_poll=host->io.now(host->io.cookie)+h->interval;
        uint8_t b[64]; int n=host->ops->packet(d,h->endpoint,USB_PID_IN,h->toggle,b,h->packet,true);
        if (n>=0) {
            h->toggle^=1;
            if (usb_hid_report(d,h,b,(unsigned)n)) { usb_device_quarantine(d,"hid_report_length"); return; }
        } else if (n!=-USB_EAGAIN) { usb_device_quarantine(d,n==-USB_EPIPE ? "hid_stall" : "hid_transfer"); return; }
    }
    if (h->repeat_key && usb_expired(host,h->repeat_at)) {
        key(d,h->repeat_key,2); h->repeat_at=host->io.now(host->io.cookie)+INPUT_REPEAT_INTERVAL_MS;
    }
}
