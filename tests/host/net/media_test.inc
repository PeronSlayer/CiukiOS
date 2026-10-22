/* Register-level forced media, auto return, actual link status and failures. */
static void test_media_options(void)
{
    struct model h;struct netif *d=&devices[0];model_setup(&h,d,M_E100);device_count=1;
    CHECK(!model_attach(&h,d));
    struct ciuki_net_control r={.index=0,.config={.enabled=1}};
    for(unsigned speed=10;speed<=100;speed+=90)for(unsigned duplex=1;duplex<=2;duplex++){
        r.config.speed=speed;r.config.duplex=duplex;CHECK(!net_control_driver(0,&r,true));
        CHECK((h.phy_control&0x3100)==((speed==100 ? 0x2000u : 0)|(duplex==2 ? 0x100u : 0)));
        CHECK(d->driver->link_state(d)==NET_LINK_UP && d->speed==speed && d->duplex==duplex);
    }
    r.config.speed=r.config.duplex=0;CHECK(!net_control_driver(0,&r,true) && (h.phy_control&0x1200)==0x1200);
    h.mii_status|=0x20;h.phy_peer=0x100;CHECK(d->driver->link_state(d)==NET_LINK_UP && d->speed==100 && d->duplex==2);
    CHECK(!net_control_driver(0,&r,false) && r.media_caps==7 && r.irq==11 && r.pci_vendor==0x8086 && r.io_base==0x1000);
    h.mdi_stuck=true;r.config.speed=10;r.config.duplex=1;CHECK(net_control_driver(0,&r,true)<0 && !d->configured_speed);h.mdi_stuck=false;
    r.config.speed=r.config.duplex=0;r.config.enabled=0;CHECK(!net_control_driver(0,&r,true) && d->administratively_down);
    uint8_t frame[60]={0};CHECK(netif_transmit(d,d->generation,frame,sizeof(frame))==-19 && !d->quarantined);
    struct net_packet packet;CHECK(!netif_pop(d,&packet));
    r.config.enabled=1;CHECK(!net_control_driver(0,&r,true) && !d->administratively_down);
    CHECK(!netif_stop(d));
    struct second_model second;second_setup(&second,d,S_E1000);CHECK(!second_attach(&second,d));
    for(unsigned speed=10;speed<=100;speed+=90)for(unsigned duplex=1;duplex<=2;duplex++){
        r.config.speed=speed;r.config.duplex=duplex;CHECK(!net_control_driver(0,&r,true));
        CHECK((second.phy_regs[0]&0x3140)==((speed==100 ? 0x2000u : 0)|(duplex==2 ? 0x100u : 0)));
        CHECK(!(second.phy_regs[16]&0x60));
        CHECK((second.phy_regs[16]&0x800) && (second.phy_regs[20]&0x70)==0x70);
        uint32_t ctrl=second.intel[0];CHECK((ctrl&0x1800)==0x1800 && ((ctrl>>8)&3)==(speed==100) && !!(ctrl&1)==(duplex==2));
    }
    r.config.speed=r.config.duplex=0;CHECK(!net_control_driver(0,&r,true) && !(second.intel[0]&0x1800) && (second.phy_regs[0]&0x1200)==0x1200);
    CHECK((second.phy_regs[16]&0x60)==0x60);
    d->pci.device=0x100f;r.config.speed=100;r.config.duplex=2;CHECK(!net_control_driver(0,&r,true));
    r.config.speed=r.config.duplex=0;CHECK(!net_control_driver(0,&r,true));
    second.intel[2]=2|0x20;CHECK(d->driver->link_state(d)==NET_LINK_UP);
    CHECK(!net_control_driver(0,&r,false) && r.media_caps==CIUKI_NET_MEDIA_AUTO);
    r.config.speed=10;r.config.duplex=1;CHECK(net_control_driver(0,&r,true)==-EOPNOTSUPP);
    second.intel[2]=2|1|0x80;CHECK(d->driver->link_state(d)==NET_LINK_UP && d->speed==1000 && d->duplex==2);
    second.mii_stuck=true;r.config.speed=100;r.config.duplex=2;CHECK(net_control_driver(0,&r,true)<0 && !d->configured_speed);second.mii_stuck=false;
    second.phy_reset_stuck=true;CHECK(net_control_driver(0,&r,true)==-110);second.phy_reset_stuck=false;
    CHECK(!netif_stop(d));
    model_setup(&h,d,M_NE);CHECK(!model_attach(&h,d));r.config.speed=10;r.config.duplex=1;
    CHECK(net_control_driver(0,&r,true)==-EOPNOTSUPP);CHECK(!net_control_driver(0,&r,false) && r.media_caps==CIUKI_NET_MEDIA_AUTO);
    CHECK(!netif_stop(d));device_count=0;
    puts("PASS f2-100 media: e100/e1000 10/100 half/full, auto restoration, negotiated status, MDI failures, resources/counters and unsupported-driver refusal");
}
