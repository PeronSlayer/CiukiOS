/* Descriptor -> production USB HID -> merged queue -> user grant, no hardware.
 * Included by input_grant_test.c. SPDX-License-Identifier: GPL-2.0-only */
static unsigned drain_hid(struct process *p, int fd, struct ciuki_input_event *out)
{
    int n=grant_input_read(p,fd,BUFFER,64);
    if (n==-EAGAIN) return 0;
    CHECK(n>0); CHECK(!ua_read(p->memory,out,BUFFER,(unsigned)n*sizeof(*out))); return (unsigned)n;
}
static int hid_ioctl(struct process *p, int fd, unsigned op, void *record, unsigned bytes)
{
    CHECK(!ua_write(p->memory,BUFFER,record,bytes));
    int err=grant_input_devctl(p,fd,op,BUFFER,bytes);
    CHECK(!ua_read(p->memory,record,BUFFER,bytes)); return err;
}
static void fixture_attach(struct usb_host *host, const uint8_t *descriptor, unsigned length, unsigned protocol)
{
    struct usb_device *d=&host->devices[0];
    *d=(struct usb_device){.host=host,.generation=77,.address=1,
        .hid={{.protocol=protocol,.endpoint=1,.packet=16,.interval=1}}};
    hid_fixture=descriptor; hid_fixture_length=length;
    protocol_writes=output_writes=0; protocol_stall=false;
    CHECK(!usb_hid_attach(d,&d->hid[0],length));
}
static void fixture_detach(struct usb_host *host)
{
    usb_hid_release(&host->devices[0],&host->devices[0].hid[0]);
    if (host->devices[0].hid[0].reports) kfree(host->devices[0].hid[0].reports->layout);
    kfree(host->devices[0].hid[0].reports); host->devices[0].hid[0].reports=NULL;
}
static void hid_extension_test(struct process *p, int fd)
{
    static struct usb_host host;
    host=(struct usb_host){.io={.now=input_now},.ops=&input_ops}; query_host=&host;
    unsigned blocks=heap_blocks;
    struct ciuki_input_event e[64];
    fixture_attach(&host,hid_nkro_keyboard,sizeof(hid_nkro_keyboard),1);
    struct usb_device *d=&host.devices[0]; struct usb_hid *h=&d->hid[0];
    CHECK(h->reports && h->report_protocol && protocol_writes==2);
    CHECK(output_writes==1 && output_id==3 && output_length==2 && output_bytes[0]==3);
    CHECK(drain_hid(p,fd,e)==1 && e[0].type==CIUKI_INPUT_DEVICE && e[0].value==1 && e[0].value2==1);
    struct ciuki_hid_caps caps={.version=1,.size=sizeof(caps),.device=1};
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_CAPS,&caps,sizeof(caps)) && caps.generation==77 && caps.keyboard[0]==0xf0);
    struct ciuki_hid_caps saved=caps;
    caps.version=99; CHECK(hid_ioctl(p,fd,INPUT_HID_GET_CAPS,&caps,sizeof(caps))==-EINVAL);
    caps=saved; caps.reserved=1; CHECK(hid_ioctl(p,fd,INPUT_HID_GET_CAPS,&caps,sizeof(caps))==-EINVAL);
    CHECK(grant_input_devctl(p,fd,INPUT_HID_GET_CAPS,BUFFER,sizeof(caps)-1)==-EINVAL);
    CHECK(grant_input_devctl(p,fd,INPUT_HID_GET_CAPS,0,sizeof(caps))==-EFAULT);
    CHECK(grant_input_devctl(p,-1,INPUT_HID_GET_CAPS,BUFFER,sizeof(caps))<0);
    uint8_t keys[16]={3,0x01,0xff}; /* eight ordinary keys plus Ctrl */
    CHECK(!usb_hid_report(d,h,keys,16));
    unsigned n=drain_hid(p,fd,e), usages=0, translated=0;
    for (unsigned i=0;i<n;i++) {
        usages+=e[i].type==CIUKI_INPUT_USAGE;
        translated+=e[i].type==CIUKI_INPUT_KEY;
        if (e[i].type==CIUKI_INPUT_USAGE) CHECK(e[i].lost_count==1 && e[i].value2==1 && e[i].generation==77);
    }
    CHECK(usages==9 && translated==9);
    CHECK(!usb_hid_report(d,h,keys,16) && !drain_hid(p,fd,e));
    struct ciuki_hid_state state={.version=1,.size=sizeof(state),.device=1,.generation=77};
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_STATE,&state,sizeof(state)) && state.keyboard[0]==0xf0);
    struct ciuki_hid_state prior=state;
    CHECK(usb_hid_report(d,h,keys,4)==-USB_EPROTO);
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_STATE,&state,sizeof(state)) && !memcmp(&state,&prior,sizeof(state)));
    input_tick=h->repeat_at; usb_hid_poll(d,h);
    n=drain_hid(p,fd,e); CHECK(n && e[0].type==CIUKI_INPUT_KEY && e[0].value==2);
    fixture_detach(&host); n=drain_hid(p,fd,e); usages=translated=0;
    for (unsigned i=0;i<n;i++) { usages+=e[i].type==CIUKI_INPUT_USAGE && !e[i].value; translated+=e[i].type==CIUKI_INPUT_KEY && !e[i].value; }
    CHECK(usages==9 && translated==9 && e[n-1].type==CIUKI_INPUT_DEVICE && !e[n-1].value);
    /* Boot report keyboard, descriptor LEDs and rollover retain modifiers. */
    fixture_attach(&host,hid_boot_keyboard,sizeof(hid_boot_keyboard),1); drain_hid(p,fd,e);
    CHECK(output_writes==1 && output_id==0 && output_length==1);
    uint8_t boot[]={2,0xff,4,0,0,0,0,0}; CHECK(!usb_hid_report(d,h,boot,8)); drain_hid(p,fd,e);
    boot[0]=0; boot[2]=1; CHECK(!usb_hid_report(d,h,boot,8) && !drain_hid(p,fd,e));
    CHECK(h->reports->state.keyboard[0]==16 && h->reports->state.keyboard[28]==2);
    fixture_detach(&host); drain_hid(p,fd,e);
    /* SET_PROTOCOL STALL keeps the advertised boot keyboard usable. */
    *d=(struct usb_device){.host=&host,.generation=78,.address=1,
        .hid={{.protocol=1,.endpoint=1,.packet=8,.interval=1}}};
    protocol_stall=true; CHECK(!usb_hid_attach(d,h,sizeof(hid_boot_keyboard)) && !h->reports && !h->report_protocol);
    uint8_t plain[]={0,0,4,0,0,0,0,0}; CHECK(!usb_hid_report(d,h,plain,8));
    CHECK(drain_hid(p,fd,e)==2);
    state=(struct ciuki_hid_state){.version=1,.size=sizeof(state),.device=1,.generation=78};
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_STATE,&state,sizeof(state)) && state.keyboard[0]==16);
    fixture_detach(&host); drain_hid(p,fd,e);
    for (int failure=0;failure<2;failure++) {
        *d=(struct usb_device){.host=&host,.generation=78,.address=1,
            .hid={{.protocol=1,.endpoint=1,.packet=16,.interval=1}}};
        protocol_stall=false; alloc_fail=failure;
        CHECK(!usb_hid_attach(d,h,sizeof(hid_boot_keyboard)) && !h->reports && !h->report_protocol && h->active);
        alloc_fail=-1; fixture_detach(&host); drain_hid(p,fd,e); CHECK(heap_blocks==blocks);
    }
    /* Overlapping usages in separate report IDs release only on the last ID. */
    uint8_t ids[2*sizeof(hid_nkro_keyboard)];
    memcpy(ids,hid_nkro_keyboard,sizeof(hid_nkro_keyboard));
    memcpy(ids+sizeof(hid_nkro_keyboard),hid_nkro_keyboard,sizeof(hid_nkro_keyboard));
    ids[sizeof(hid_nkro_keyboard)+7]=6;
    fixture_attach(&host,ids,sizeof(ids),0); drain_hid(p,fd,e);
    uint8_t one[16]={3,0,1}, two[16]={6,0,1};
    CHECK(!usb_hid_report(d,h,one,16)); CHECK(drain_hid(p,fd,e)>=2);
    CHECK(!usb_hid_report(d,h,two,16) && !drain_hid(p,fd,e));
    one[2]=0; CHECK(!usb_hid_report(d,h,one,16) && !drain_hid(p,fd,e));
    CHECK(h->reports->state.keyboard[0]==16);
    two[2]=0; CHECK(!usb_hid_report(d,h,two,16) && drain_hid(p,fd,e)==2);
    fixture_detach(&host); drain_hid(p,fd,e);
    /* A composite keyboard and mouse share the interface without clearing keys. */
    uint8_t composite[200];
    memcpy(composite,hid_boot_keyboard,6); composite[6]=0x85; composite[7]=1;
    memcpy(composite+8,hid_boot_keyboard+6,sizeof(hid_boot_keyboard)-6);
    unsigned at=sizeof(hid_boot_keyboard)+2;
    memcpy(composite+at,qemu_mouse_descriptor,6); at+=6; composite[at++]=0x85; composite[at++]=2;
    memcpy(composite+at,qemu_mouse_descriptor+6,sizeof(qemu_mouse_descriptor)-6); at+=sizeof(qemu_mouse_descriptor)-6;
    fixture_attach(&host,composite,at,0); drain_hid(p,fd,e);
    uint8_t numbered_key[]={1,0,0,4,0,0,0,0,0};
    CHECK(!usb_hid_report(d,h,numbered_key,9)); drain_hid(p,fd,e);
    uint8_t numbered_mouse[]={2,1,1,0xff,1}; CHECK(!usb_hid_report(d,h,numbered_mouse,5));
    CHECK(h->reports->state.keyboard[0]==16);
    n=drain_hid(p,fd,e); CHECK(n==4);
    for (unsigned i=0;i<n;i++) CHECK(e[i].type!=CIUKI_INPUT_KEY && e[i].type!=CIUKI_INPUT_USAGE);
    fixture_detach(&host); n=drain_hid(p,fd,e); CHECK(n==4 && e[n-1].type==CIUKI_INPUT_DEVICE);
    fixture_attach(&host,hid_consumer,sizeof(hid_consumer),0); CHECK(!protocol_writes); drain_hid(p,fd,e);
    for (unsigned a=0;a<6;a++) {
        uint8_t press[]={4,1u<<a}; CHECK(!usb_hid_report(d,h,press,2));
        CHECK(drain_hid(p,fd,e)==1 && e[0].type==CIUKI_INPUT_CONSUMER && e[0].code==(int)a+1 && e[0].value==1);
        CHECK(!usb_hid_report(d,h,press,2) && !drain_hid(p,fd,e));
        press[1]=0; CHECK(!usb_hid_report(d,h,press,2));
        CHECK(drain_hid(p,fd,e)==1 && !e[0].value);
    }
    uint8_t press[]={4,63}; CHECK(!usb_hid_report(d,h,press,2)); drain_hid(p,fd,e);
    fixture_detach(&host); CHECK(drain_hid(p,fd,e)==7);
    fixture_attach(&host,hid_joystick,sizeof(hid_joystick),0); drain_hid(p,fd,e);
    caps=(struct ciuki_hid_caps){.version=1,.size=sizeof(caps),.device=1};
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_CAPS,&caps,sizeof(caps)) && caps.axes==3 && caps.buttons==15);
    CHECK(caps.axis[0].centre==128 && caps.axis[0].physical_minimum==-100);
    uint8_t joy[]={0,255,9}; CHECK(!usb_hid_report(d,h,joy,3));
    n=drain_hid(p,fd,e); CHECK(n==4);
    CHECK(e[0].type==CIUKI_INPUT_JOY_AXIS && e[0].value==-32767 && e[1].value==32767);
    struct ciuki_hid_calibration cal={.version=1,.size=sizeof(cal),.device=1,.generation=77,.axis=0,.minimum=10,.centre=120,.maximum=240};
    CHECK(!hid_ioctl(p,fd,INPUT_HID_CALIBRATE,&cal,sizeof(cal)));
    cal.centre=10; CHECK(hid_ioctl(p,fd,INPUT_HID_CALIBRATE,&cal,sizeof(cal))==-EINVAL);
    state=(struct ciuki_hid_state){.version=1,.size=sizeof(state),.device=1,.generation=76};
    CHECK(hid_ioctl(p,fd,INPUT_HID_GET_STATE,&state,sizeof(state))==-ENODEV);
    joy[0]=120; joy[1]=128; CHECK(!usb_hid_report(d,h,joy,3));
    n=drain_hid(p,fd,e); CHECK(n==2 && !e[0].value && !e[1].value);
    fixture_detach(&host); drain_hid(p,fd,e);
    fixture_attach(&host,hid_gamepad,sizeof(hid_gamepad),0); drain_hid(p,fd,e);
    uint8_t pad[]={5,0,0,0,0,0x83,0}; CHECK(!usb_hid_report(d,h,pad,7));
    CHECK(drain_hid(p,fd,e)==2 && e[0].type==CIUKI_INPUT_JOY_HAT && e[0].value==3 && e[1].code==4);
    /* Overflow recovery uses the full state query, independent of event loss. */
    for (unsigned i=0;i<300;i++) { pad[5]=(i&1) ? 0x83 : 8; CHECK(!usb_hid_report(d,h,pad,7)); }
    n=drain_hid(p,fd,e); CHECK(n && e[0].type==CIUKI_INPUT_RESYNC);
    while (drain_hid(p,fd,e)) {}
    state=(struct ciuki_hid_state){.version=1,.size=sizeof(state),.device=1,.generation=77};
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_STATE,&state,sizeof(state)) && state.hats[0]==3 && state.buttons==8);
    usb_hid_release(d,h); drain_hid(p,fd,e);
    CHECK(!hid_ioctl(p,fd,INPUT_HID_GET_STATE,&state,sizeof(state)) && !state.attached && state.hats[0]==-1 && !state.buttons);
    kfree(h->reports->layout); kfree(h->reports); h->reports=NULL;
    CHECK(heap_blocks==blocks); query_host=NULL; hid_fixture=NULL;
    printf("PASS F1-104 native ABI, boot/report fallback, NKRO, repeat, LEDs, consumer edges, calibration, hats, detach and overflow recovery; input_event_bytes=%zu hid_bytes=%zu runtime_bytes=%zu\n",
           sizeof(struct input_event),sizeof(struct usb_hid),sizeof(struct usb_hid_reports));
}
