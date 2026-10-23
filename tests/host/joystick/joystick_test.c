/* Production analog + HID cache + virtual RC + BIOS; no physical I/O.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../desktop/replay_base.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ciuki/joystick.h>
#include <ciuki/input.h>
#include <ciuki/usb.h>
#include "../../../apps/dosbox/joystick_settings.h"
#include "../../../src/kernel/vm/dosvm_internal.h"
unsigned joystick_irq_depth;
volatile uint64_t g_ticks;
uint64_t arch_w32_counter(void) { assert(!"physical clock access"); return 0; }
unsigned settings_logs;
void klog(const char *format, ...) { if (!strncmp(format,"[joystick-settings]",19)) settings_logs++; }
static bool entropy_ready;
int entropy_get(void *out,size_t bytes) { assert(!joystick_irq_depth && bytes==4);if (!entropy_ready) return -EAGAIN;*(uint32_t *)out=123456;return 0; }
static unsigned assertions;
#define CHECK(c) do { assertions++; if (!(c)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#c); abort(); } } while (0)
static struct resource claims[4];
static unsigned claim_count, triggers, events, samples;
static bool conflict, valid=true;
int registry_claim(enum res_type t,uint32_t start,uint32_t end,const char *owner,bool share)
{
    CHECK(t==RES_PORT && end==start+1 && !strcmp(owner,"gameport") && !share);
    if (conflict) return -EBUSY;
    unsigned i=claim_count++; CHECK(i<4);
    claims[i]=(struct resource){.type=t,.start=start,.end=end,.owner=owner,.generation=i+1,.state=RS_CLAIMED};
    return (int)i;
}
const struct resource *registry_get(unsigned i) { CHECK(i<claim_count); return &claims[i]; }
int registry_activate(int h,gen_t g) { CHECK(g==claims[h].generation); claims[h].state=RS_ACTIVE; return 0; }
int registry_release(int h,gen_t g) { (void)h; (void)g; return 0; }
bool registry_valid(int h,gen_t g) { return valid && claims[h].generation==g && claims[h].state==RS_ACTIVE; }
void kpreempt_point(void) { CHECK(!joystick_irq_depth); }
void input_hid_event(uint16_t type,uint16_t code,int32_t value,uint32_t device,gen_t generation,uint64_t tick)
{
    (void)code;(void)value;(void)tick;
    CHECK(type>=INPUT_JOY_AXIS && type<=INPUT_DEVICE && device>=1 && device<=264 && generation);
    events++;
}
static struct usb_host usb;
static struct usb_hid_reports reports;
static struct hid_layout layout={.reports=1};
static uint64_t usb_now(void *arg) { (void)arg; return 100; }
unsigned usb_count(void) { return 1; }
const struct usb_host *usb_get(unsigned h) { return h ? 0 : &usb; }
struct rc_model {
    uint64_t clock, trigger;
    int32_t duration[4];
    uint8_t buttons;
    unsigned jitter, gap_at, gap, reads;
    bool frozen, absent;
};
static uint8_t model_read(void *context,uint16_t base)
{
    struct rc_model *m=context; CHECK(!joystick_irq_depth && (base==0x201 || base==0x300));
    m->reads++; uint8_t value=(uint8_t)(~(unsigned)m->buttons<<4)&0xf0;
    if (m->absent) return 0xff;
    for (unsigned a=0;a<4;a++) if (m->duration[a]<0 || m->clock-m->trigger<(unsigned)m->duration[a]) value|=1u<<a;
    return value;
}
static void model_trigger(void *context,uint16_t base)
{
    struct rc_model *m=context; CHECK(!joystick_irq_depth);
    CHECK(base==0x201 || base==0x300);
    if (claim_count) CHECK(claims[0].state==RS_ACTIVE);
    m->trigger=m->clock; m->reads=0; triggers++;
}
static uint64_t model_clock(void *context)
{
    struct rc_model *m=context;
    if (!m->frozen) m->clock+=3+(m->jitter ? (m->reads*13)%m->jitter : 0);
    if (m->gap_at && m->reads>=m->gap_at) { m->clock+=m->gap; m->gap_at=0; }
    return m->clock;
}
static struct ciuki_joystick_control request(unsigned op,unsigned device,unsigned generation)
{ return (struct ciuki_joystick_control){.version=1,.size=100,.device=device,.generation=generation,.operation=op}; }
static struct ciuki_joystick_control query(unsigned device)
{
    struct ciuki_joystick_control c=request(CIUKI_JOY_QUERY,device,0);
    CHECK(!joystick_control(&c)); return c;
}
static void settings(void)
{
    struct ciuki_joystick_control c=request(CIUKI_JOY_ROUTE,257,42);
    CHECK(dosbox_joystick_save(1,&c)==-EAGAIN);
    c.second_device=1;c.second_generation=77;c.mode=1;c.session=123456;
    CHECK(!dosbox_joystick_save(1,&c));
    struct ciuki_dosvm_command command={0};command.launch.reserved[0]=1;
    dosbox_joystick_load(1,&command);
    CHECK(command.launch.reserved[0]==1 && command.launch.reserved[1]==257 && command.launch.reserved[2]==42 &&
        command.launch.reserved[3]==1 && command.launch.reserved[4]==77 && command.launch.reserved[5]==1 && command.launch.reserved[6]==123456);
    char path[256];CHECK(!dosbox_joystick_path(1,path,sizeof(path)));
    const char *bad[]={"257 42 1 77 1", "257 42 1 77 2 123456\n", "99999999999999999999999999999 42 1 77 1 123456\n", "0257 42 1 77 1 123456\n", "257 42 1 77 1 123456\ntrailing", "265 42 1 77 1 123456\n", "257 42 265 77 1 123456\n"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(bad[i],1,strlen(bad[i]),f)==strlen(bad[i]));CHECK(!fclose(f));
        memset(&command,0,sizeof(command));dosbox_joystick_load(1,&command);CHECK(!command.launch.reserved[1]);
    }
    c=request(CIUKI_JOY_ROUTE,CIUKI_JOY_DISABLED,0);CHECK(!dosbox_joystick_save(1,&c));
    dosbox_joystick_load(1,&command);CHECK(command.launch.reserved[1]==CIUKI_JOY_DISABLED);
    c=request(CIUKI_JOY_ROUTE,0,0);CHECK(!dosbox_joystick_save(1,&c));
    dosbox_joystick_load(1,&command);CHECK(!command.launch.reserved[1]);
    CHECK(!unlink(path));CHECK(dosbox_joystick_path(0,path,sizeof(path))<0);
    puts("PASS per-game independent joystick preferences, default/off, exact parsing, overflow and malformed refusal");
}
static void analog(void)
{
    struct rc_model m={.duration={100,500,900,1300},.buttons=5,.jitter=8};
    struct gameport_io io={model_read,model_trigger,model_clock,&m};
    struct gameport_sample sample;
    for (unsigned i=1;i<20;i++) {
        m.jitter=i;
        CHECK(!gameport_measure(&io,0x201,&sample) && sample.axes==15 && sample.buttons==5);
        for (unsigned a=0;a<4;a++) CHECK(sample.clocks[a]>=m.duration[a] && sample.clocks[a]<m.duration[a]+50);
    }
    m.reads=0; m.gap_at=2; m.gap=600; CHECK(gameport_measure(&io,0x201,&sample)==-EAGAIN);
    m.frozen=true; CHECK(gameport_measure(&io,0x201,&sample)==-ETIMEDOUT); m.frozen=false;
    m.absent=true; CHECK(!gameport_measure(&io,0x201,&sample) && !sample.axes); m.absent=false;
    conflict=true; unsigned before=triggers;
    CHECK(gameport_admit(0x201,0,&io)==-EBUSY && before==triggers); conflict=false;
    CHECK(!gameport_admit(0x201,0,&io)); CHECK(gameport_admit(0x201,0,&io)==-EBUSY);
    g_ticks+=1001;gameport_poll(); struct ciuki_joystick_control c=query(257);
    CHECK(c.session==joystick_session() && c.session==123456);
    CHECK(c.attached && c.axes==15 && c.buttons==5 && events);
    before=triggers;gameport_poll();CHECK(triggers==before);
    struct ciuki_joystick_control bad=request(CIUKI_JOY_QUERY,257,c.generation+1);
    CHECK(joystick_control(&bad)==-ENODEV);
    bad=request(CIUKI_JOY_QUERY,257,0); bad.reserved=1; CHECK(joystick_control(&bad)==-EINVAL);
    bad=request(CIUKI_JOY_QUERY,257,0); bad.raw[0]=1; CHECK(joystick_control(&bad)==-EINVAL);
    struct ciuki_hid_caps cap={.version=1,.size=sizeof(cap),.device=257};
    CHECK(!joystick_hid_query(INPUT_HID_GET_CAPS,&cap) && cap.axes==15 && cap.host==UINT32_MAX);
    c=request(CIUKI_JOY_CALIBRATE,257,c.generation); c.axis=0; c.minimum=50; c.centre=100; c.maximum=150;
    CHECK(!joystick_control(&c)); CHECK(query(257).position[0]>=0 && query(257).position[0]<10000);
    c.minimum=100; CHECK(joystick_control(&c)==-EINVAL);
    c.minimum=1; c.centre=2; c.maximum=4000; CHECK(joystick_control(&c)==-EINVAL);
    c=request(CIUKI_JOY_CONFIGURE,257,c.generation); c.mode=1; CHECK(!joystick_control(&c));
    g_ticks+=1001;gameport_poll(); c=query(257); struct ciuki_joystick_control second=query(258);
    CHECK(c.axes==3 && second.axes==3 && c.buttons==1 && second.buttons==1);
    struct joystick_route r={257,c.generation,258,second.generation,1};
    CHECK(!joystick_route_validate(&r)); int32_t axes[4];uint8_t mask,buttons;
    joystick_snapshot(&r,axes,&mask,&buttons); CHECK(mask==15 && buttons==5);
    c=request(CIUKI_JOY_ROUTE,257,r.generation); c.second_device=258;c.second_generation=r.second_generation;c.mode=1;
    CHECK(!joystick_control(&c));
    c.session=123455;CHECK(joystick_control(&c)==-ENODEV);c.session=123456;
    struct joystick_route saved; joystick_default_route(&saved);CHECK(!memcmp(&r,&saved,sizeof(r)));
    c.second_generation++; CHECK(joystick_control(&c)==-ENODEV);
    joystick_default_route(&saved);CHECK(!memcmp(&r,&saved,sizeof(r)));
    m.duration[2]=m.duration[3]=-1; g_ticks+=1001;gameport_poll();
    joystick_snapshot(&r,axes,&mask,&buttons); CHECK(mask==3 && buttons==1);
    CHECK(joystick_route_validate(&r)==-ENODEV);
    g_ticks+=21;gameport_poll();CHECK(m.reads<100);
    m.reads=0;m.gap_at=1;m.gap=5000;g_ticks+=1001;gameport_poll(); CHECK(query(257).attached);
    for (unsigned i=0;i<2;i++) { m.reads=0;m.gap_at=1;m.gap=5000;g_ticks+=1001;gameport_poll(); }
    cap=(struct ciuki_hid_caps){.version=1,.size=sizeof(cap),.device=257};
    CHECK(!gameport_hid_query(INPUT_HID_GET_CAPS,&cap) && !cap.attached);
    valid=false;before=triggers;g_ticks+=1001;gameport_poll(); CHECK(triggers==before);
    cap=(struct ciuki_hid_caps){.version=1,.size=sizeof(cap),.device=257};
    CHECK(!gameport_hid_query(INPUT_HID_GET_CAPS,&cap) && !cap.attached);
    samples=before;
    puts("PASS native RC jitter, clock fault/gap, absent/stuck axes, calibration, lease conflict/loss, 4-axis and dual-stick mapping");
}
static void pci(void)
{
    uint16_t base;
    struct pci_func p={.vendor=0x1102,.command=1,.bar={0x301}};
    for (unsigned id=0x7002;id<=0x7005;id++) { p.device=id;CHECK(!gameport_pci_base(&p,&base) && base==0x300); }
    p.command=0;CHECK(gameport_pci_base(&p,&base)==-ENODEV); p.command=1;
    p.bar[0]=0x10001;CHECK(gameport_pci_base(&p,&base)==-ENODEV);
    p.bar[0]=0x300;CHECK(gameport_pci_base(&p,&base)==-ENODEV);
    p=(struct pci_func){.vendor=0x1234,.class_code=9,.subclass=4,.prog_if=0x10,.command=1,.bar={0,0,0x301}};
    CHECK(!gameport_pci_base(&p,&base) && base==0x300);
    p.prog_if=0; CHECK(gameport_pci_base(&p,&base)==-ENODEV);
    puts("PASS Creative and PCI standard legacy programming interface, disabled/invalid/unknown resources refused");
}
static uint8_t poll_code[16]={0xee,0xec}; /* OUT DX,AL; IN AL,DX */
static void *poll_memory(void *arg,uint32_t linear,unsigned bytes,bool write)
{ (void)arg; (void)write;return linear<=sizeof(poll_code) && bytes<=sizeof(poll_code)-linear ? poll_code+linear : 0; }
static int poll_io(void *arg,uint16_t port,unsigned width,bool write,uint32_t *value)
{ return dosvm_joystick_port(arg,port,width,write,value,0); }
static void real_poll(struct dosvm *v)
{
    struct v86 monitor; struct v86_ops ops={.memory=poll_memory,.arg=v};
    v86_init(&monitor,&ops);monitor.io=poll_io;CHECK(!v86_begin(&monitor,0,500));
    struct v86_frame f={0};f.tf.edx=0x201;f.tf.eflags=V86_VM|V86_IF|2;
    CHECK(!v86_emulate(&monitor,&f) && f.tf.eip==1);
    unsigned count=0;
    do { f.tf.eip=1;CHECK(!v86_emulate(&monitor,&f) && f.tf.eip==2);count++; }
    while ((f.tf.eax&15) && count<2000);
    CHECK(count<2000 && count>1400 && (f.tf.eax&0xf0)==0x60);
    CHECK(monitor.stats.insn[V86_OUT]==1 && monitor.stats.insn[V86_IN]==count);
}
static void usb_and_vm(void)
{
    struct usb_device *d=&usb.devices[0];d->host=&usb;d->generation=77;d->hid[0].reports=&reports;
    reports.caps=(struct ciuki_hid_caps){.version=1,.size=sizeof(reports.caps),.device=1,.generation=77,.attached=1,.classes=CIUKI_HID_JOYSTICK,.axes=15,.buttons=15};
    reports.state=(struct ciuki_hid_state){.version=1,.size=sizeof(reports.state),.device=1,.generation=77,.attached=1};
    reports.layout=&layout;usb.io.now=usb_now;
    for (unsigned a=0;a<4;a++) {
        reports.calibration[a][0]=-32767;reports.calibration[a][1]=0;reports.calibration[a][2]=32767;
        reports.caps.axis[a]=(struct ciuki_hid_axis){.minimum=-32767,.centre=0,.maximum=32767};
    }
    struct hid_values value_report={.axes=15,.buttons=9,.axis={-32767,0,32767,16000}};
    usb_hid_publish(d,&d->hid[0],&value_report,0);
    struct ciuki_joystick_control c=query(1); CHECK(c.raw[0]==-32767 && c.position[0]==-32767);
    c=request(CIUKI_JOY_NEXT,0,0); CHECK(!joystick_control(&c) && c.device==1 && c.generation==77);
    struct dosvm *v=aligned_alloc(_Alignof(struct dosvm),sizeof(*v));CHECK(v);memset(v,0,sizeof(*v));
    v->joystick.route=(struct joystick_route){1,77,0,0,0};
    unsigned before=triggers;
    real_poll(v);
    uint32_t value=0;
    CHECK(!dosvm_joystick_port(v,0x201,1,true,&value,0));
    unsigned counts[4]={0}, loops=0;uint8_t pending=15;
    /* DOS polling-loop algorithm: OUT DX,AL; IN AL,DX; TEST AL,mask;
     * count loops while high; bound disconnected axes. Real decoder fixture
     * below executes the actual OUT/IN instructions through v86_emulate. */
    while (pending && loops<2000) {
        value=0;CHECK(!dosvm_joystick_port(v,0x201,1,false,&value,0));loops++;
        for (unsigned a=0;a<4;a++) if (value&(1u<<a)) counts[a]++;
        pending=(uint8_t)value&15;
    }
    CHECK(loops<2000 && counts[0]<counts[1] && counts[1]<counts[3] && counts[3]<counts[2]);
    CHECK((value&0xf0)==0x60 && triggers==before && samples==before);
    CHECK(dosvm_joystick_port(v,0x201,2,false,&value,0)==-V86_EPERM);
    CHECK(dosvm_joystick_port(v,0x200,1,false,&value,0)==-V86_EPERM);
    struct v86_frame f={0};f.tf.eax=0x12348499;f.tf.ebx=0x5678abcd;f.tf.edx=0;f.tf.eflags=V86_CF;
    CHECK(!dosvm_joystick_bios(v,&f) && !(f.tf.eflags&V86_CF) && f.tf.eax==0x12340060 && f.tf.ebx==0x5678abcd);
    f.tf.eax=0xabcd8400;f.tf.edx=1;CHECK(!dosvm_joystick_bios(v,&f));
    CHECK(f.tf.eax==0xabcd0000 && (uint16_t)f.tf.ebx==208 && f.tf.ecx==416 && f.tf.edx>208 && f.tf.edx<416);
    f.tf.eax=0x8400;f.tf.edx=2;CHECK(!dosvm_joystick_bios(v,&f) && (f.tf.eflags&V86_CF) && (uint16_t)f.tf.eax==0x8600);
    struct joystick_virtual other={0};CHECK(joystick_virtual_io(&other,0,true)==0xff);
    CHECK(v->joystick.route.device==1 && other.route.device==0);
    reports.caps.attached=reports.state.attached=0;
    CHECK(joystick_virtual_io(&v->joystick,0,false)==0xff);
    uint16_t regs[4];CHECK(joystick_virtual_bios(&v->joystick,1,regs)==-ENODEV);
    CHECK(!(regs[0]|regs[1]|regs[2]|regs[3]));
    f=(struct v86_frame){0};f.tf.eax=0x8400;f.tf.edx=1;
    CHECK(!dosvm_joystick_bios(v,&f) && (f.tf.eflags&V86_CF) && !(f.tf.eax|f.tf.ebx|f.tf.ecx|f.tf.edx));
    reports.caps.attached=reports.state.attached=1;reports.caps.generation=78;
    CHECK(joystick_virtual_io(&v->joystick,0,false)==0xff && triggers==before);
    free(v);
    puts("PASS real USB HID cache to virtual 201h, DOS polling-loop timing/order/buttons, isolated VMs, INT15/84 registers/errors, stale/detached sources");
}
int main(void) { settings();pci();CHECK(!joystick_session());gameport_poll();CHECK(!joystick_session());entropy_ready=true;analog();usb_and_vm();printf("PASS joystick: %u assertions\n",assertions);fflush(stdout);return 0; }
