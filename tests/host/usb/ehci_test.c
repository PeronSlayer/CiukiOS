/* Production core/engines and HID, over an EHCI register/RAM model.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main usb_regression_main
#include "usb_test.c"
#undef main
#include "ehci_model.h"
static struct ehci_model e;
static struct usb_host group[4];
static struct usb_host *hc(void) { return &group[0]; }
static int peripheral_packet(struct ehci_model *model, unsigned addr, unsigned ep,
                             unsigned pid, unsigned toggle, uint8_t *buf, unsigned n)
{
    m.now = model->now;
    /* Hub reset selects a downstream address-zero device independently of
     * the EHCI root-port selector. Preserve that peripheral-side routing. */
    if (m.dev[0].kind != HUB) m.selected = model->selected;
    /* The existing USB peripheral corpus is independent of the EHCI walker. */
    if (ep > 1) {
        if (model->now < model->bulk_ready_at) return -USB_EAGAIN;
        unsigned *expected = &model->bulk_toggles[pid == USB_PID_IN];
        assert(toggle == *expected); *expected ^= 1;
        if (pid == USB_PID_IN) {
            unsigned got = model->short_bytes ? model->short_bytes - 1 : n;
            model->short_bytes = 0; assert(got <= n); memset(buf, 0x5a, got); return (int)got;
        }
        for (unsigned i = 0; i < n; i++) assert(buf[i] == 0xa7);
        return (int)n;
    }
    if (!ep && pid == USB_PID_SETUP && buf[0] == 0x23 && (buf[1] == 8 || buf[1] == 9)) {
        model->clear_value = buf[2] | (unsigned)buf[3] << 8; model->clear_port = buf[4];
        if (buf[1] == 8) { model->tt_clear++; if (model->clear_fail) return -USB_EPIPE; }
        else {
            model->tt_reset++; if (model->tt_reset_fail) return -USB_EPIPE;
            assert(!(model->regs[0x24/4] & 0x4000));
            memset(model->split_pending, 0, sizeof(model->split_pending));
        }
        model->tt_busy = model->split_wait = false;
        memcpy(m.setup, (unsigned[8]){buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]}, sizeof(m.setup));
        return 8;
    }
    if (!ep && !n && m.setup[0] == 0x23 && (m.setup[1] == 8 || m.setup[1] == 9)) return 0;
    unsigned before = m.data_at;
    int got = transaction(&m, addr, ep, pid, toggle, buf, n);
    if (!ep && pid == USB_PID_IN && m.setup[1] == 6 && m.setup[3] == 1 && !before && got >= 8 &&
        (m.selected < 2 || (m.hub_status[m.selected - 2] & 0x400))) buf[7] = 64;
    if (!ep && pid == USB_PID_IN && m.setup[1] == 6 && m.setup[3] == 2 && model->hid_interval &&
        before<=33 && got>0 && before+(unsigned)got>33) buf[33-before]=(uint8_t)model->hid_interval;
    return got;
}
static struct pci_func controller(void)
{ return (struct pci_func){.vendor=0x8086,.device=0x24cd,.bus=0,.dev=29,.fn=7,
    .class_code=0x0c,.subclass=3,.prog_if=0x20,.bar={0xf0000000},.irq_line=11,.irq_pin=1}; }
