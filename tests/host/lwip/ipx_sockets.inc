/* Native fd/request/poll integration for IPX and both SPX types. */
static struct ciuki_sockaddr_in ipx_carrier(const struct ciuki_sockaddr_ipx *a)
{ struct ciuki_sockaddr_in out;memcpy(&out,a,16);return out; }
static void ipx_socket_tests(void)
{
    extern void cu_ipx_test_busy(unsigned);
    extern unsigned cu_ipx_test_wire(void);
    extern int cu_ipx_test_dos_only(void);
    CHECK(!cu_ipx_test_dos_only());
    net_ipx_frame_publish(CIUKI_IPX_FRAME_II); /* Deliberately stale bridge observation is still LLC. */
    cu_ipx_test_busy(3); /* Owner contention is retried, never exposed as errno. */
    int a=net_socket_create(&client,AF_IPX,SOCK_DGRAM|SOCK_NONBLOCK,NSPROTO_IPX+4);
    int b=net_socket_create(&client,AF_IPX,SOCK_DGRAM|SOCK_NONBLOCK,NSPROTO_IPX+4);CHECK(a>=0 && b>=0);
    struct ciuki_sockaddr_ipx any={.family=AF_IPX};struct ciuki_sockaddr_in address=ipx_carrier(&any),local;
    cu_ipx_test_busy(3);
    CHECK(!net_socket_bind(&client,a,&address,NULL));CHECK(!net_socket_bind(&client,b,&address,NULL));
    cu_ipx_test_busy(3);CHECK(!net_socket_name(&client,b,0,&local,NULL));
    CHECK(net_socket_io(&client,a,"IPX",3,0,&local,true,NULL)==3);
    struct ciuki_pollfd p={b,POLLIN,0};CHECK(net_poll(&client,&p,1,0,NULL)==1 && (p.revents&POLLIN));
    char data[16];unsigned out;CHECK(net_socket_receive(&client,b,data,2,0,&address,NULL,&out)==2 && out==MSG_TRUNC && !memcmp(data,"IP",2));
    struct ciuki_ipx_address_data info={0};unsigned length=sizeof(info);
    CHECK(!net_socket_option(&client,a,0,NSPROTO_IPX,IPX_ADDRESS,&info,&length,NULL) && !info.wan && info.maxpkt==1462 && length==24);
    struct ciuki_sockaddr_ipx remote={.family=AF_IPX,.node={2,0,0,0,0,99},.socket=0x326c};
    struct ciuki_sockaddr_in wire=ipx_carrier(&remote);
    CHECK(net_socket_io(&client,a,"net",3,0,&wire,true,NULL)==3 && cu_ipx_test_wire()==IPX_II);
    for(unsigned setting=0;setting<=CIUKI_IPX_FRAME_AUTO;setting++) {
        net_ipx_frame_publish(setting);
        CHECK(net_socket_io(&client,a,"net",3,0,&wire,true,NULL)==3);
        CHECK(cu_ipx_test_wire()==ipx_frame_wire(setting));
    }
    extern void cu_ipx_test_network(uint32_t);
    cu_ipx_test_network(0x04030201);length=sizeof(info);
    CHECK(!net_socket_option(&client,a,0,NSPROTO_IPX,IPX_ADDRESS,&info,&length,NULL) && info.netnum[0]==1 && info.netnum[3]==4);
    CHECK(!net_socket_name(&client,b,0,&local,NULL));struct ciuki_sockaddr_ipx learned;
    memcpy(&learned,&local,sizeof(learned));CHECK(learned.network[0]==1 && learned.network[3]==4);
    CHECK(!file_close(&client,a));CHECK(!file_close(&client,b));drain();
    for(unsigned type=SOCK_STREAM;type<=SOCK_SEQPACKET;type+=4) {
        int listener=net_socket_create(&client,AF_IPX,(int)type|SOCK_NONBLOCK,NSPROTO_SPX);CHECK(listener>=0);
        CHECK(!net_socket_bind(&client,listener,&address,NULL));CHECK(!net_socket_listen(&client,listener,4,NULL));
        CHECK(!net_socket_name(&client,listener,0,&local,NULL));
        a=net_socket_create(&client,AF_IPX,(int)type|SOCK_NONBLOCK,NSPROTO_SPX);CHECK(a>=0);
        CHECK(!net_socket_connect(&client,a,&local,NULL));p=(struct ciuki_pollfd){listener,POLLIN,0};CHECK(net_poll(&client,&p,1,0,NULL)==1);
        b=net_socket_accept(&client,listener,&address,NULL);CHECK(b>=0);
        CHECK(net_socket_io(&client,a,"SPX",3,0,NULL,true,NULL)==3);
        p=(struct ciuki_pollfd){b,POLLIN,0};CHECK(net_poll(&client,&p,1,0,NULL)==1);
        CHECK(net_socket_receive(&client,b,data,sizeof(data),0,NULL,NULL,&out)==3 && !memcmp(data,"SPX",3));
        struct ciuki_socket_linger linger={1,0};length=sizeof(linger);
        CHECK(!net_socket_option(&client,a,1,SOL_SOCKET,SO_LINGER,&linger,&length,NULL));
        CHECK(!net_socket_option(&client,b,1,SOL_SOCKET,SO_LINGER,&linger,&length,NULL));
        CHECK(!file_close(&client,a));CHECK(!file_close(&client,b));CHECK(!file_close(&client,listener));drain();
        any=(struct ciuki_sockaddr_ipx){.family=AF_IPX};address=ipx_carrier(&any);
    }
    puts("PASS f6-27 DOS-only RIP/fallback, stale bridge/manual first open, live frame changes, native socket/request/fd/poll IPX and SPX stream/seqpacket");
}
