/* f6-24: actual core timers, public SO_ERROR, and protected summaries. */
static void expire_syn(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<300;i++)tcp_slowtmr();
}
static void refuse_syn(void *unused)
{
    (void)unused;
    CHECK(output_bytes>=54 && output_frame[23]==6 && (output_frame[47]&2));
    tcp_reply(output_frame,0,get32(output_frame+38)+1,0x14,NULL,0);
}
static void f624_tcp_tests(void)
{
    cu_host_record=record_line;
    unsigned from=records_now();
    struct ciuki_sockaddr_in a={.family=AF_INET,.address=PP_HTONL(0xc633640a),.port=PP_HTONS(443)};
    int fd=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(fd>=0);
    CHECK(net_socket_connect(&client,fd,&a,NULL)==-EINPROGRESS);
    CHECK(tcpip_callback_wait(expire_syn,NULL)==ERR_OK);
    struct ciuki_pollfd p={fd,POLLOUT,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1 && (p.revents&POLLERR));
    int error=0;unsigned bytes=4;
    CHECK(!net_socket_option(&client,fd,0,SOL_SOCKET,SO_ERROR,&error,&bytes,NULL) && error==ETIMEDOUT);
    CHECK(!file_close(&client,fd));drain();
    a.port=PP_HTONS(80);fd=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(fd>=0);
    CHECK(net_socket_connect(&client,fd,&a,NULL)==-EINPROGRESS);
    CHECK(tcpip_callback_wait(refuse_syn,NULL)==ERR_OK);
    p=(struct ciuki_pollfd){fd,POLLOUT,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1 && (p.revents&POLLERR));
    CHECK(!net_socket_option(&client,fd,0,SOL_SOCKET,SO_ERROR,&error,&bytes,NULL) && error==ECONNREFUSED);
    CHECK(!file_close(&client,fd));drain();
    cu_host_clock_offset(60000);watch_pass();
    CHECK(find_record(from,"[net] tcp connect ","window_ms=10000")>=0);
    CHECK(find_record(from,"[net] tcp timeout ","port=443")>=0);
    CHECK(find_record(from,"[net] tcp reset ","port=80")>=0);
    unsigned records_after=count_records(from,"[net] tcp ");
    watch_pass();CHECK(count_records(from,"[net] tcp ")==records_after);
    cu_host_clock_offset(0);
    cu_host_record=NULL;
    puts("PASS f6-24 SYN retry exhaustion -> ETIMEDOUT, RST -> ECONNREFUSED; bounded TCP summaries");
}