static void prepare_model(bool hub)
{
    if (hc()->irq_attached) usb_host_quarantine(hc(), "fixture_end");
    setup(false, hub); memset(group, 0, sizeof(group));
    ehci_model_init(&e, hc()); e.transaction = peripheral_packet;
    m.regs[0x10/4] = 5 | 5u << 16; /* peripheral corpus's root attachment view */
    if (hub) e.regs[0x68/4] = 0;
    if (hub) m.hub_status[0] |= 0x200;
}
static int attach_ehci(void)
{
    struct pci_func p = controller(); struct usb_io cb = ehci_io(&e);
    return usb_attach(hc(), &p, &cb, m.dma, 0x100000);
}
static void start_ehci(bool irq)
{
    assert(!attach_ehci() && !hc()->ready && !(e.regs[0x60/4] & 1));
    route_ready = irq; e.deliver_irq = irq;
    assert(!usb_ehci_activate(hc(), group, 1));
    assert(hc()->ready && hc()->ehci_started && (e.regs[0x60/4] & 1));
}
static void step_ehci(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        ehci_sleep(&e, 1); usb_step(hc());
        fs_lock_take(&hc()->transfer_lock); usb_hid_service(hc()); fs_lock_drop(&hc()->transfer_lock);
    }
}
static void enumeration_hid(void)
{
    for (unsigned irq = 0; irq < 3; irq++) {
        prepare_model(false); start_ehci(irq != 0); if (irq == 2) e.deliver_irq = false;
        usb_step(hc()); step_ehci(6);
        assert(hc()->device_count == 2 && hc()->devices[0].configured && hc()->devices[1].configured);
        assert(hc()->devices[0].ep0 == 64 && hc()->devices[0].high_speed && usb_devices == 2);
        struct usb_hid *kbd = &hc()->devices[0].hid[0];
        m.dev[0].report[2] = 4; m.dev[0].report_len = 8;
        step_ehci(70); assert(held[INPUT_KEY_A] && kbd->toggle == 1);
        memset(m.dev[0].report, 0, 8); m.dev[0].report_len = 8;
        step_ehci(70); assert(!held[INPUT_KEY_A] && !kbd->toggle);
        unsigned complete = e.completions;
        usb_detach(&hc()->devices[0], "test_detach");
        m.dev[0].report_len = 8; step_ehci(70);
        assert(e.completions == complete && !kbd->active && !kbd->pipe_state);
        assert(irq != 1 || hc()->interrupts > 0);
        assert(irq != 2 || !hc()->interrupts);
        usb_host_quarantine(hc(), "test_stop");
        assert(!hc()->ready && !(e.config[1] & 4) && !e.regs[0x60/4]);
    }
    puts("PASS EHCI enumeration/control/HID toggles/NAK/detach/stop: polling, IRQ and lost IRQ");
}
static struct usb_device *bulk_device(void)
{
    struct usb_device *d = &hc()->devices[0];
    *d = (struct usb_device){.host=hc(),.address=1,.ep0=64,.high_speed=true,.present=true,.configured=true};
    d->storage = (struct usb_storage){.bound=true,.in=2,.out=3,.in_packet=512,.out_packet=512};
    hc()->device_count = 1; e.regs[0x64/4] = 5;
    return d;
}
static void bulk_and_unlink(void)
{
    for (unsigned mode = 0; mode < 7; mode++) {
        prepare_model(false); start_ehci(false); struct usb_device *d = bulk_device();
        uint8_t data[1536]; unsigned got = 0;
        if (mode == 1) e.short_bytes = 18; /* 17-byte short packet */
        if (mode == 2) e.iaa_lost = true;
        if (mode == 3) e.inject = 0x40;
        if (mode == 4) e.inject = 0x48;
        if (mode == 5) e.nak = true;
        if (mode == 6) e.late_ack = true;
        unsigned requested = mode == 6 ? 512u : sizeof(data);
        usb_host_take(hc());
        int err = usb_bulk(d, true, data, requested, &got);
        usb_host_drop(hc());
        if (mode < 3 || mode == 6) {
            assert(!err && got == (mode == 1 ? 17u : requested));
            for (unsigned i = 0; i < got; i++) assert(data[i] == 0x5a);
            assert(d->storage.in_toggle == 1);
        } else assert(err == (mode == 3 ? -USB_EPIPE : mode == 5 ? -USB_ETIMEDOUT : -USB_EIO));
        assert(!hc()->quarantined); /* the asynchronous schedule has a stop proof */
        assert(*(uint32_t *)(m.dma + USB_EHCI_HEAD) == hc()->dma_phys + USB_EHCI_HEAD + 2);
        assert(e.doorbells && d->packet_trace.valid && !d->packet_trace.retired);
        if (!mode) {
            memset(data, 0xa7, sizeof(data)); usb_host_take(hc());
            assert(!usb_bulk(d, false, data, sizeof(data), &got) && got == sizeof(data)); usb_host_drop(hc());
        }
        usb_host_quarantine(hc(), "bulk_end");
    }
    prepare_model(false); start_ehci(false); struct usb_device *d = bulk_device();
    uint8_t buf[512]; unsigned got;
    e.iaa_lost = e.ass_stuck = true;
    usb_host_take(hc()); assert(usb_bulk(d, true, buf, sizeof(buf), &got) == -USB_EIO); usb_host_drop(hc());
    assert(hc()->quarantined && !strcmp(hc()->reason, "ehci_unlink_uncertain"));
    assert(leases[hc()->claims[2]].state == RS_QUARANTINED && hc()->dma == m.dma);
    puts("PASS EHCI bulk 512-byte MPS/IN/OUT/short/STALL/transaction error/NAK timeout/IAA loss/uncertain unlink quarantine");
}
static void periodic_and_faults(void)
{
    for (unsigned interval=1;interval<=14;interval++) {
        prepare_model(false); start_ehci(false);
        struct usb_device *d=bulk_device(); m.dev[0].addr=1;
        struct usb_hid *p=&d->hid[0];
        *p=(struct usb_hid){.endpoint=1,.packet=8,.interval=(uint8_t)interval};
        uint8_t report[8];
        assert(hc()->ops->interrupt(d,p,report,8)==-USB_EAGAIN);
        unsigned packets=e.packets; ehci_sleep(&e,1024);
        assert(e.packets-packets==8192u/(1u<<(interval-1)));
        assert(hc()->ops->interrupt(d,p,report,8)==-USB_EAGAIN && p->pipe_state==1);
        hc()->ops->interrupt_stop(d,p); packets=e.packets; ehci_sleep(&e,3);
        assert(e.packets==packets && !p->pipe_state);
        usb_host_quarantine(hc(),"interval_end");
    }
    prepare_model(false); start_ehci(false); struct usb_device *d=bulk_device(); m.dev[0].addr=1;
    struct usb_hid *p=&d->hid[0]; *p=(struct usb_hid){.endpoint=1,.packet=8,.interval=1};
    uint8_t report[8];
    assert(hc()->ops->interrupt(d,p,report,8)==-USB_EAGAIN);
    m.dev[0].report_len=8; ehci_sleep(&e,1);
    assert(hc()->ops->interrupt(d,p,report,8)==-USB_EAGAIN && p->pipe_state==2);
    uint8_t saved[224]; memcpy(saved,m.dma+USB_EHCI_HID_BASE,sizeof(saved));
    ehci_sleep(&e,1); assert(hc()->ops->interrupt(d,p,report,8)==-USB_EAGAIN);
    assert(!memcmp(saved,m.dma+USB_EHCI_HID_BASE,sizeof(saved)));
    ehci_sleep(&e,1); assert(hc()->ops->interrupt(d,p,report,8)==8 && !p->pipe_state);
    assert(hc()->ops->interrupt(d,p,report,8)==-USB_EAGAIN);
    e.freeze=true; ehci_sleep(&e,USB_PERIODIC_WATCHDOG_MS);
    (void)hc()->ops->interrupt(d,p,report,8); assert(hc()->quarantined);
    prepare_model(false); start_ehci(false); d=bulk_device(); m.dev[0].addr=1;
    p=&d->hid[0]; *p=(struct usb_hid){.endpoint=1,.packet=8,.interval=1};
    ((uint32_t *)m.dma)[0]=hc()->dma_phys+USB_DMA_BYTES+2;
    assert(hc()->ops->interrupt(d,p,report,8)==-USB_EIO && hc()->quarantined);
    prepare_model(false); start_ehci(false); d=bulk_device(); e.dma_error=true;
    uint8_t data[512]; unsigned got;
    usb_host_take(hc()); assert(usb_bulk(d,true,data,sizeof(data),&got)==-USB_EIO); usb_host_drop(hc());
    assert(hc()->quarantined && !strcmp(hc()->reason,"ehci_dma_length"));
    puts("PASS EHCI hardware bInterval 1..14, periodic cancellation/two-frame reuse, stalled clock and corrupt DMA/link containment");
}
static void ownership_and_admission(void)
{
    for (unsigned fault = 0; fault < 5; fault++) {
        prepare_model(false);
        e.bios_stuck = fault == 1; e.os_stuck = fault == 2; e.smi_stuck = fault == 3;
        if (fault == 4) { e.regs[2] = 0x5000; e.config[0x50/4] = 0x5002; }
        int err = attach_ehci();
        assert(fault ? err < 0 && hc()->quarantined && !hc()->owned : !err && hc()->owned);
        if (fault) assert(!e.reg_writes && !hc()->ready && !e.regs[0x60/4]);
    }
    prepare_model(false); e.regs[1] = 0x1202; assert(!attach_ehci());
    assert(usb_ehci_activate(hc(), group, 1) == -ENOSYS && !hc()->ready);
    assert(!strcmp(hc()->reason, "route_only_companions_unqualified"));
    struct usb_controller_inventory info = {.version=1,.size=sizeof(info)};
    assert(!usb_controller_snapshot(hc(), &info) && (info.flags & USB_CONTROLLER_ROUTE_ONLY));
    info.version++; assert(usb_controller_snapshot(hc(), &info) == -EINVAL);
    info.version=1; info.size--; assert(usb_controller_snapshot(hc(), &info) == -EINVAL);
    prepare_model(false); assert(!attach_ehci()); hc()->inventory.quirks |= USB_QUIRK_AMD8111;
    assert(usb_ehci_activate(hc(), group, 1) == -ENOSYS && !e.regs[0x60/4]);
    prepare_model(false);
    struct pci_func quirk=controller(); quirk.vendor=0x10de; quirk.device=0x00d8;
    struct usb_io cb=ehci_io(&e);
    assert(!usb_attach(hc(),&quirk,&cb,m.dma,0x100000));
    assert(hc()->inventory.quirks & USB_QUIRK_NVIDIA_31BIT);
    assert(!usb_ehci_activate(hc(),group,1)); usb_host_quarantine(hc(),"nvidia_end");
    prepare_model(false); quirk=controller(); quirk.vendor=0x1106; quirk.device=0x3104; quirk.revision=0x60;
    cb=ehci_io(&e); e.config[0x48/4]=0x003412ab;
    assert(!usb_attach(hc(),&quirk,&cb,m.dma,0x100000));
    assert(!usb_ehci_activate(hc(),group,1) && (e.config[0x48/4]>>24)==0x20);
    assert((e.config[0x48/4]&0xffffff)==0x3412ab); usb_host_quarantine(hc(),"via_end");
    prepare_model(false); quirk=controller(); quirk.vendor=0x1022; quirk.device=0x7463;
    cb=ehci_io(&e); assert(!usb_attach(hc(),&quirk,&cb,m.dma,0x100000));
    assert((hc()->inventory.quirks & USB_QUIRK_AMD8111) && usb_ehci_activate(hc(),group,1)==-ENOSYS);
    prepare_model(false); e.regs[1] = 0x1202; assert(!attach_ehci());
    group[1].pci = controller(); group[1].pci.fn = 0; group[1].pci.prog_if = 0;
    group[1].owned = group[1].ready = true;
    for (unsigned i=0;i<3;i++) group[1].claims[i]=-1;
    group[1].ready=false;
    assert(usb_ehci_activate(hc(),group,2)==-ENOSYS && !hc()->ready);
    assert(group[1].inventory.parent_locator==usb_pci_locator(&hc()->pci));
    group[1].ready=true;
    assert(!usb_ehci_activate(hc(), group, 2));
    assert(group[1].inventory.parent_locator == usb_pci_locator(&hc()->pci));
    assert(hc()->inventory.port_companion[1] == usb_pci_locator(&group[1].pci));
    e.regs[0x64/4] = 0x401; e.port_speed[0] = 0;
    usb_step(hc()); assert(e.regs[0x64/4] & 0x2000);
    assert(!hc()->root_blocked[0] && !hc()->root_child[0] && e.port_handoffs == 1);
    /* A physical disconnect returns ownership in hardware, not by stealing
     * a still-connected companion port in software. */
    e.regs[0x64/4] = 2; hc()->next_scan=0; usb_step(hc());
    e.regs[0x64/4] = 3; e.port_speed[0] = 1; hc()->next_scan=0; usb_step(hc());
    assert(e.port_handoffs == 2 && !hc()->root_blocked[0]);
    group[1].quarantined=true;
    info=(struct usb_controller_inventory){.version=1,.size=sizeof(info)};
    assert(!usb_controller_snapshot(hc(),&info) && !info.ready_functions);
    assert(!usb_ehci_companion_ready(hc(),0));
    e.regs[0x64/4]=0x401; hc()->next_scan=0; usb_step(hc());
    assert(e.port_handoffs==2 && hc()->root_blocked[0] && hc()->ready);
    usb_host_quarantine(hc(), "routing_end");
    puts("PASS EHCI BIOS semaphore/SMI/capability-cycle failures; versioned inventory; missing companions/AMD refusal; low/full-speed hand-back/disconnect");
}
static void hub_tt(void)
{
    prepare_model(true); start_ehci(false); usb_step(hc());
    assert(hc()->device_count == 3 && hc()->devices[0].hub_ports);
    for (unsigned i=1;i<3;i++) {
        struct usb_device *d=&hc()->devices[i];
        assert(d->present && d->configured && !d->quarantined && !d->high_speed);
        assert(d->hid[0].active && !strcmp(d->reason,"hid_ready"));
    }
    unsigned packets=e.packets; hc()->next_scan=0; usb_step(hc());
    assert(e.packets-packets < 100 && hc()->device_count==3);
    assert(e.starts && e.completes);
    usb_host_quarantine(hc(), "hub_end");
    prepare_model(true); m.hub_status[0]=(m.hub_status[0]&~0x200u)|0x400; start_ehci(false); usb_step(hc());
    assert(hc()->device_count==3 && hc()->devices[1].configured && hc()->devices[1].high_speed);
    assert(hc()->devices[2].configured && !hc()->devices[2].high_speed && !hc()->devices[2].low_speed);
    usb_host_quarantine(hc(), "mixed_hub_end");
    puts("PASS EHCI high-speed hub with full/low-speed children: split control enumeration and mixed HS/LS");
}
static void split_budgets_recovery(void)
{
    prepare_model(true); start_ehci(true); usb_step(hc()); step_ehci(20);
    struct usb_device *hub=&hc()->devices[0], *mouse=&hc()->devices[2], *kbd=&hc()->devices[1];
    assert(hub->tt_think==8 && !hub->tt_multi && mouse->configured && kbd->configured);
    assert(!mouse->high_speed && !mouse->low_speed && kbd->low_speed);
    uint8_t b[64];
    /* Existing FS/LS endpoints share one TT. New requests cannot reserve
     * overlapping downstream microframes even with abundant HS bandwidth. */
    struct usb_hid *extra=&mouse->hid[1];
    hub->tt_think=32;
    *extra=(struct usb_hid){.endpoint=2,.packet=4,.interval=10};
    assert(hc()->ops->interrupt(mouse,extra,b,4)==-USB_EAGAIN);
    unsigned extra_at=USB_EHCI_HID_BASE+(2*USB_HID_MAX+1)*USB_EHCI_HID_STRIDE;
    assert(*(uint32_t *)(m.dma+extra_at+84)==((4u*8+160)*7/6+1)*84+32*84);
    struct usb_hid *overflow=&mouse->hid[2];
    *overflow=(struct usb_hid){.endpoint=3,.packet=4,.interval=10};
    assert(hc()->ops->interrupt(mouse,overflow,b,4)==-ENOSPC && !overflow->pipe_state);
    unsigned at=USB_EHCI_HID_BASE+2*USB_HID_MAX*USB_EHCI_HID_STRIDE;
    assert((*(uint32_t *)(m.dma+at+8)&255) && (*(uint32_t *)(m.dma+at+8)&0xff00));
    hc()->ops->interrupt_stop(mouse,extra);
    assert(hc()->ops->interrupt(mouse,overflow,b,4)==-ENOSPC);
    ehci_sleep(&e,2); /* cancel retains its TT budget until HC retirement */
    /* A multi-TT interface may reserve the same time on a distinct port. */
    hub->tt_multi=true;
    assert(hc()->ops->interrupt(mouse,overflow,b,4)==-USB_EAGAIN);
    hc()->ops->interrupt_stop(mouse,overflow);
    hub->tt_multi=false;
    for(unsigned mode=0;mode<4;mode++) {
        e.bulk_fault=true;
        mouse->storage=(struct usb_storage){.bound=true,.in=2,.out=3,.in_packet=64,.out_packet=64};
        e.bulk_toggles[0]=e.bulk_toggles[1]=0;
        unsigned cleared=e.tt_clear, reset=e.tt_reset, got=0;
        e.inject=mode==0 ? 0x48:mode==1 ? 0x40:0;
        e.nak=mode==2; e.clear_fail=mode==3;
        if(mode==3)e.inject=0x48;
        usb_host_take(hc());
        int err=usb_bulk(mouse,true,b,sizeof(b),&got);
        usb_host_drop(hc());
        assert(err<0 && e.tt_clear==cleared+1 && !e.tt_busy);
        assert(e.tt_reset==reset+(mode==3));
        assert(hc()->ready && !hub->quarantined && !mouse->quarantined);
        assert(e.clear_port==1);
        if(mode!=3)assert(e.clear_value==(0x8000u|(2u<<11)|(unsigned)mouse->address<<4|2));
        e.nak=e.clear_fail=false;
        usb_host_take(hc());assert(!usb_bulk(mouse,true,b,sizeof(b),&got) && got==sizeof(b));usb_host_drop(hc());
    }
    /* Control cleanup covers both directions. Multi-TT class requests use
     * port+1, while QHs always carry the physical downstream port. */
    hub->tt_multi=true;
    e.bulk_fault=false;e.control_fault=true;
    unsigned cleared=e.tt_clear; e.inject=0x48;
    usb_host_take(hc());assert(usb_control(kbd,0x80,6,0x100,0,b,8)<0);usb_host_drop(hc());
    assert(e.tt_clear==cleared+2 && e.clear_port==kbd->port+1u);
    /* Periodic failures reset the TT, never ClearTTBuffer. */
    cleared=e.tt_clear; unsigned reset=e.tt_reset;
    e.control_fault=false;e.periodic_fault=true;
    e.inject=0x48;
    for(unsigned n=0;n<40 && e.tt_reset==reset;n++)step_ehci(1);
    assert(e.tt_reset==reset+1 && e.tt_clear==cleared && hc()->ready);
    usb_host_quarantine(hc(),"tt_recovery_end");
    puts("PASS EHCI TT shared/multi-port budgets, SS/CS masks, STALL/error/timeout ClearTTBuffer, control directions, ResetTT fallback and periodic recovery");
}
static void admission_and_failed_tt(void)
{
    prepare_model(false);start_ehci(false);
    unsigned admitted=0;uint8_t b[64];
    for(unsigned i=0;i<USB_DEVICE_MAX;i++) {
        struct usb_device *d=&hc()->devices[i];
        *d=(struct usb_device){.host=hc(),.address=(uint8_t)(i+1),.high_speed=true};
        hc()->device_count=i+1;
        for(unsigned j=0;j<USB_HID_MAX;j++) {
            struct usb_hid *p=&d->hid[j];
            *p=(struct usb_hid){.endpoint=(uint8_t)(j+1),.packet=64,.interval=1};
            int err=hc()->ops->interrupt(d,p,b,8);
            if(err==-ENOSPC)assert(!p->pipe_state);
            else {assert(err==-USB_EAGAIN);admitted++;}
        }
    }
    assert(admitted==80000u/(64u*20+2000));
    usb_host_quarantine(hc(),"budget_end");
    prepare_model(true);start_ehci(false);usb_step(hc());step_ehci(20);
    struct usb_device *d=&hc()->devices[2];
    d->storage=(struct usb_storage){.bound=true,.in=2,.out=3,.in_packet=64,.out_packet=64};
    e.bulk_fault=e.clear_fail=e.tt_reset_fail=true;e.inject=0x48;
    unsigned got;usb_host_take(hc());assert(usb_bulk(d,true,b,sizeof(b),&got)<0);usb_host_drop(hc());
    assert(hc()->devices[0].quarantined && d->quarantined && hc()->ready);
    assert(e.tt_clear==1 && e.tt_reset==1 && (e.regs[0x24/4]&0x4000));
    usb_host_quarantine(hc(),"tt_failed_end");
    puts("PASS EHCI 80 us microframe budget exhausted without publication; failed TT reset isolates hub and restarts periodic service for healthy peers");
}
static void mixed_tree(void)
{
    prepare_model(true); start_ehci(true); usb_step(hc()); step_ehci(20);
    struct usb_device *stick=&hc()->devices[hc()->device_count++];
    *stick=(struct usb_device){.host=hc(),.address=4,.ep0=8,.parent=1,.port=2,
        .present=true,.configured=true};
    stick->storage=(struct usb_storage){.bound=true,.in=2,.out=3,.in_packet=64,.out_packet=64};
    assert(hc()->devices[1].low_speed && !hc()->devices[2].low_speed && !hc()->devices[2].high_speed);
    m.dev[2].report[2]=4;m.dev[2].report_len=8;
    m.dev[3].report[1]=3;m.dev[3].report[2]=5;m.dev[3].report_len=4;
    uint8_t b[192];unsigned got;
    e.bulk_ready_at=e.now+100;
    usb_host_take(hc());assert(!usb_bulk(stick,true,b,sizeof(b),&got) && got==sizeof(b));usb_host_drop(hc());
    assert(held[INPUT_KEY_A] && last_x==3 && last_y==5);
    for(unsigned i=0;i<sizeof(b);i++)assert(b[i]==0x5a);
    memset(b,0xa7,sizeof(b));usb_host_take(hc());
    assert(!usb_bulk(stick,false,b,sizeof(b),&got) && got==sizeof(b));usb_host_drop(hc());
    assert(e.starts && e.completes && hc()->ready && hc()->device_count==4);
    usb_host_quarantine(hc(),"mixed_tree_end");
    puts("PASS EHCI mixed TT tree: HS hub, LS keyboard, FS mouse and FS stick; input progresses during NAK'd split bulk IN/OUT");
}
static void idle_interrupts(void)
{
    prepare_model(false); e.regs[0x68/4]=0; m.dev[1].present=false;
    e.hid_interval=4; /* 1 ms hardware service period for the latency fixture */
    start_ehci(true); usb_step(hc());
    struct usb_host *h=hc();
    fs_lock_take(&h->transfer_lock);usb_hid_service(h);fs_lock_drop(&h->transfer_lock);
    assert(usb_hid_deadline(h)==UINT64_MAX && !h->irq_fallback);
    unsigned wakes=0; uint64_t irqs=h->interrupts, due=usb_hid_deadline(h);
    for(unsigned n=0;n<1000;n++) {
        ehci_sleep(&e,1);
        if(h->interrupts!=irqs || due<=e.now) {
            wakes++;irqs=h->interrupts;usb_hid_service(h);
            due=usb_hid_deadline(h);
        }
    }
    assert(!wakes && usb_hid_deadline(h)==UINT64_MAX);
    unsigned completed=e.completions;
    m.dev[0].report[2]=4;m.dev[0].report_len=8;
    for(unsigned n=0;n<USB_HID_LATENCY_LIMIT && !held[INPUT_KEY_A];n++) {
        ehci_sleep(&e,1);
        if(h->interrupts!=irqs || due<=e.now) {
            wakes++;irqs=h->interrupts;usb_hid_service(h);
            due=usb_hid_deadline(h);
        }
    }
    assert(held[INPUT_KEY_A] && e.completions==completed+1 && wakes<=3);
    usb_host_quarantine(h,"idle_end");
    puts("PASS EHCI HID idle: zero periodic worker wakes/1000 ms; one IOC plus retirement deadline delivers key within input budget");
}
static void irq_grace(void)
{
    prepare_model(false);start_ehci(true);
    struct usb_host *h=hc();
    assert(usb_hid_deadline(h)==UINT64_MAX);
    e.regs[0x24/4]|=1;
    usb_irq_completion_check(h);
    assert(h->irq_probe_pending && !h->irq_fallback);
    e.now++;usb_irq_completion_check(h);
    assert(!h->irq_fallback);
    assert(usb_host_irq(h) && !h->irq_probe_pending && h->completion_irq);
    usb_irq_completion_check(h);assert(!h->irq_fallback);
    e.regs[0x24/4]|=1;usb_irq_completion_check(h);
    e.now+=USB_IRQ_GRACE_MS;usb_irq_completion_check(h);
    assert(h->irq_fallback && !h->irq_probe_pending);
    usb_host_quarantine(h,"irq_grace_end");
    puts("PASS EHCI IOC qualification: ITC grace avoids false fallback; persistent unacknowledged completion proves lost IRQ");
}
static void stop_and_faults(void)
{
    for (unsigned fault=0;fault<6;fault++) {
        prepare_model(false); start_ehci(false);
        if (fault==1) e.halt_stuck=true;
        if (fault==2) e.bme_stuck=true;
        if (fault==3) {e.mask_stuck=true;e.regs[0x28/4]=1;}
        if (fault==4) e.host_error=true;
        if (fault==5) hc()->irq_status=0x10;
        usb_host_quarantine(hc(), "stop_fault");
        assert(hc()->quarantined && !hc()->ready && hc()->dma==m.dma);
        assert(leases[hc()->claims[2]].state==RS_QUARANTINED);
        if (!fault || fault==5) assert(!(e.config[1]&4) && !e.regs[0x60/4]);
        else assert(e.regs[0x60/4]&1); /* no companion reroute on uncertain stop */
    }
    puts("PASS EHCI controller stop: mask/halt/BME/HSE uncertainty retains arena/claims and routing");
}
static void irq_lease_loss(void)
{
    prepare_model(false); start_ehci(true); usb_step(hc()); step_ehci(6);
    unsigned command=e.regs[0x20/4], pci=e.config[1], cf=e.regs[0x60/4];
    registry_quarantine(hc()->claims[1],hc()->claim_gen[1]);
    usb_step(hc());
    assert(hc()->ready && !hc()->irq_attached && !e.regs[0x28/4]);
    assert(e.regs[0x20/4]==command && e.config[1]==pci && e.regs[0x60/4]==cf);
    m.dev[0].report[2]=4; m.dev[0].report_len=8; step_ehci(70);
    assert(held[INPUT_KEY_A] && hc()->ready);
    usb_host_quarantine(hc(),"lost_irq_end");
    puts("PASS EHCI IRQ lease loss: local mask, retained DMA/schedule/CF, HID continues by polling");
}
static void concurrent_hid_bulk(void)
{
    prepare_model(false); start_ehci(false); usb_step(hc()); step_ehci(6);
    struct usb_device *d=&hc()->devices[0];
    d->storage=(struct usb_storage){.bound=true,.in=2,.out=3,.in_packet=512,.out_packet=512};
    m.dev[0].report[2]=4; m.dev[0].report_len=8;
    e.bulk_ready_at=e.now+100;
    uint8_t data[1024]; unsigned got;
    usb_host_take(hc()); assert(!usb_bulk(d,true,data,sizeof(data),&got) && got==sizeof(data)); usb_host_drop(hc());
    assert(held[INPUT_KEY_A] && d->hid[0].toggle && hc()->ready);
    usb_host_quarantine(hc(),"concurrent_end");
    uint8_t config[]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,
                      7,5,0x81,2,0,2,0,7,5,2,2,0,2,0};
    struct usb_device descriptor_device={.high_speed=true};
    assert(!usb_parse_config(&descriptor_device,config,sizeof(config)));
    assert(descriptor_device.storage.in_packet==512 && descriptor_device.storage.out_packet==512);
    descriptor_device.high_speed=false;
    assert(usb_parse_config(&descriptor_device,config,sizeof(config))==-USB_EPROTO);
    puts("PASS EHCI concurrent HID while bulk NAKs, private DMA/mutex progress, speed-specific BOT endpoint negotiation");
}
static void long_interrupt_reports(void)
{
    prepare_model(false); start_ehci(false);
    struct usb_device *d=bulk_device(); m.dev[0].addr=1;
    for (unsigned index=0;index<USB_DEVICE_MAX*USB_HID_MAX;index++) {
        d=&hc()->devices[index/USB_HID_MAX];
        d->host=hc(); d->address=1; d->high_speed=true;
        struct usb_hid *p=&d->hid[index%USB_HID_MAX];
        *p=(struct usb_hid){.endpoint=1,.packet=64,.interval=1,.toggle=m.dev[0].in_toggle};
        unsigned at=USB_EHCI_HID_BASE+index*USB_EHCI_HID_STRIDE;
        uint8_t before[USB_EHCI_HID_STRIDE], after[USB_EHCI_HID_STRIDE];
        memcpy(before,m.dma+at-USB_EHCI_HID_STRIDE,sizeof(before));
        memcpy(after,m.dma+at+USB_EHCI_HID_STRIDE,sizeof(after));
        for (unsigned bytes=32;bytes<=64;bytes+=32) {
            uint8_t got[64]; memset(got,0xa5,sizeof(got));
            for (unsigned i=0;i<bytes;i++) m.dev[0].report[i]=(uint8_t)(i+index+1);
            m.dev[0].report_len=bytes;
            int n=-USB_EAGAIN;
            for (unsigned turns=0;turns<20 && n==-USB_EAGAIN;turns++) {
                n=hc()->ops->interrupt(d,p,got,bytes); ehci_sleep(&e,1);
            }
            assert(n==(int)bytes && !memcmp(got,m.dev[0].report,bytes));
            assert(!p->pipe_state && p->trace.retired==0 && p->trace.actual==(int)bytes);
            p->toggle^=1;
            for (unsigned i=bytes;i<64;i++) assert(got[i]==0xa5);
            assert(!memcmp(before,m.dma+at-USB_EHCI_HID_STRIDE,sizeof(before)));
            assert(!memcmp(after,m.dma+at+USB_EHCI_HID_STRIDE,sizeof(after)));
        }
        uint8_t got[65];
        assert(hc()->ops->interrupt(d,p,got,sizeof(got))==-USB_EPROTO);
        p->packet=32; assert(hc()->ops->interrupt(d,p,got,64)==-USB_EPROTO);
    }
    usb_host_quarantine(hc(),"long_reports_end");
    puts("PASS F1-104b EHCI: 32/64-byte reports in all 64 slots, previous/next guards, retirement, oversize/MPS refusal");
}
static void sleep_resume(void)
{
    prepare_model(false);start_ehci(false);
    static uint8_t schedule[USB_DMA_BYTES];memcpy(schedule,m.dma,sizeof(schedule));
    gen_t generation=hc()->generation;unsigned leases=lease_count;
    for(unsigned cycle=0;cycle<2;cycle++) {
        assert(!usb_host_suspend(hc()) && hc()->suspended && !hc()->ready);
        assert(!usb_host_resume(hc()) && hc()->ready && !hc()->suspended);
        assert(generation==hc()->generation && leases==lease_count && !memcmp(schedule,m.dma,sizeof(schedule)));
    }
    usb_host_quarantine(hc(),"sleep_test_end");
    puts("PASS G11B EHCI: retained periodic/async schedules and claims, two suspend/resume cycles");
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    sleep_resume(); ownership_and_admission(); enumeration_hid(); bulk_and_unlink(); periodic_and_faults(); hub_tt();
    split_budgets_recovery(); admission_and_failed_tt(); mixed_tree(); idle_interrupts(); irq_grace();
    stop_and_faults(); irq_lease_loss(); concurrent_hid_bulk();
    long_interrupt_reports();
    printf("PASS EHCI model suite (ASan+UBSan), host=%zu device=%zu storage=%zu arena=%u, inventory=%zu\n",
           sizeof(struct usb_host),sizeof(struct usb_device),sizeof(struct usb_storage),USB_DMA_BYTES,sizeof(struct usb_controller_inventory));
    return 0;
}
