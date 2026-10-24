/* Real loopback ICMP, no host raw socket privileges. */
static void g26b_tests(void);
static void raw_tests(void)
{
    CHECK(net_socket_create(&client,AF_INET,SOCK_RAW,IPPROTO_TCP)==-EPROTONOSUPPORT);
    CHECK(net_socket_create(&client,AF_INET,SOCK_RAW,0)==-EPROTONOSUPPORT);
    int fd=net_socket_create(&client,AF_INET,SOCK_RAW,IPPROTO_ICMP);CHECK(fd>=0);
    struct ciuki_net_socket_list snapshot;net_socket_snapshot(&snapshot);
    CHECK(snapshot.version==1 && snapshot.size==sizeof(snapshot) && snapshot.count==1 &&
        snapshot.sockets[0].pid==client.pid && snapshot.sockets[0].type==SOCK_RAW);
    CHECK(call(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_SOCKETS,8192,sizeof(snapshot),0)==0);
    memcpy(&snapshot,user+8192,sizeof(snapshot));CHECK(snapshot.count==1 && snapshot.sockets[0].type==SOCK_RAW);
    CHECK(call(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_SOCKETS,1,sizeof(snapshot),0)==-EFAULT);
    CHECK(call(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_SOCKETS,8192,sizeof(snapshot)-1,0)==-EINVAL);
    CHECK(call(CIUKI_SYS_NET_INFO,2,8192,sizeof(snapshot),0)==-EINVAL);
    unsigned length=4;int ttl=7;
    CHECK(!net_socket_option(&client,fd,1,IPPROTO_IP,IP_TTL,&ttl,&length,NULL));
    ttl=0;CHECK(!net_socket_option(&client,fd,0,IPPROTO_IP,IP_TTL,&ttl,&length,NULL) && ttl==7);
    ttl=256;CHECK(net_socket_option(&client,fd,1,IPPROTO_IP,IP_TTL,&ttl,&length,NULL)==-EINVAL);
    ttl=0;CHECK(net_socket_option(&client,fd,1,IPPROTO_IP,IP_TTL,&ttl,&length,NULL)==-EINVAL);
    uint8_t packet[12]={8,0,0,0,0x53,0x26,0,1,'w','o','o','f'};
    uint16_t sum=inet_chksum(packet,sizeof(packet));memcpy(packet+2,&sum,2);
    struct ciuki_sockaddr_in target=address(0);
    CHECK(net_socket_io(&client,fd,packet,sizeof(packet),0,&target,true,NULL)==sizeof(packet));
    struct ciuki_pollfd p={fd,POLLIN,0};
    int got=0;
    for(unsigned i=0;i<4 && !got;i++) {
        CHECK(net_poll(&client,&p,1,1000,NULL)==1);
        uint8_t received[96];struct ciuki_sockaddr_in peer;
        int n=net_socket_io(&client,fd,received,sizeof(received),0,&peer,false,NULL);
        CHECK(n>=32 && (received[0]>>4)==4 && received[9]==IPPROTO_ICMP);
        unsigned ip=(received[0]&15)*4;CHECK(ip+12<=(unsigned)n);
        if(received[ip]!=0)continue; /* raw sees the outbound request too */
        CHECK(!inet_chksum(received+ip,n-ip));
        CHECK(!memcmp(received+ip+4,packet+4,8));got=1;
    }
    CHECK(got);CHECK(!file_close(&client,fd));drain();
    net_socket_snapshot(&snapshot);CHECK(!snapshot.count);
    g26b_tests();
    puts("PASS G26 raw ICMP loopback/header/checksum/identifier/TTL/options/cleanup; read-only socket snapshot and marshalling");
}

