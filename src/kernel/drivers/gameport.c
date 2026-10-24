#include <ciuki/system_settings.h>
/* Independent implementation of analog RC sampling. Behavioral references:
 * Linux drivers/input/{gameport/ns558.c,gameport/gameport.c,joystick/analog.c},
 * and gameport/emu10k1-gp.c; see peripheral-driver-coverage.md. No donor code.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/win32.h>
#include <ciuki/input.h>
#include <ciuki/joystick.h>
#include <ciuki/sha256.h>
#include <ciuki/joystick_settings.h>
/* Reduced standalone harnesses may omit the storage service. */
#pragma weak joystick_settings_calibrate
#pragma weak joystick_settings_route
#pragma weak joystick_settings_mode
#pragma weak joystick_settings_status
#pragma weak joystick_settings_game
#pragma weak joystick_settings_get_game
#pragma weak joystick_settings_service
#pragma weak joystick_storage_start
#define GAMEPORTS 4u
#define RC_TIMEOUT 3580u /* 3 ms at 1193182 Hz */
#define RC_GAP 120u     /* 100 us maximum uncertainty */
struct gameport {
    struct gameport_io io;
    struct pci_func pci;
    uint16_t base;
    int claim;
    gen_t lease;
    unsigned mode;
    bool used;
    unsigned timing_failures;
    uint64_t next_sample, next_discovery;
    const char *reason;
    struct ciuki_hid_caps caps[2];
    struct ciuki_hid_state state[2];
    int32_t raw[4], calibration[4][3];
};
static struct gameport ports[GAMEPORTS];
static struct joystick_route default_route;
static uint32_t boot_session;
extern int entropy_get(void *, size_t) __attribute__((weak));
uint32_t joystick_session(void) { return boot_session; }
extern int usb_hid_query(uint32_t, void *) __attribute__((weak));
extern int usb_hid_raw(uint32_t, uint32_t, int32_t[4]) __attribute__((weak));
extern int usb_joystick_identity(uint32_t, uint32_t, uint8_t[32]) __attribute__((weak));

int joystick_identity(uint32_t device, uint32_t generation, uint8_t out[32])
{
    if (device<CIUKI_GAMEPORT_FIRST)
        return usb_joystick_identity ? usb_joystick_identity(device,generation,out) : -ENODEV;
    if (device>CIUKI_JOYSTICK_LAST || !generation) return -ENODEV;
    unsigned slot=device-CIUKI_GAMEPORT_FIRST;
    struct gameport *p=&ports[slot/2];
    if (!p->used || p->caps[slot&1].generation!=generation) return -ENODEV;
    const uint8_t key[]={ 'J','P','O','R','T',1,p->base,p->base>>8,slot&1 };
    sha256(key,sizeof(key),out); return 0;
}

