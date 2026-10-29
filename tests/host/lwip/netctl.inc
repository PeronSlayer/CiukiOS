/* Exercise syscall 90 against the real core and a durable-byte-store seam. */
static struct ciuki_net_config netctl_saved[2];
static int netctl_storage_error;
static bool netctl_saved_ready, netctl_exhaust_on_apply;
static sys_sem_t netctl_sems[128];static unsigned netctl_held;
static struct ciuki_net_config netctl_driver_config;
static char netctl_disk[NET_SETTINGS_BYTES];
void net_settings_get(unsigned index,struct ciuki_net_config *c)
{ if (netctl_saved_ready) *c=netctl_saved[index]; else net_config_default(c); }
int net_settings_save(unsigned index,const struct ciuki_net_config *c)
{
    if (netctl_storage_error) return netctl_storage_error;
    if (!netctl_saved_ready) for(unsigned i=0;i<2;i++)net_config_default(&netctl_saved[i]);
    netctl_saved[index]=*c;
    size_t n=net_config_format(netctl_saved,NULL,netctl_disk,sizeof(netctl_disk)); CHECK(n);
    CHECK(!net_config_parse(netctl_disk,n,netctl_saved,NULL));netctl_saved_ready=true;return 0;
}
int net_control_driver(unsigned index,struct ciuki_net_control *r,bool apply)
{
    if(index>=2)return -ENODEV;
    if(apply) {
        netctl_driver_config=r->config;
        if(netctl_exhaust_on_apply){netctl_exhaust_on_apply=false;
            while(netctl_held<128 && sys_sem_new(&netctl_sems[netctl_held],0)==ERR_OK)netctl_held++;
        }
        return 0;
    }
    r->generation=100+index;r->count=2;r->media_caps=7;r->link=2;r->state=1;r->irq=11;r->io_base=0x1000;
    snprintf(r->name,sizeof(r->name),"en%u",index);snprintf(r->driver,sizeof(r->driver),"e100");return 0;
}
static int ctl(unsigned operation,struct ciuki_net_control *record)
{
    memcpy(user+12000,record,sizeof(*record));
    int e=call(CIUKI_SYS_NETCTL,operation,12000,sizeof(*record),0);
    if(!e)memcpy(record,user+12000,sizeof(*record));return e;
}
static void security_socket_tests(void)
{
    int fd=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ciuki_net_socket_list list;
    security_fixture_identity(&client,1000,1,false,0);
    net_socket_snapshot(&list);CHECK(list.count==1);
    security_fixture_identity(&client,1001,1,false,0);
    net_socket_snapshot(&list);CHECK(!list.count);
    struct net_socket_ref ref={0};CHECK(net_socket_capture(&client,fd,&ref)==-EACCES && !ref.object);
    struct ciuki_pollfd foreign={.fd=fd,.events=POLLIN|POLLOUT};
    CHECK(net_poll(&client,&foreign,1,0,NULL)==1 && foreign.revents==POLLNVAL);
    security_fixture_identity(&client,1000,2,false,0);
    net_socket_snapshot(&list);CHECK(!list.count);
    security_fixture_identity(&client,500,2,true,0);
    net_socket_snapshot(&list);CHECK(list.count==1);
    security_fixture_identity(&client,0,0,true,0);
    int system=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(system>=0);
    security_fixture_identity(&client,500,2,true,0);
    net_socket_snapshot(&list);CHECK(list.count==1);
    security_fixture_identity(&client,0,0,true,0);
    net_socket_snapshot(&list);CHECK(list.count==2);
    CHECK(!file_close(&client,system) && !file_close(&client,fd));drain();
    security_fixture_identity(&client,1000,1,true,0);
    puts("H05 socket scope: uid/session isolation, SYSTEM details hidden from administrator, transferred fd denied PASS");
}
static void netctl_tests(void)
{
    security_socket_tests();
    struct ciuki_net_control r={.version=1,.size=sizeof(r)};
    CHECK(!ctl(CIUKI_NETCTL_GET,&r)); client.network_capability=true;
    CHECK(!ctl(CIUKI_NETCTL_GET,&r) && r.count==2 && r.config.dhcp && r.address==PP_HTONL(0xc0000264));
    CHECK(r.ipx_frame_type==CIUKI_IPX_FRAME_AUTO);
    r.generation++;CHECK(ctl(1,&r)==-EBUSY);r.generation--;
    r.version=2;CHECK(ctl(0,&r)==-EOPNOTSUPP);r.version=1;
    r.reserved[0]=1;CHECK(ctl(0,&r)==-EINVAL);r.reserved[0]=0;
    CHECK(ctl(9,&r)==-EINVAL);
    CHECK(call(CIUKI_SYS_NETCTL,1,1,sizeof(r),0)==-EFAULT);
    struct ciuki_net_config c=r.config;c.dhcp=0;c.address=PP_HTONL(0xc000026e);c.mask=PP_HTONL(0xffffff00);c.gateway=PP_HTONL(0xc0000201);
    c.dns[0]=PP_HTONL(0xc0000201);c.dns[1]=PP_HTONL(0xc0000202);c.dns[2]=PP_HTONL(0xc0000203);
    strcpy(c.hostname,"ciuki-test");strcpy(c.domain,"example.test");
    r.config=c;r.config.mask=PP_HTONL(0xff00ff00);CHECK(ctl(1,&r)==-EINVAL);
    r.config=c;r.config.gateway=PP_HTONL(0xc0000301);CHECK(ctl(1,&r)==-EINVAL);
    r.config=c;r.config.address=PP_HTONL(0xe0000001);CHECK(ctl(1,&r)==-EINVAL);
    r.config=c;memset(r.config.hostname,'x',sizeof(r.config.hostname));CHECK(ctl(1,&r)==-EINVAL);
    r.config=c;r.config.dns[0]=0;CHECK(ctl(1,&r)==-EINVAL);
    r.config=c;r.config.speed=100;r.config.duplex=0;CHECK(ctl(1,&r)==-EINVAL);
    r.config=c;CHECK(!ctl(1,&r));CHECK(r.address==c.address && r.mask==c.mask && r.gateway==c.gateway);
    CHECK(!memcmp(r.dns,c.dns,sizeof(c.dns)) && !memcmp(&netctl_saved[0],&c,sizeof(c)));
    char broken[NET_SETTINGS_BYTES];strcpy(broken,netctl_disk);broken[30]^=1;
    struct ciuki_net_config parsed[2];CHECK(net_config_parse(broken,strlen(broken),parsed,NULL)==-EINVAL);
    CHECK(net_config_parse(netctl_disk,strlen(netctl_disk)-1,parsed,NULL)==-EINVAL);
    CHECK(!net_resolve_override("host.example.test",PP_HTONL(0xc0000277)));
    uint32_t ip;int status;CHECK(!net_resolve(&client,"host",&ip,&status) && !status && ip==PP_HTONL(0xc0000277));
    CHECK(!net_resolve_override("host.example.test",0));
    CHECK(ctl(3,&r)==-EINVAL);
    netctl_exhaust_on_apply=true;r.config.speed=100;r.config.duplex=2;CHECK(ctl(1,&r)==-ENOMEM);
    while(netctl_held)sys_sem_free(&netctl_sems[--netctl_held]);
    CHECK(!netctl_driver_config.speed && !ctl(0,&r) && r.address==c.address);
    netctl_storage_error=-EIO;r.config.address=PP_HTONL(0xc0000270);CHECK(ctl(1,&r)==-EIO);netctl_storage_error=0;
    CHECK(!ctl(0,&r) && r.address==c.address && r.config.address==c.address);
    r.config.enabled=0;CHECK(!ctl(1,&r) && !r.address && !r.config.enabled);
    r.config.enabled=1;CHECK(!ctl(1,&r) && r.address==c.address);
    /* Persist a second port's static intent, then attach it: no DHCP packet
     * may precede its static address, even with carrier already present. */
    CHECK(!net_settings_save(1,&c));unsigned discovers=dhcp_discover;
    const uint8_t mac[6]={2,0,0,0,0,3};CHECK(!net_stack_attach(1,mac,true));
    struct ciuki_net_control other={.version=1,.size=sizeof(other),.index=1};
    CHECK(!ctl(0,&other) && other.address==c.address && !other.config.dhcp && dhcp_discover==discovers);
    net_config_default(&r.config);CHECK(!ctl(1,&r));retry_acquisition();CHECK(!ctl(0,&r) && r.config.dhcp && r.address);
    CHECK(!ctl(3,&r) && !r.address);discovers=dhcp_discover;
    for(unsigned i=0;i<10;i++)net_stack_link(0,true);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
    CHECK(dhcp_discover==discovers);CHECK(!ctl(2,&r));retry_acquisition();CHECK(!ctl(0,&r) && r.address);
    client.network_capability=false;CHECK(!ctl(0,&r));CHECK(!live_pins);
    client.dhcp_capability=true;
    security_fixture_identity(&client,1001,2,false,0);
    struct ciuki_net_config before=netctl_saved[0];
    CHECK(!ctl(CIUKI_NETCTL_GET,&r) && ctl(CIUKI_NETCTL_SET,&r)==-EPERM);
    CHECK(ctl(CIUKI_NETCTL_RELEASE,&r)==-EPERM && ctl(CIUKI_NETCTL_RENEW,&r)==-EPERM);
    CHECK(!memcmp(&before,&netctl_saved[0],sizeof(before)));
    security_fixture_identity(&client,501,3,false,0);
    CHECK(!ctl(CIUKI_NETCTL_GET,&r) && ctl(CIUKI_NETCTL_SET,&r)==-EPERM);
    CHECK(ctl(CIUKI_NETCTL_RELEASE,&r)==-EPERM && ctl(CIUKI_NETCTL_RENEW,&r)==-EPERM);
    security_fixture_identity(&client,1000,1,true,0);
    client.dhcp_capability=false;
    CHECK(!ctl(CIUKI_NETCTL_RELEASE,&r) && !r.address);
    CHECK(!ctl(CIUKI_NETCTL_RENEW,&r));retry_acquisition();
    puts("PASS H05 network credentials: public GET; Standard/Guest SET/RENEW/RELEASE denied unchanged; administrator DHCP allowed without path flags");
    puts("PASS f2-100 netctl: capability/copies, validation, static/DHCP, three DNS, search domain, save rollback, administrative down/up, release/renew, persisted boot before DHCP");
}
