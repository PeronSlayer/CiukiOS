/* Native HID capabilities, calibration and transitions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/input.h>
#include <ciuki/sha256.h>

/* USB 2.0 9.6.1/9.6.7. The descriptor is never truncated or converted through
 * an ASCII display name. Bad advertised serials refuse durable identity. */
void usb_identity_serial(struct usb_device *d, unsigned index)
{
    d->serial_advertised=index!=0; d->serial_valid=false;
    memset(d->serial_identity,0,sizeof(d->serial_identity));
    if (!index) return;
    uint8_t b[256]={0};
    int n=usb_control(d,0x80,6,0x300,0,b,sizeof(b));
    if (n<4 || b[1]!=3 || b[0]!=n || (n&1)) goto failed;
    uint16_t language=(uint16_t)(b[2]|(unsigned)b[3]<<8);
    n=usb_control(d,0x80,6,(uint16_t)(0x300|index),language,b,sizeof(b));
    if (n<4 || n>254 || b[0]!=n || b[1]!=3 || (n&1)) goto failed;
    sha256(b+2,(size_t)n-2,d->serial_identity); d->serial_valid=true; return;
failed:
    klog("[joystick-settings] serial unavailable; identity persistence refused vid=%04x pid=%04x",d->vendor,d->product);
}

int usb_joystick_identity(uint32_t device, uint32_t generation, uint8_t out[32])
{
    if (!device || device>CIUKI_HID_DEVICES || !generation) return -ENODEV;
    unsigned slot=device-1;
    const struct usb_host *host=usb_get(slot/(USB_DEVICE_MAX*USB_HID_MAX));
    if (!host) return -ENODEV;
    const struct usb_device *d=&host->devices[(slot/USB_HID_MAX)%USB_DEVICE_MAX];
    const struct usb_hid *h=&d->hid[slot%USB_HID_MAX];
    if (!h->reports || !h->reports->caps.attached || h->reports->caps.generation!=generation ||
        !(h->reports->caps.classes&CIUKI_HID_JOYSTICK)) return -ENODEV;
    if (d->serial_advertised && !d->serial_valid) return -EAGAIN;
    uint8_t key[48]={ 'J','U','S','B',1,d->vendor,d->vendor>>8,d->product,d->product>>8,h->interface };
    if (d->serial_valid) {
        key[10]=1; memcpy(key+11,d->serial_identity,32);
    } else {
        key[11]=host->pci.bus; key[12]=host->pci.dev; key[13]=host->pci.fn;
        key[14]=d->root_port;
        /* Enumeration currently permits one external hub; retain a bounded
         * chain so device addresses/retained slot numbers never enter the key. */
        const struct usb_device *p=d; unsigned depth=0;
        while (p->parent) {
            if (depth==USB_DEVICE_MAX || p->parent>USB_DEVICE_MAX) return -EIO;
            if (15+depth>=sizeof(key)) return -EIO;
            key[15+depth++]=p->port+1; p=&host->devices[p->parent-1];
        }
        key[10]=(uint8_t)(2+depth);
    }
    sha256(key,sizeof(key),out); return 0;
}

