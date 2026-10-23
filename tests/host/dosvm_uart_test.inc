/* Full VM lifecycle/port/BIOS adapter with the established host transports.
 * SPDX-License-Identifier: GPL-2.0-only */
static void test_serial_uart(void)
{
    uint64_t saved_ticks=g_ticks; uint32_t saved_flags=cpu_flags; unsigned saved_progress=native_progress;
    struct dosvm *v=create(), *w=create(); const char *why=0;
    struct dosvm_key key=dosvm_key(v), other=dosvm_key(w);
    uint16_t bda[4]; CHECK(!dosvm_read(v,0x400,bda,sizeof(bda)));
    CHECK(bda[0]==0x3f8 && bda[1]==0x2f8 && bda[2]==0x3e8 && bda[3]==0x2e8);
    uint16_t equipment=0; CHECK(!dosvm_read(v,0x410,&equipment,2) && (equipment&0xe00)==0x800);
    /* INT11 and COM changes preserve a simultaneous LPT1 equipment count. */
    equipment|=0x4000;CHECK(!dosvm_write(v,0x410,&equipment,2));
    struct v86_frame eq={0};eq.tf.eax=0xaabb0000;eq.tf.eflags=0x247;
    CHECK(!dosvm_bios(v,&eq,0x11) && eq.tf.eax==0xaabb4822 && eq.tf.eflags==0x247);
    CHECK(!dosvm_uart_bind(key,3,-2,&why));
    eq.tf.eax=0;CHECK(!dosvm_bios(v,&eq,0x11) && eq.tf.eax==0x4622);
    uint16_t absent=1;CHECK(!dosvm_read(v,0x406,&absent,2) && !absent);
    uint32_t unmapped=0;CHECK(!dosvm_uart_port(v,0x2e8,1,false,&unmapped,0) && unmapped==0xff);
    CHECK(!dosvm_uart_bind(key,3,-1,&why));
    eq.tf.eax=0;CHECK(!dosvm_bios(v,&eq,0x11) && eq.tf.eax==0x4822);
    uint32_t value=0xa5; CHECK(!dosvm_uart_port(v,0x3ff,1,true,&value,0));
    value=0; CHECK(!dosvm_uart_port(w,0x3ff,1,false,&value,0) && !value);
    CHECK(!dosvm_uart_bind(key,0,0,&why) && host_com_owner[0]==CIUKI_COM_OWNER_VM);
    CHECK(dosvm_uart_bind(other,0,0,&why)==-EBUSY && why && w->uart[0].native==-1);
    CHECK(!dosvm_uart_bind(other,0,1,&why));
    uint32_t lease=v->uart[0].generation;
    CHECK(!dosvm_uart_bind(key,0,0,&why) && v->uart[0].generation==lease);
    value=3; CHECK(!dosvm_uart_port(v,0x3fb,1,true,&value,0));
    unsigned prior_baud=host_com_line[0].baud;
    value=0x83; CHECK(!dosvm_uart_port(v,0x3fb,1,true,&value,0));
    value=12; CHECK(!dosvm_uart_port(v,0x3f8,1,true,&value,0));
    value=0x0b; CHECK(!dosvm_uart_port(v,0x3fc,1,true,&value,0) && host_com_line[0].baud==prior_baud);
    value=3; CHECK(!dosvm_uart_port(v,0x3fb,1,true,&value,0) && host_com_line[0].baud==9600);
    value=0x55; CHECK(!dosvm_uart_port(v,0x3f8,1,true,&value,0));
    dosvm_uart_service(v,g_ticks); CHECK(host_com_tx[0]==1 && !host_com_tx[1]);
    host_com_busy=true; g_ticks++;
    value=0; CHECK(!dosvm_uart_port(v,0x3fd,1,false,&value,0) && !(value&0x40));
    host_com_busy=false; g_ticks++;
    CHECK(!dosvm_uart_port(v,0x3fd,1,false,&value,0) && (value&0x40));
    /* Native rejection must restore guest line state and report a local error. */
    uint8_t previous=v->uart[0].model.lcr;
    host_com_busy=true; value=0x1b;
    CHECK(!dosvm_uart_port(v,0x3fb,1,true,&value,0) && v->uart[0].model.lcr==previous);
    host_com_busy=false;
    CHECK(!dosvm_uart_bind(key,0,-1,&why) && !host_com_owner[0]);
    struct v86_frame f={0}; f.tf.eax=0x00e3; CHECK(!dosvm_bios(v,&f,0x14) && (f.tf.eax&0x6000)==0x6000);
    CHECK(v->uart[0].model.dll==12 && v->uart[0].model.lcr==3);
    f.tf.eax=0x0155; CHECK(!dosvm_bios(v,&f,0x14) && !(f.tf.eax&0x8000));
    g_ticks+=2; dosvm_uart_service(v,g_ticks); g_ticks+=2; dosvm_uart_service(v,g_ticks);
    uart16550_receive(&v->uart[0].model,0x5a,0,g_ticks*1000);
    f.tf.eax=0x0200; CHECK(!dosvm_bios(v,&f,0x14) && (uint8_t)f.tf.eax==0x5a && !(f.tf.eax&0x8000));
    uint8_t zero=0; CHECK(!dosvm_write(v,0x47c,&zero,1));
    f.tf.eax=0x0233; CHECK(!dosvm_bios(v,&f,0x14) && (f.tf.eax&0x8000) && (uint8_t)f.tf.eax==0x33);
    uint8_t second=1; CHECK(!dosvm_write(v,0x47c,&second,1));
    uint64_t wait_begin=g_ticks; cpu_flags=V86_IF; f.tf.eax=0x0233;
    CHECK(!dosvm_bios(v,&f,0x14) && (f.tf.eax&0x8000) && g_ticks-wait_begin==55);
    f.tf.edx=4; f.tf.eax=0x0300; CHECK(!dosvm_bios(v,&f,0x14) && (f.tf.eax&0x8000));
    /* Shared COM1/COM3 line creates one edge until BOTH UARTs deassert. */
    v->uart[0].model.ier=2; v->uart[0].model.mcr=8; v->uart[0].model.thre=true;
    v->uart[2].model.ier=2; v->uart[2].model.mcr=8; v->uart[2].model.thre=true;
    dosvm_uart_service(v,g_ticks); CHECK(v->uart_irq[1] && (v->monitor.pic[0].irr&16));
    value=0; CHECK(!dosvm_uart_port(v,0x3fa,1,false,&value,0) && value==2 && v->uart_irq[1]);
    CHECK(!dosvm_uart_port(v,0x3ea,1,false,&value,0) && !v->uart_irq[1]);
    /* An IRQ3 virtual peer refuses UART interrupt activation locally. */
    v->ports[0].first=0x300; v->ports[0].handler=dosvm_uart_port;
    value=8; CHECK(!dosvm_uart_port(v,0x2fc,1,true,&value,0));
    value=2; CHECK(!dosvm_uart_port(v,0x2f9,1,true,&value,0) && !v->uart[1].model.ier);
    memset(&v->ports[0],0,sizeof(v->ports[0]));
    CHECK(!dosvm_uart_bind(key,1,2,&why));
    CHECK(!dosvm_network_irq_available(v,3));
    CHECK(!dosvm_uart_bind(key,1,-1,&why));
    CHECK(dosvm_network_irq_available(v,3));
    CHECK(!dosvm_uart_bind(key,0,0,&why));
    v->started=true; CHECK(dosvm_uart_bind(key,0,-1,&why)==-EBUSY); v->started=false;
    CHECK(!dosvm_stop(key,DOSVM_TERMINATE));
    unsigned tx=host_com_tx[0]; dosvm_uart_service(v,g_ticks+5); CHECK(host_com_tx[0]==tx);
    value=0x55; CHECK(dosvm_uart_port(v,0x3f8,1,true,&value,0)==-ENODEV && host_com_tx[0]==tx);
    destroy(v,true); CHECK(!host_com_owner[0]);
    destroy(w,true); CHECK(!host_com_owner[1]); empty();
    CHECK(dosvm_uart_bind(key,0,0,&why)==-EINVAL);
    struct dosvm_config config={.commitment=DOSVM_LOW_CAP,.serial={1,0,0,0}};
    CHECK(!dosvm_create(&config,&v) && v->uart[0].native==0 && host_com_owner[0]==CIUKI_COM_OWNER_VM);
    CHECK(dosvm_create(&config,&w)==-EBUSY && !w && host_com_owner[0]==CIUKI_COM_OWNER_VM);
    destroy(v,true); empty(); CHECK(!host_com_owner[0]);
    config.serial[0]=9; CHECK(dosvm_create(&config,&v)==-EINVAL && !v); empty();
    g_ticks=saved_ticks; cpu_flags=saved_flags; native_progress=saved_progress;
    puts("VM serial: BDA, COM1-4, leases, INT14, IRQ aggregate, peer conflict, stop and teardown PASS");
}