static int measure_axes(const struct gameport_io *io, uint16_t base, unsigned requested, struct gameport_sample *out)
{
    if (!io || !io->read || !io->trigger || !io->clock || !out) return -EINVAL;
    struct gameport_sample s={0};
    for (unsigned a=0;a<4;a++) s.clocks[a]=-1;
    uint64_t begin=io->clock(io->context), previous=begin;
    io->trigger(io->context,base);
    uint8_t pending=io->read(io->context,base)&requested&15;
    bool uncertain=false;
    /* A second iteration bound covers a broken/nonadvancing clock adapter. */
    for (unsigned n=0;n<16384;n++) {
        uint8_t b=io->read(io->context,base);
        uint64_t now=io->clock(io->context);
        if (now<previous) return -EIO;
        uint64_t elapsed=now-begin;
        if (now-previous>RC_GAP) uncertain=true;
        if (elapsed>RC_TIMEOUT) { *out=s; return uncertain ? -EAGAIN : 0; }
        unsigned fallen=pending&~b&15;
        if (fallen && now-previous>RC_GAP) uncertain=true;
        for (unsigned a=0;a<4;a++) if (fallen&(1u<<a)) {
            if (elapsed && now-previous<=RC_GAP) { s.axes|=1u<<a; s.clocks[a]=(int32_t)elapsed; }
        }
        pending&=b;
        s.buttons=(uint8_t)(~(unsigned)b>>4)&15;
        previous=now;
        if (!pending) { *out=s; return uncertain ? -EAGAIN : 0; }
    }
    return -ETIMEDOUT;
}
int gameport_measure(const struct gameport_io *io, uint16_t base, struct gameport_sample *out)
{ return measure_axes(io,base,15,out); }
static int32_t normalized(int32_t raw, const int32_t c[3])
{
    if (raw<=c[0]) return -32767;
    if (raw>=c[2]) return 32767;
    if (raw<c[1]) return (int32_t)(((int64_t)raw-c[1])*32767/(c[1]-c[0]));
    return (int32_t)(((int64_t)raw-c[1])*32767/(c[2]-c[1]));
}
static void emit(struct gameport *p, unsigned stick, unsigned type, unsigned code, int32_t value)
{
    input_hid_event(type,code,value,p->caps[stick].device,p->caps[stick].generation,g_ticks);
}
static void publish(struct gameport *p, const struct gameport_sample *s)
{
    for (unsigned stick=0;stick<2;stick++) {
        struct ciuki_hid_caps *c=&p->caps[stick];
        struct ciuki_hid_state *v=&p->state[stick];
        unsigned axes=p->mode ? (s->axes>>(stick*2))&3 : stick ? 0 : s->axes;
        bool changed=c->axes!=axes;
        if (changed && c->attached) {
            for (unsigned a=0;a<4;a++) if (v->axes[a]) emit(p,stick,INPUT_JOY_AXIS,a,0);
            for (unsigned b=0;b<4;b++) if (v->buttons&(1u<<b)) emit(p,stick,INPUT_JOY_BUTTON,b+1,0);
            emit(p,stick,INPUT_DEVICE,0,0);
        }
        if (changed) { c->generation++; if (!c->generation) c->generation=1; }
        c->axes=axes; c->attached=axes!=0;
        c->buttons=p->mode ? 3 : 15;
        v->generation=c->generation; v->attached=c->attached;
        unsigned buttons=c->attached ? (p->mode ? (s->buttons>>(stick*2))&3 : s->buttons) : 0;
        for (unsigned a=0;a<4;a++) {
            unsigned physical=p->mode ? stick*2+a : a;
            int32_t value=axes&(1u<<a) ? normalized(s->clocks[physical],p->calibration[physical]) : 0;
            if (changed || v->axes[a]!=value) emit(p,stick,INPUT_JOY_AXIS,a,value);
            v->axes[a]=value;
        }
        for (unsigned b=0;b<4;b++) if (changed || ((v->buttons^buttons)&(1u<<b)))
            emit(p,stick,INPUT_JOY_BUTTON,b+1,!!(buttons&(1u<<b)));
        v->buttons=buttons;
        if (changed && c->attached) emit(p,stick,INPUT_DEVICE,0,1);
    }
    memcpy(p->raw,s->clocks,sizeof(p->raw));
}
int gameport_pci_base(const struct pci_func *p, uint16_t *base)
{
    if (!p || !base) return -EINVAL;
    bool creative=p->vendor==0x1102 && p->device>=0x7002 && p->device<=0x7005;
    bool analog=p->class_code==9 && p->subclass==4 && p->prog_if==0x10;
    if (!creative && !analog) return -ENODEV;
    if (!(p->command&1)) return -ENODEV;
    for (unsigned i=0;i<(creative ? 1u : 6u);i++) {
        uint32_t b=p->bar[i]&~3u;
        if ((p->bar[i]&1) && b && b<=0xfff8) { *base=(uint16_t)b; return 0; }
    }
    return -ENODEV;
}
int gameport_admit(uint16_t base, const struct pci_func *pci, const struct gameport_io *io)
{
    if (!base || !io || !io->read || !io->trigger || !io->clock) return -EINVAL;
    unsigned slot;
    for (slot=0;slot<GAMEPORTS;slot++) {
        if (ports[slot].used && ports[slot].base==base) return -EBUSY;
        if (!ports[slot].used) break;
    }
    if (slot==GAMEPORTS) return -ENOSPC;
    struct gameport *p=&ports[slot];
    if (!system_admit(0x72000u + slot, "gameport")) {
        p->used = true; p->base = base; p->claim = -1; if (pci) p->pci = *pci;
        p->reason = "Disabled by hardware profile; no game port probe."; return (int)slot;
    }
    /* Only the analog register is accessed. Never MIDI/audio registers. */
    int h=registry_claim(RES_PORT,base,(uint32_t)base+1,"gameport",false);
    if (h<0) return h;
    const struct resource *r=registry_get((unsigned)h);
    if (!r) return -EIO;
    p->claim=h; p->lease=r->generation;
    if (registry_activate(h,p->lease)) { registry_release(h,p->lease); return -EBUSY; }
    p->io=*io; p->base=base; p->used=true;
    if (pci) p->pci=*pci;
    p->reason="Analog port claimed; no connected axes detected.";
    for (unsigned a=0;a<4;a++) {
        p->calibration[a][0]=1; p->calibration[a][1]=716; p->calibration[a][2]=RC_TIMEOUT;
    }
    for (unsigned j=0;j<2;j++) {
        p->caps[j]=(struct ciuki_hid_caps){.version=1,.size=sizeof(p->caps[j]),
            .device=CIUKI_GAMEPORT_FIRST+slot*2+j,.generation=1,.classes=CIUKI_HID_JOYSTICK,
            .host=UINT32_MAX,.address=base,.interface=j};
        p->state[j]=(struct ciuki_hid_state){.version=1,.size=sizeof(p->state[j]),
            .device=p->caps[j].device,.generation=1};
        for (unsigned a=0;a<4;a++) p->caps[j].axis[a]=(struct ciuki_hid_axis){.minimum=1,.centre=716,.maximum=RC_TIMEOUT};
        for (unsigned a=0;a<4;a++) p->state[j].hats[a]=-1;
    }
    return 0;
}
void gameport_poll(void)
{
    /* Thread context before the timing loop. No RNG or mutex in a query,
     * guest I/O, IRQ exclusion or calibration. Zero remains explicitly
     * unavailable when the existing entropy service is not ready. */
    if (!boot_session && entropy_get) {
        uint32_t session=0;
        if (!entropy_get(&session,sizeof(session)) && session) boot_session=session;
    }
    for (unsigned i=0;i<GAMEPORTS;i++) {
        struct gameport *p=&ports[i];
        if (!p->used) continue;
        struct gameport_sample s={0};
        if (!registry_valid(p->claim,p->lease)) {
            p->reason="Analog resource lease invalid; sampling stopped.";
            uint32_t flags=irq_save(); publish(p,&s); irq_restore(flags); continue;
        }
        if (g_ticks<p->next_sample) continue;
        unsigned known=p->mode ? p->caps[0].axes|(p->caps[1].axes<<2) : p->caps[0].axes;
        unsigned requested=known;
        if (!known || g_ticks>=p->next_discovery) { requested=15; p->next_discovery=g_ticks+1000; }
        int error=measure_axes(&p->io,p->base,requested,&s);
        p->next_sample=g_ticks+(known ? 20 : 1000);
        if (error) {
            p->reason="Analog timing uncertain; sample discarded.";
            if (++p->timing_failures>=3) { memset(&s,0,sizeof(s)); uint32_t flags=irq_save(); publish(p,&s); irq_restore(flags); }
            continue;
        }
        p->timing_failures=0;
        p->reason=s.axes ? "Native analog joystick; DOS uses a logical subscription." : "No connected analog axes; buttons released.";
        uint32_t flags=irq_save(); publish(p,&s); irq_restore(flags);
        kpreempt_point();
    }
}
int gameport_restore_mode(uint32_t device, uint32_t generation, unsigned mode)
{
    if (device<CIUKI_GAMEPORT_FIRST || device>CIUKI_JOYSTICK_LAST || mode>1) return -EINVAL;
    struct gameport *p=&ports[(device-CIUKI_GAMEPORT_FIRST)/2];
    if (!generation || p->caps[(device-CIUKI_GAMEPORT_FIRST)&1].generation!=generation) return -ENODEV;
    if (p->mode!=mode) {
        struct gameport_sample empty={0}; publish(p,&empty); p->mode=mode;
        p->next_sample=p->next_discovery=0;
        for (unsigned j=0;j<2;j++) { p->caps[j].generation++; p->state[j].generation=p->caps[j].generation; }
    }
    return 0;
}
int gameport_hid_query(uint32_t command, void *record)
{
    uint32_t *h=record;
    if (h[2]<CIUKI_GAMEPORT_FIRST || h[2]>CIUKI_JOYSTICK_LAST) return -ENODEV;
    unsigned slot=h[2]-CIUKI_GAMEPORT_FIRST, j=slot&1;
    struct gameport *p=&ports[slot/2];
    if (!p->used || (h[3] && h[3]!=p->caps[j].generation)) return -ENODEV;
    if (command==INPUT_HID_GET_CAPS) { *(struct ciuki_hid_caps *)record=p->caps[j]; return 0; }
    if (!h[3]) return -EINVAL;
    if (command==INPUT_HID_GET_STATE) { *(struct ciuki_hid_state *)record=p->state[j]; return 0; }
    if (command!=INPUT_HID_CALIBRATE) return -ENOTTY;
    struct ciuki_hid_calibration *c=record;
    if (!p->caps[j].attached) return -ENODEV;
    if (c->axis>=4 || !(p->caps[j].axes&(1u<<c->axis)) || c->minimum<1 || c->maximum>(int32_t)RC_TIMEOUT ||
        c->minimum>=c->centre || c->centre>=c->maximum) return -EINVAL;
    unsigned a=p->mode ? j*2+c->axis : c->axis;
    p->calibration[a][0]=c->minimum; p->calibration[a][1]=c->centre; p->calibration[a][2]=c->maximum;
    p->state[j].axes[c->axis]=normalized(p->raw[a],p->calibration[a]);
    emit(p,j,INPUT_JOY_AXIS,c->axis,p->state[j].axes[c->axis]);
    return 0;
}
int joystick_hid_query(uint32_t command, void *record)
{
    uint32_t *h=record;
    return h[2]>=CIUKI_GAMEPORT_FIRST ? gameport_hid_query(command,record) :
        usb_hid_query ? usb_hid_query(command,record) : -ENODEV;
}
static int caps(uint32_t device, uint32_t generation, struct ciuki_hid_caps *c)
{
    *c=(struct ciuki_hid_caps){.version=1,.size=sizeof(*c),.device=device,.generation=generation};
    int e=joystick_hid_query(INPUT_HID_GET_CAPS,c);
    return e ? e : !(c->classes&CIUKI_HID_JOYSTICK) || !c->attached ? -ENODEV : 0;
}
int joystick_route_validate(const struct joystick_route *r)
{
    if (!r || r->mode>CIUKI_JOY_TWO_STICKS) return -EINVAL;
    if (!r->device) return r->generation || r->second_device || r->second_generation || r->mode ? -EINVAL : 0;
    struct ciuki_hid_caps c;
    if (!r->generation || caps(r->device,r->generation,&c)) return -ENODEV;
    if (r->second_device) {
        if (!r->mode || !r->second_generation || caps(r->second_device,r->second_generation,&c)) return -ENODEV;
    } else if (r->second_generation) return -EINVAL;
    return 0;
}
void joystick_default_route(struct joystick_route *r)
{
    uint32_t flags=irq_save();
    *r=default_route;
    if (joystick_settings_game) (void)joystick_settings_game(0,r);
    irq_restore(flags);
}
void joystick_snapshot(const struct joystick_route *r, int32_t axes[4], uint8_t *mask, uint8_t *buttons)
{
    memset(axes,0,4*sizeof(*axes)); *mask=*buttons=0;
    for (unsigned j=0;j<(r->mode ? 2u : 1u);j++) {
        uint32_t device=j ? r->second_device : r->device;
        uint32_t generation=j ? r->second_generation : r->generation;
        struct ciuki_hid_caps c;
        if (!generation || caps(device,generation,&c)) continue;
        struct ciuki_hid_state s={.version=1,.size=sizeof(s),.device=device,.generation=generation};
        if (joystick_hid_query(INPUT_HID_GET_STATE,&s) || !s.attached) continue;
        unsigned n=r->mode ? 2 : 4;
        for (unsigned a=0;a<n;a++) if (c.axes&(1u<<a)) { axes[j*2+a]=s.axes[a]; *mask|=1u<<(j*2+a); }
        *buttons|=r->mode ? (s.buttons&3)<<(j*2) : s.buttons&15;
    }
}
int joystick_control(struct ciuki_joystick_control *c)
{
    struct ciuki_joystick_control valid={.version=CIUKI_JOYSTICK_VERSION,.size=sizeof(*c),
        .device=c->device,.generation=c->generation,.operation=c->operation};
    if (c->operation==CIUKI_JOY_CALIBRATE) {
        valid.axis=c->axis; valid.minimum=c->minimum; valid.centre=c->centre; valid.maximum=c->maximum;
    } else if (c->operation==CIUKI_JOY_ROUTE || c->operation==CIUKI_JOY_SAVE_GAME) {
        valid.session=c->session; valid.mode=c->mode; valid.second_device=c->second_device; valid.second_generation=c->second_generation;
        if (c->operation==CIUKI_JOY_SAVE_GAME) valid.axis=c->axis;
    } else if (c->operation==CIUKI_JOY_GET_GAME) valid.axis=c->axis;
    else if (c->operation==CIUKI_JOY_CONFIGURE) valid.mode=c->mode;
    else if (c->operation>CIUKI_JOY_GET_GAME) return -EINVAL;
    if (memcmp(&valid,c,sizeof(valid))) return -EINVAL;
    if (c->operation==CIUKI_JOY_GET_GAME) {
        if (c->device || c->generation) return -EINVAL;
        return joystick_settings_get_game ? joystick_settings_get_game(c) : -ENODEV;
    }
    if (c->operation==CIUKI_JOY_SAVE_STATUS) {
        if (c->device || c->generation) return -EINVAL;
        if (!joystick_settings_status) return -ENODEV;
        joystick_settings_status(c); return 0;
    }
    if (c->operation==CIUKI_JOY_SAVE_GAME) {
        if (!c->axis || c->axis>JOY_SETTINGS_GAMES) return -EINVAL;
        return joystick_settings_route ? joystick_settings_route(c->axis,c) : -ENODEV;
    }
    if (c->operation==CIUKI_JOY_GET_ROUTE) {
        if (c->device || c->generation) return -EINVAL;
        struct joystick_route r; joystick_default_route(&r);
        c->device=r.device; c->generation=r.generation;
        c->second_device=r.second_device; c->second_generation=r.second_generation;
        c->mode=r.mode; c->session=boot_session; return 0;
    }
    if (c->operation==CIUKI_JOY_ROUTE) {
        if (c->session && c->session!=boot_session) return -ENODEV;
        struct joystick_route r={c->device,c->generation,c->second_device,c->second_generation,c->mode};
        int e=joystick_route_validate(&r);
        if (!e && joystick_settings_route) e=joystick_settings_route(0,c);
        if (!e) default_route=r; return e;
    }
    struct ciuki_hid_caps cap;
    if (c->operation==CIUKI_JOY_NEXT) {
        if (c->generation || c->device>CIUKI_JOYSTICK_LAST) return -EINVAL;
        uint32_t d=c->device+1;
        for (;d<=CIUKI_JOYSTICK_LAST;d++) if (!caps(d,0,&cap)) break;
        if (d>CIUKI_JOYSTICK_LAST) return -ENODEV;
        c->device=d; c->generation=cap.generation;
    } else {
        int e=caps(c->device,c->generation,&cap); if (e) return e;
    }
    if (c->operation==CIUKI_JOY_CONFIGURE) {
        if (!c->generation || c->device<CIUKI_GAMEPORT_FIRST || c->mode>1) return -EINVAL;
        return joystick_settings_mode ? joystick_settings_mode(c->device,c->generation,c->mode) :
            gameport_restore_mode(c->device,c->generation,c->mode);
    }
    if (c->operation==CIUKI_JOY_CALIBRATE) {
        if (joystick_settings_calibrate) return joystick_settings_calibrate(c);
        struct ciuki_hid_calibration cal={.version=1,.size=sizeof(cal),.device=c->device,
            .generation=c->generation,.axis=c->axis,.minimum=c->minimum,.centre=c->centre,.maximum=c->maximum};
        return joystick_hid_query(INPUT_HID_CALIBRATE,&cal);
    }
    if (c->operation==CIUKI_JOY_QUERY && !c->generation) c->generation=cap.generation;
    struct ciuki_hid_state s={.version=1,.size=sizeof(s),.device=c->device,.generation=c->generation};
    int e=joystick_hid_query(INPUT_HID_GET_STATE,&s); if (e) return e;
    c->session=boot_session;
    c->axes=cap.axes; c->buttons=s.buttons; c->attached=s.attached;
    memcpy(c->position,s.axes,sizeof(c->position));
    if (c->device>=CIUKI_GAMEPORT_FIRST) {
        unsigned slot=c->device-CIUKI_GAMEPORT_FIRST;
        struct gameport *p=&ports[slot/2]; c->mode=p->mode;
        for (unsigned a=0;a<4;a++) c->raw[a]=(c->axes&(1u<<a)) ? p->raw[p->mode ? (slot&1)*2+a : a] : 0;
    } else if (usb_hid_raw) (void)usb_hid_raw(c->device,c->generation,c->raw);
    return 0;
}
bool gameport_inventory(unsigned i, uint16_t *base, const struct pci_func **pci, bool *ready, const char **reason)
{
    if (i>=GAMEPORTS || !ports[i].used) return false;
    struct gameport *p=&ports[i]; *base=p->base; *pci=p->pci.vendor ? &p->pci : 0;
    *ready=p->caps[0].attached || p->caps[1].attached; *reason=p->reason; return true;
}
static uint8_t native_read(void *arg, uint16_t p) { (void)arg; return inb(p); }
static void native_trigger(void *arg, uint16_t p) { (void)arg; outb(p,255); }
static uint64_t native_clock(void *arg) { (void)arg; return arch_w32_counter(); }
static void worker(void *arg)
{
    (void)arg;
    const struct gameport_io io={native_read,native_trigger,native_clock,0};
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i); uint16_t base;
        if (!gameport_pci_base(p,&base)) (void)gameport_admit(base,p,&io);
    }
    (void)gameport_admit(0x201,0,&io);
    for (;;) { gameport_poll(); task_sleep_ms(20); }
}
static void settings_worker(void *arg)
{
    (void)arg;
    joystick_storage_start();
    for (;;) { joystick_settings_service(); task_sleep_ms(250); }
}
void gameport_init(void)
{
    if (!task_create_kernel("gameport",worker,0,P_NORMAL)) klog("[gameport] sampling worker unavailable");
    /* Disk admission/completion must never stall the 20 ms RC sampler. */
    if (joystick_storage_start && joystick_settings_service &&
        !task_create_kernel("joystick-settings",settings_worker,0,P_NORMAL))
        klog("[joystick-settings] storage worker unavailable; live input remains available");
}