static unsigned locator(struct usb_device *d, struct usb_hid *h, unsigned *host)
{
    unsigned hi=0, di=0, ii=0;
    for (unsigned i=0;i<usb_count();i++) if (usb_get(i)==d->host) hi=i;
    for (unsigned i=0;i<USB_DEVICE_MAX;i++) if (&d->host->devices[i]==d) di=i;
    for (unsigned i=0;i<USB_HID_MAX;i++) if (&d->hid[i]==h) ii=i;
    *host=hi; return 1+(hi*USB_DEVICE_MAX+di)*USB_HID_MAX+ii;
}
static void event(struct usb_device *d, struct usb_hid *h, unsigned type, unsigned code, int32_t value)
{
    input_hid_event(type,code,value,h->reports->caps.device,d->generation,
                    d->host->io.now(d->host->io.cookie));
}
static int32_t normalize(int32_t raw, const int32_t c[3])
{
    if (raw<=c[0]) return -32767;
    if (raw>=c[2]) return 32767;
    if (raw<c[1]) return (int32_t)(((int64_t)raw-c[1])*32767/((int64_t)c[1]-c[0]));
    return (int32_t)(((int64_t)raw-c[1])*32767/((int64_t)c[2]-c[1]));
}
int usb_hid_describe(struct usb_device *d, struct usb_hid *h)
{
    struct usb_hid_reports *p=h->reports;
    struct ciuki_hid_caps *c=&p->caps;
    c->version=CIUKI_HID_VERSION; c->size=sizeof(*c);
    c->device=locator(d,h,&c->host); c->generation=d->generation;
    c->classes=p->layout->classes; c->address=d->address; c->interface=h->interface;
    for (unsigned j=0;j<p->layout->fields;j++) {
        const struct hid_field *f=&p->layout->field[j];
        if (f->kind!=HID_INPUT) continue;
        bool joy=f->application==0x10004 || f->application==0x10005;
        if (joy && (!(f->flags&2) || (f->flags&4))) {
            /* Button selector arrays are valid; axes must be variables. */
            for (unsigned i=0;i<f->usages;i++) if ((p->layout->usage[f->first_usage+i].low>>16)!=9) return -USB_EPROTO;
        }
        unsigned hats=f->hat;
        for (unsigned s=0;s<f->usages;s++) {
            struct hid_usage u=p->layout->usage[f->first_usage+s];
            if ((u.low>>16)==7 && f->application==0x10006) {
                if ((u.high&65535)>255 || (f->flags&4) ||
                    ((f->flags&2) && (f->minimum!=0 || f->maximum!=1))) return -USB_EPROTO;
                for (unsigned k=u.low&65535;k<=(u.high&65535);k++) if (k>3) c->keyboard[k/8]|=1u<<(k%8);
            }
            for (unsigned a=1;a<=6;a++) {
                static const uint32_t usages[]={0,0xc00e9,0xc00ea,0xc00e2,0xc00cd,0xc00b5,0xc00b6};
                if (usages[a]>=u.low && usages[a]<=u.high) c->consumers|=1u<<(a-1);
            }
            if (joy && (u.low>>16)==9) {
                if ((u.high&65535)>32 ||
                    ((f->flags&2) && (f->minimum!=0 || f->maximum!=1))) return -USB_EPROTO;
                for (unsigned k=u.low&65535;k<=(u.high&65535);k++) if (k) c->buttons|=1u<<(k-1);
            }
        }
        for (unsigned i=0;i<f->count;i++) {
            uint32_t u=hid_usage_at(p->layout,f,i);
            unsigned axis=hid_axis_index(u);
            if (joy && axis<CIUKI_HID_AXES) {
                if (!(f->flags&2) || (int64_t)f->maximum-f->minimum<2 || (c->axes&(1u<<axis))) return -USB_EPROTO;
                c->axes|=1u<<axis;
                int32_t centre=(int32_t)((int64_t)f->minimum+((int64_t)f->maximum-f->minimum+1)/2);
                c->axis[axis]=(struct ciuki_hid_axis){.minimum=f->minimum,.centre=centre,.maximum=f->maximum,
                    .physical_minimum=f->physical_minimum,.physical_maximum=f->physical_maximum,
                    .unit=f->unit,.unit_exponent=f->unit_exponent};
                p->calibration[axis][0]=f->minimum; p->calibration[axis][1]=centre;
                p->calibration[axis][2]=f->maximum; p->raw[axis]=centre;
            }
            if (joy && u==0x10039) c->hats|=1u<<hats++;
        }
    }
    if (c->consumers) c->classes|=CIUKI_HID_CONSUMER;
    p->state=(struct ciuki_hid_state){.version=CIUKI_HID_VERSION,.size=sizeof(p->state),
        .device=c->device,.generation=c->generation};
    for (unsigned i=0;i<CIUKI_HID_HATS;i++) p->state.hats[i]=-1;
    return 0;
}
void usb_hid_publish(struct usb_device *d, struct usb_hid *h, const struct hid_values *v, unsigned report)
{
    struct usb_hid_reports *p=h->reports;
    struct ciuki_hid_state next=p->state;
    memcpy(p->keys[report],v->keyboard,32); p->consumers[report]=v->consumers;
    p->buttons[report]=v->buttons;
    memset(next.keyboard,0,32); next.consumers=next.buttons=0;
    for (unsigned r=0;r<p->layout->reports;r++) {
        for (unsigned k=0;k<32;k++) next.keyboard[k]|=p->keys[r][k];
        next.consumers|=p->consumers[r]; next.buttons|=p->buttons[r];
    }
    for (unsigned k=4;k<256;k++) if ((next.keyboard[k/8]^p->state.keyboard[k/8])&(1u<<(k%8)))
        event(d,h,INPUT_USAGE,k,!!(next.keyboard[k/8]&(1u<<(k%8))));
    for (unsigned a=0;a<6;a++) if ((next.consumers^p->state.consumers)&(1u<<a))
        event(d,h,INPUT_CONSUMER,a+1,!!(next.consumers&(1u<<a)));
    for (unsigned a=0;a<CIUKI_HID_AXES;a++) if (v->axes&(1u<<a)) {
        p->raw[a]=v->axis[a]; next.axes[a]=normalize(v->axis[a],p->calibration[a]);
        if (next.axes[a]!=p->state.axes[a]) event(d,h,INPUT_JOY_AXIS,a,next.axes[a]);
    }
    for (unsigned a=0;a<CIUKI_HID_HATS;a++) if (v->hats&(1u<<a)) {
        next.hats[a]=v->hat[a];
        if (next.hats[a]!=p->state.hats[a]) event(d,h,INPUT_JOY_HAT,a,next.hats[a]);
    }
    for (unsigned a=0;a<32;a++) if ((next.buttons^p->state.buttons)&(1u<<a))
        event(d,h,INPUT_JOY_BUTTON,a+1,!!(next.buttons&(1u<<a)));
    p->state=next;
}
void usb_hid_lifecycle(struct usb_device *d, struct usb_hid *h, bool attached)
{
    struct usb_hid_reports *p=h->reports;
    if (!p) return;
    if (!attached) {
        struct hid_values neutral={.axes=p->caps.axes,.hats=p->caps.hats};
        for (unsigned i=0;i<CIUKI_HID_AXES;i++) neutral.axis[i]=p->calibration[i][1];
        for (unsigned i=0;i<CIUKI_HID_HATS;i++) neutral.hat[i]=-1;
        for (unsigned r=0;r<p->layout->reports;r++) usb_hid_publish(d,h,&neutral,r);
    }
    p->caps.attached=p->state.attached=attached;
    event(d,h,INPUT_DEVICE,0,attached);
}
/* Caller holds the UP non-preemptible boundary. Records are retained after
 * detach, and no physical access, waiting or allocation occurs here. */
