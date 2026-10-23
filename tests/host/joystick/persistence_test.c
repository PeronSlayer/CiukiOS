/* Reuse the existing real RC/HID fixtures, with the production persistence
 * service and a durable byte-store fault adapter. SPDX-License-Identifier: GPL-2.0-only */
#define main joystick_baseline_main
#include "joystick_test.c"
#undef main
#include <ciuki/joystick_settings.h>
#include <ciuki/sha256.h>
struct disk_file { char name[112]; uint8_t bytes[512]; size_t size; };
static struct disk_file disk[128];
static unsigned files, writes;
static bool fail_write, tear_write;
static struct usb_hid_reports other_reports, moved_reports;
static const char *serial_text;
static bool serial_bad;
static struct disk_file *file(const char *name)
{
    for (unsigned i=0;i<files;i++) if (!strcmp(name,disk[i].name)) return &disk[i];
    return 0;
}
static int read_disk(void *ctx,const char *name,void *data,size_t cap,size_t *n)
{
    (void)ctx; CHECK(!joystick_irq_depth);
    struct disk_file *f=file(name); if (!f) return -ENOENT;
    if (f->size>cap) return -EIO;
    memcpy(data,f->bytes,f->size); *n=f->size; return 0;
}
static int write_disk(void *ctx,const char *name,const void *data,size_t n)
{
    (void)ctx; CHECK(!joystick_irq_depth); writes++;
    if (fail_write) return -EIO;
    struct disk_file *f=file(name);
    if (!f) { CHECK(files<128); f=&disk[files++]; snprintf(f->name,sizeof(f->name),"%s",name); }
    CHECK(n<=sizeof(f->bytes));
    f->size=tear_write ? n/2 : n; memcpy(f->bytes,data,f->size);
    return tear_write ? -EIO : 0;
}
static const struct joystick_settings_io io={read_disk,write_disk,0};
int usb_control(struct usb_device *d,uint8_t type,uint8_t request,uint16_t value,
                uint16_t index,void *data,unsigned size)
{
    (void)d; CHECK(!joystick_irq_depth && type==0x80 && request==6 && size==256);
    uint8_t *p=data;
    if (value==0x300) { CHECK(!index); p[0]=4;p[1]=3;p[2]=9;p[3]=4;return 4; }
    CHECK(value==0x301 && index==0x409);
    if (serial_bad) { p[0]=5;p[1]=3;return 5; }
    size_t n=strlen(serial_text); CHECK(n<=126);
    p[0]=(uint8_t)(2+2*n);p[1]=3;
    for (size_t i=0;i<n;i++) { p[2+i*2]=(uint8_t)serial_text[i];p[3+i*2]=0; }
    return (int)(2+2*n);
}
static void connect_usb(unsigned slot,unsigned port,unsigned generation,
                        const char *serial,struct usb_hid_reports *p)
{
    struct usb_device *d=&usb.devices[slot]; memset(d,0,sizeof(*d));memset(p,0,sizeof(*p));
    d->host=&usb; d->generation=generation; d->vendor=0x1234;d->product=0x5678;d->root_port=port;
    d->address=slot+1;d->hid[0].reports=p;d->hid[0].interface=2;
    p->layout=&layout; unsigned locator=1+slot*USB_HID_MAX;
    p->caps=(struct ciuki_hid_caps){.version=1,.size=sizeof(p->caps),.device=locator,.generation=generation,
        .attached=1,.classes=CIUKI_HID_JOYSTICK,.axes=15,.buttons=15};
    p->state=(struct ciuki_hid_state){.version=1,.size=sizeof(p->state),.device=locator,.generation=generation,.attached=1};
    for (unsigned a=0;a<4;a++) {
        p->calibration[a][0]=-32767;p->calibration[a][1]=0;p->calibration[a][2]=32767;
        p->caps.axis[a]=(struct ciuki_hid_axis){.minimum=-32767,.centre=0,.maximum=32767};
    }
    serial_text=serial; usb_identity_serial(d,serial ? 1 : 0);
}
static void save_game(unsigned game,unsigned a,unsigned b)
{
    struct ciuki_joystick_control c=request(CIUKI_JOY_SAVE_GAME,a,a && a!=CIUKI_JOY_DISABLED ? query(a).generation : 0);
    c.axis=game;
    if (b) { c.mode=1;c.second_device=b;c.second_generation=query(b).generation; }
    CHECK(!joystick_control(&c));
}
static void calibrate(unsigned device,int32_t centre)
{
    struct ciuki_joystick_control c=request(CIUKI_JOY_CALIBRATE,device,query(device).generation);
    c.minimum=-20000;c.centre=centre;c.maximum=20000;
    CHECK(!joystick_control(&c));
}
static void status(unsigned pending,int error)
{
    struct ciuki_joystick_control c=request(CIUKI_JOY_SAVE_STATUS,0,0);
    CHECK(!joystick_control(&c)); CHECK((c.buttons!=0)==(pending!=0) && c.minimum==error);
}
static void fresh(void)
{ memset(&usb,0,sizeof(usb));usb.io.now=usb_now;usb.pci.bus=0;usb.pci.dev=7;usb.pci.fn=2; }
int main(void)
{
    fresh();connect_usb(0,0,10,"stick-A",&reports);connect_usb(1,1,11,"stick-B",&other_reports);
    joystick_settings_start(&io);joystick_settings_service();status(0,0);
    CHECK(!files && !writes); /* Migration from no data does not write defaults. */
    struct joystick_route r={0};CHECK(!joystick_settings_game(1,&r) && !r.device);
    calibrate(1,1000);calibrate(1+USB_HID_MAX,-1000);save_game(1,1,1+USB_HID_MAX);
    status(1,0);joystick_settings_service();status(0,0);CHECK(files==3);
    struct ciuki_joystick_control saved=request(CIUKI_JOY_GET_GAME,0,0);saved.axis=1;
    CHECK(!joystick_control(&saved) && saved.attached==2 && saved.device==1 && saved.second_device==1+USB_HID_MAX);
    unsigned before_query=writes;joystick_settings_service();CHECK(writes==before_query);
    saved=request(CIUKI_JOY_GET_GAME,0,0);saved.axis=2;
    CHECK(!joystick_control(&saved) && saved.attached==0 && !saved.device);
    uint8_t id[32], moved_id[32];CHECK(!joystick_identity(1,10,id));
    /* Full reboot: retain only disk bytes. All live generations, USB slots,
     * addresses and ports change. Serial identity still selects A and B. */
    fresh();connect_usb(2,5,110,"stick-A",&reports);connect_usb(0,2,111,"stick-B",&other_reports);
    joystick_settings_start(&io);joystick_settings_service();
    CHECK(!joystick_identity(1+2*USB_HID_MAX,110,moved_id) && !memcmp(id,moved_id,32));
    CHECK(reports.calibration[0][1]==1000 && other_reports.calibration[0][1]==-1000);
    CHECK(!joystick_settings_game(1,&r) && r.device==1+2*USB_HID_MAX && r.generation==110 &&
        r.second_device==1 && r.second_generation==111 && r.mode==1);
    struct disk_file *route_file=file("dos-game-1-joystick.1.ini");CHECK(route_file);
    unsigned rejection_logs=settings_logs;route_file->bytes[0]^=1;
    joystick_settings_start(&io);joystick_settings_service();
    CHECK(settings_logs>rejection_logs && !joystick_settings_game(1,&r) && !r.device);
    route_file->bytes[0]^=1;joystick_settings_start(&io);joystick_settings_service();
    /* Detach/replug in the same boot, with a new port/locator/generation. */
    reports.caps.attached=reports.state.attached=0;
    CHECK(joystick_settings_game(1,&r) && r.device==1 && !r.mode && !r.second_device);
    saved=request(CIUKI_JOY_GET_GAME,0,0);saved.axis=1;
    CHECK(!joystick_control(&saved) && saved.attached==3 && saved.device==1);
    connect_usb(3,6,112,"stick-A",&moved_reports);joystick_settings_service();
    CHECK(moved_reports.calibration[0][1]==1000);
    CHECK(!joystick_settings_game(1,&r) && r.device==1+3*USB_HID_MAX && r.generation==112);
    /* Snapshot already held by a VM does not rebind when source detaches. */
    struct joystick_route snapshot=r; moved_reports.caps.attached=moved_reports.state.attached=0;
    int32_t axes[4];uint8_t mask,buttons;joystick_snapshot(&snapshot,axes,&mask,&buttons);CHECK((mask&3)==0);
    other_reports.caps.attached=other_reports.state.attached=0;
    CHECK(joystick_settings_game(1,&r) && !r.device && !r.generation);
    /* Identical VID/PID, no serial: socket+interface identities stay distinct,
     * retained device slots and addresses do not enter the key. */
    fresh();connect_usb(0,0,210,0,&reports);connect_usb(1,1,211,0,&other_reports);
    joystick_settings_start(&io);joystick_settings_service();
    CHECK(!joystick_identity(1,210,id));CHECK(!joystick_identity(1+USB_HID_MAX,211,moved_id));CHECK(memcmp(id,moved_id,32));
    calibrate(1,500);calibrate(1+USB_HID_MAX,-500);save_game(2,1,0);joystick_settings_service();
    fresh();connect_usb(2,0,310,0,&reports);connect_usb(0,1,311,0,&other_reports);
    joystick_settings_start(&io);joystick_settings_service();
    CHECK(reports.calibration[0][1]==500 && other_reports.calibration[0][1]==-500);
    CHECK(!joystick_settings_game(2,&r) && r.device==1+2*USB_HID_MAX);
    reports.caps.attached=reports.state.attached=0;connect_usb(3,4,312,0,&moved_reports);joystick_settings_service();
    CHECK(!moved_reports.calibration[0][1]);CHECK(joystick_settings_game(2,&r) && r.device==1);
    /* Nonmatching VID/PID or HID interface never inherits a serial-less key. */
    CHECK(!joystick_identity(1,311,id));usb.devices[0].hid[0].interface++;
    CHECK(!joystick_identity(1,311,moved_id) && memcmp(id,moved_id,32));usb.devices[0].hid[0].interface--;
    usb.devices[0].product++;CHECK(!joystick_identity(1,311,moved_id) && memcmp(id,moved_id,32));usb.devices[0].product--;
    /* Explicit off and default do not overwrite each other or the global route. */
    struct ciuki_joystick_control global=request(CIUKI_JOY_ROUTE,1,311);CHECK(!joystick_control(&global));
    save_game(3,CIUKI_JOY_DISABLED,0);joystick_settings_service();
    joystick_settings_start(&io);joystick_settings_service();
    CHECK(!joystick_settings_game(3,&r) && !r.device);
    save_game(3,0,0);joystick_settings_service();CHECK(!joystick_settings_game(3,&r) && r.device==1);
    /* An interrupted save preserves the older valid slot, reports failure,
     * and a later successful retry clears pending/error. */
    calibrate(1,700);joystick_settings_service();
    calibrate(1,900);tear_write=true;joystick_settings_service();status(1,-EIO);tear_write=false;
    unsigned logs=settings_logs;
    joystick_settings_start(&io); other_reports.calibration[0][1]=0;joystick_settings_service();
    CHECK(settings_logs>logs && other_reports.calibration[0][1]==700);
    calibrate(1,1100);fail_write=true;joystick_settings_service();status(1,-EIO);
    fail_write=false;joystick_settings_service();status(0,0);
    /* Reject both corrupt slots and restore driver defaults on a real reboot. */
    char stem[96];CHECK(!joystick_identity(1,311,id));memcpy(stem,"joystick-",9);sha256_hex(id,stem+9);
    for (unsigned slot=0;slot<2;slot++) {
        char name[112];snprintf(name,sizeof(name),"%s.%u.ini",stem,slot);
        struct disk_file *f=file(name);CHECK(f);f->bytes[0]^=1;
    }
    fresh();connect_usb(0,1,411,0,&other_reports);logs=settings_logs;
    joystick_settings_start(&io);joystick_settings_service();CHECK(settings_logs>logs && !other_reports.calibration[0][1]);
    /* Advertised malformed serials and duplicate serial identities refuse
     * persistence, without disabling the live controller. */
    serial_bad=true;connect_usb(2,2,412,"bad",&reports);serial_bad=false;
    CHECK(joystick_identity(1+2*USB_HID_MAX,412,id)==-EAGAIN);
    save_game(1,0,0);joystick_settings_service();
    char long_a[127],long_b[127];memset(long_a,'a',126);long_a[126]=0;memcpy(long_b,long_a,sizeof(long_a));long_b[125]='b';
    connect_usb(2,2,413,long_a,&reports);connect_usb(3,3,414,long_b,&moved_reports);
    CHECK(!joystick_identity(1+2*USB_HID_MAX,413,id) && !joystick_identity(1+3*USB_HID_MAX,414,moved_id) && memcmp(id,moved_id,32));
    connect_usb(2,2,413,0,&reports);usb.devices[2].parent=1;usb.devices[2].port=3;
    CHECK(!joystick_identity(1+2*USB_HID_MAX,413,id));usb.devices[2].port=4;
    CHECK(!joystick_identity(1+2*USB_HID_MAX,413,moved_id) && memcmp(id,moved_id,32));
    connect_usb(2,2,413,"duplicate",&reports);connect_usb(3,3,414,"duplicate",&moved_reports);
    joystick_settings_service();
    struct ciuki_joystick_control duplicate=request(CIUKI_JOY_SAVE_GAME,1+2*USB_HID_MAX,413);duplicate.axis=1;
    CHECK(joystick_control(&duplicate)==-ENODEV);
    /* Legacy off migrates; legacy explicit boot locators are recorded and
     * ignored. Corrupt/unsupported versions never become live locators. */
    memset(disk,0,sizeof(disk));files=0;
    static const char off[]="4294967295 0 0 0 0 0\n", old[]="257 42 1 77 1 123456\n";
    CHECK(!write_disk(0,"dos-game-1-joystick.cfg",off,sizeof(off)-1));
    CHECK(!write_disk(0,"dos-game-2-joystick.cfg",old,sizeof(old)-1));
    logs=settings_logs;joystick_settings_start(&io);joystick_settings_service();CHECK(settings_logs>logs);
    CHECK(!joystick_settings_game(1,&r) && !r.device);CHECK(!joystick_settings_game(2,&r) && !r.device);
    /* Analog identity is independent of enumeration order, keyed by I/O base;
     * calibration survives detach and worker restart, wiring is restored. */
    static struct rc_model model={.duration={100,500,900,1300}};
    struct gameport_io port_io={model_read,model_trigger,model_clock,&model};
    CHECK(!gameport_admit(0x201,0,&port_io));g_ticks+=1001;gameport_poll();joystick_settings_service();
    struct ciuki_joystick_control analog_c=query(257);CHECK(!joystick_identity(257,analog_c.generation,id));
    analog_c=request(CIUKI_JOY_CALIBRATE,257,analog_c.generation);analog_c.minimum=50;analog_c.centre=100;analog_c.maximum=150;
    CHECK(!joystick_control(&analog_c));joystick_settings_service();
    model.absent=true;g_ticks+=1001;gameport_poll();model.absent=false;g_ticks+=1001;gameport_poll();
    joystick_settings_start(&io);joystick_settings_service();
    analog_c=query(257);CHECK(!joystick_identity(257,analog_c.generation,moved_id) && !memcmp(id,moved_id,32));
    CHECK(analog_c.position[0]>=0 && analog_c.position[0]<10000);
    analog_c=request(CIUKI_JOY_CONFIGURE,257,analog_c.generation);analog_c.mode=1;CHECK(!joystick_control(&analog_c));
    g_ticks+=1001;gameport_poll();joystick_settings_service();
    analog_c=query(257);CHECK(analog_c.mode==1 && analog_c.axes==3);
    CHECK(!gameport_restore_mode(257,analog_c.generation,0));g_ticks+=1001;gameport_poll();
    joystick_settings_start(&io);joystick_settings_service();g_ticks+=1001;gameport_poll();joystick_settings_service();
    CHECK(query(257).mode==1);
    CHECK(!gameport_admit(0x300,0,&port_io));g_ticks+=1001;gameport_poll();joystick_settings_service();
    struct ciuki_joystick_control second_port=query(259);
    CHECK(!joystick_identity(259,second_port.generation,moved_id) && memcmp(id,moved_id,32));
    CHECK(second_port.position[0]<0); /* The 201h calibration was not applied to 300h. */
    printf("PASS joystick persistence: %u assertions; reboot, moved port, identical devices, corruption, migration, save failure, analog and wiring\n",assertions);
    fflush(stdout);return 0;
}
