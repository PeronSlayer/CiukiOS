/* Real loopback ICMP, no host raw socket privileges. */
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
    puts("PASS G26 raw ICMP loopback/header/checksum/identifier/TTL/options/cleanup; read-only socket snapshot and marshalling");
}