int usb_hid_query(uint32_t command, void *record)
{
    uint32_t *header=record, device=header[2], generation=header[3];
    if (!device || device>CIUKI_HID_DEVICES) return -ENODEV;
    unsigned slot=device-1, hi=slot/(USB_DEVICE_MAX*USB_HID_MAX);
    const struct usb_host *host=usb_get(hi);
    if (!host) return -ENODEV;
    const struct usb_device *d=&host->devices[(slot/USB_HID_MAX)%USB_DEVICE_MAX];
    const struct usb_hid *h=&d->hid[slot%USB_HID_MAX];
    struct usb_hid_reports *p=h->reports;
    /* Boot fallback keeps its legacy event stream and allocation footprint.
     * The versioned snapshot still recovers its raw usages after RESYNC. */
    if (!p && h->input_registered && h->protocol==1) {
        if (generation && generation!=d->generation) return -ENODEV;
        if (command==INPUT_HID_GET_CAPS) {
            struct ciuki_hid_caps c={.version=1,.size=sizeof(c),.device=device,
                .generation=d->generation,.attached=h->active,.classes=CIUKI_HID_KEYBOARD,
                .host=hi,.address=d->address,.interface=h->interface};
            memset(c.keyboard,255,sizeof(c.keyboard)); c.keyboard[0]&=0xf0;
            *(struct ciuki_hid_caps *)record=c; return 0;
        }
        if (command==INPUT_HID_GET_STATE && generation) {
            struct ciuki_hid_state s={.version=1,.size=sizeof(s),.device=device,
                .generation=d->generation,.attached=h->active};
            s.keyboard[28]=h->previous[0];
            for (unsigned i=2;i<8;i++) {
                unsigned key=h->previous[i]; if (key>3) s.keyboard[key/8]|=1u<<(key%8);
            }
            for (unsigned i=0;i<CIUKI_HID_HATS;i++) s.hats[i]=-1;
            *(struct ciuki_hid_state *)record=s; return 0;
        }
        return -EINVAL;
    }
    if (!p || !p->caps.device) return -ENODEV;
    if (generation && generation!=p->caps.generation) return -ENODEV;
    if (command==INPUT_HID_GET_CAPS) { *(struct ciuki_hid_caps *)record=p->caps; return 0; }
    if (!generation) return -EINVAL;
    if (command==INPUT_HID_GET_STATE) { *(struct ciuki_hid_state *)record=p->state; return 0; }
    if (command==INPUT_HID_CALIBRATE) {
        struct ciuki_hid_calibration *c=record;
        if (!p->caps.attached) return -ENODEV;
        if (c->axis>=CIUKI_HID_AXES || !(p->caps.axes&(1u<<c->axis))) return -EINVAL;
        struct ciuki_hid_axis *a=&p->caps.axis[c->axis];
        if (c->minimum<a->minimum || c->maximum>a->maximum || c->minimum>=c->centre || c->centre>=c->maximum) return -EINVAL;
        int32_t *cal=p->calibration[c->axis];
        cal[0]=c->minimum; cal[1]=c->centre; cal[2]=c->maximum;
        p->state.axes[c->axis]=normalize(p->raw[c->axis],cal);
        return 0;
    }
    return -ENOTTY;
}

/* Cached raw axes for the desktop's common calibration page, never USB I/O. */
int usb_hid_raw(uint32_t device, uint32_t generation, int32_t raw[4])
{
    if (!device || device>CIUKI_HID_DEVICES || !generation) return -ENODEV;
    unsigned slot=device-1, hi=slot/(USB_DEVICE_MAX*USB_HID_MAX);
    const struct usb_host *host=usb_get(hi);
    if (!host) return -ENODEV;
    const struct usb_device *d=&host->devices[(slot/USB_HID_MAX)%USB_DEVICE_MAX];
    const struct usb_hid_reports *p=d->hid[slot%USB_HID_MAX].reports;
    if (!p || !p->caps.attached || p->caps.generation!=generation) return -ENODEV;
    memcpy(raw,p->raw,4*sizeof(*raw)); return 0;
}