static void g26b_tests(void)
{
    int a=net_socket_create(&client,AF_INET,SOCK_DGRAM,0),b=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(a>=0 && b>=0);
    struct ciuki_sockaddr_in to=address(23191);CHECK(!net_socket_bind(&client,b,&to,NULL));
    CHECK(net_socket_io(&client,a,"hello",5,0,&to,true,NULL)==5);
    struct ciuki_pollfd pollfd={b,POLLIN,0};CHECK(net_poll(&client,&pollfd,1,1000,NULL)==1);
    unsigned length=4,available=0;
    CHECK(!net_socket_option(&client,b,0,SOL_SOCKET,SO_RX_BYTES,&available,&length,NULL) && available==5);
    struct ciuki_socket_io io={.size=sizeof(io),.version=2,.fd=b,.buffer=16000,.bytes=2,.flags=MSG_PEEK,.reserved={17000,0}};
    memcpy(user+15000,&io,sizeof(io));CHECK(call(CIUKI_SYS_RECVFROM,15000,0,0,0)==2);
    unsigned flags;memcpy(&flags,user+17000,4);CHECK(flags==MSG_TRUNC && !memcmp(user+16000,"he",2));
    io.flags=0;io.bytes=0;memcpy(user+15000,&io,sizeof(io));CHECK(call(CIUKI_SYS_RECVFROM,15000,0,0,0)==0);
    memcpy(&flags,user+17000,4);CHECK(flags==MSG_TRUNC);
    CHECK(!net_socket_option(&client,b,0,SOL_SOCKET,SO_RX_BYTES,&available,&length,NULL) && !available);
    CHECK(!file_close(&client,a) && !file_close(&client,b));drain();
    struct ciuki_net_routes routes;CHECK(!call(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_ROUTES,8192,sizeof(routes),0));
    memcpy(&routes,user+8192,sizeof(routes));CHECK(routes.version==1 && routes.size==sizeof(routes) && routes.count<=4);
    struct ciuki_net_statistics stats;
    CHECK(!call(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_STATS,8192,sizeof(stats),0));
    memcpy(&stats,user+8192,sizeof(stats));CHECK(stats.version==1 && stats.size==sizeof(stats) && stats.udp.sent && stats.udp.received);
    struct ciuki_resolve_query q={.size=sizeof(q),.version=2,.action=CIUKI_RESOLVE_START,.name=9000,.name_bytes=10};
    memcpy(user+9000,"localhost",10);memcpy(user+10000,&q,sizeof(q));
    CHECK(!call(CIUKI_SYS_RESOLVE4,10000,0,0,0));memcpy(&q,user+10000,sizeof(q));CHECK(q.token && q.status==4);
    uint32_t token=q.token;q.name=q.name_bytes=0;q.action=CIUKI_RESOLVE_CANCEL;
    memcpy(user+10000,&q,sizeof(q));CHECK(!call(CIUKI_SYS_RESOLVE4,10000,0,0,0));
    q.action=CIUKI_RESOLVE_POLL;memcpy(user+10000,&q,sizeof(q));CHECK(call(CIUKI_SYS_RESOLVE4,10000,0,0,0)==-EINVAL);
    CHECK(token);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
    puts("PASS G26B: FIONREAD next datagram, peek/truncation/zero receive, core protocol counters, cancellable DNS token marshalling");
}

static void g26b_stream_available(int sender,int receiver)
{
    static bool checked;if(checked)return;checked=true;
    CHECK(net_socket_io(&client,sender,"hello world",11,0,NULL,true,NULL)==11);
    struct ciuki_pollfd p={receiver,POLLIN,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1);
    unsigned size=4,available=0;char data[12];
    CHECK(!net_socket_option(&client,receiver,0,SOL_SOCKET,SO_RX_BYTES,&available,&size,NULL) && available==11);
    CHECK(net_socket_io(&client,receiver,data,5,MSG_PEEK,NULL,false,NULL)==5 && !memcmp(data,"hello",5));
    CHECK(!net_socket_option(&client,receiver,0,SOL_SOCKET,SO_RX_BYTES,&available,&size,NULL) && available==11);
    CHECK(net_socket_io(&client,receiver,data,3,0,NULL,false,NULL)==3);
    CHECK(!net_socket_option(&client,receiver,0,SOL_SOCKET,SO_RX_BYTES,&available,&size,NULL) && available==8);
    CHECK(net_socket_io(&client,receiver,data,sizeof(data),0,NULL,false,NULL)==8);
    CHECK(!net_socket_option(&client,receiver,0,SOL_SOCKET,SO_RX_BYTES,&available,&size,NULL) && !available);
    cu_sleep(250);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
    puts("PASS G26B FIONREAD stream queued bytes, peek and partially consumed receive buffers");
}
