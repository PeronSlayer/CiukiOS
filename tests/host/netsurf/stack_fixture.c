/* Scripted HTTP/BearSSL peer on the f6-10 synthetic Ethernet netif.
 * Reuses the existing kernel host seam, never a host socket or QEMU.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <bearssl.h>
#include <zlib.h>
#define main f6_10_unused_main
#define net_stack_transmit f6_10_transmit
#include "../lwip/stack_test.c"
#undef main
#undef net_stack_transmit
static const char *fixture_mode;
static br_x509_certificate server_certificates[12];
static unsigned server_depth=1;
static unsigned char *intermediate_data;
static _Atomic bool lookup_entered,lookup_allow;
static _Thread_local bool blocked_lookup_returned;
static br_skey_decoder_context server_key;
static unsigned char *certificate_data,*key_data;
static _Atomic unsigned http_requests,connections_seen,loop_requests;
static bool fixture_offline;
static bool send_blocked;
static unsigned blocked_sends;
static unsigned incoming_injection,injected_alerts,injected_handshakes;
static int injected_fd=-1;
static bool injected_closed,injected_output;
static const bool *injected_transmitted;
void fixture_unsolicited(void);
static void inject_hello_request(void *);
struct peer {
    unsigned port;
    uint32_t next,ack;
    unsigned char frame[54];
    char request[CIUKI_SOCKET_IO_MAX*3+1];size_t request_bytes;
    unsigned char response[40000];size_t response_bytes,response_sent;
    bool tls,waiting,close_after,unsolicited,hold_record,reset_after;
    size_t record_prefix;
    br_ssl_server_context *server;
    unsigned char *io;
};
static struct peer peers[16];
static void response(struct peer *p)
{
    char method[16],path[4097];CHECK(sscanf(p->request,"%15s %4096s",method,path)==2);
    if(!strcmp(fixture_mode,"browser-blocked")){
        CHECK(!strcmp(path,"/page")||!strcmp(path,"/favicon.ico"));
        CHECK(strstr(p->request,"\r\nHost: 192.0.2.1:8080\r\n"));
    }
    http_requests++;
    /* Lose B's response after accepting B on the admitted pooled transport.
     * GET may retry once; POST must never be replayed. A stays reusable. */
    if(http_requests==2 && (!strcmp(fixture_mode,"stale-retry")||!strcmp(fixture_mode,"post-no-replay")||
        !strcmp(fixture_mode,"tls-stale-retry")||!strcmp(fixture_mode,"tls-post-no-replay")))p->reset_after=true;
    if(p->tls){const char *sni=br_ssl_engine_get_server_name(&p->server->eng);CHECK(sni && !strcmp(sni,"fixture.example"));}
    if(!strcmp(method,"POST")){
        CHECK(strstr(p->request,"Content-Type: application/x-www-form-urlencoded\r\n"));
        CHECK(strstr(p->request,"\r\n\r\ndog=Ciuki&step=M2"));
    }
    if(!strcmp(path,"/loop"))loop_requests++;
    const char *wire="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nCiuki";
    if(!strcmp(path,"/stale"))p->close_after=true;
    if(!strcmp(path,"/unsolicited"))p->unsolicited=true;
    if(!strcmp(path,"/chunked"))wire="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;x=y\r\nCi\r\n3\r\nuki\r\n0\r\nX-Test: end\r\n\r\n";
    else if(!strcmp(path,"/tls-truncated")){wire="HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nCiuki";p->close_after=true;}
    else if(!strcmp(path,"/truncated")){wire="HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nCiuki";p->close_after=true;}
    else if(!strcmp(path,"/slow"))wire="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    else if(!strcmp(path,"/loop"))wire="HTTP/1.1 302 Loop\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n";
    else if(!strncmp(path,"/redirect",9))wire="HTTP/1.1 302 Redirect\r\nLocation: /cookie\r\nSet-Cookie: hop=one; Path=/; HttpOnly\r\nSet-Cookie: secure=hidden; Path=/; Secure\r\nSet-Cookie: path=hidden; Path=/other\r\nSet-Cookie: domain=hidden; Domain=other.example; Path=/\r\nSet-Cookie: expired=hidden; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/\r\nContent-Length: 0\r\n\r\n";
    else if(!strcmp(path,"/upgrade")){
        CHECK(!p->tls);
        wire="HTTP/1.1 302 Redirect\r\nLocation: https://fixture.example:8443/tls-cookie-start\r\nContent-Length: 0\r\n\r\n";
    } else if(!strcmp(path,"/tls-cookie-start")){
        CHECK(p->tls);
        wire="HTTP/1.1 302 Redirect\r\nLocation: /tls-cookie-end\r\nSet-Cookie: tls_session=Ciuki; Path=/; Secure; HttpOnly\r\nContent-Length: 0\r\n\r\n";
    }
    if(!strncmp(path,"/cookie-cap/",12)){
        unsigned hop=(unsigned)strtoul(path+12,NULL,10);CHECK(hop<3);
        size_t used=(size_t)snprintf((char *)p->response,sizeof(p->response),"HTTP/1.1 302 Redirect\r\nLocation: /%s\r\n",hop==0?"cookie-cap/1":hop==1?"cookie-cap/2":"cookie-count");
        for(unsigned i=0;i<100;i++)used+=(size_t)snprintf((char *)p->response+used,sizeof(p->response)-used,"Set-Cookie: cookie%u=x; Domain=fixture.example; Path=/\r\n",hop*100+i);
        used+=(size_t)snprintf((char *)p->response+used,sizeof(p->response)-used,"Content-Length: 0\r\n\r\n");p->response_bytes=used;
    } else if(!strcmp(path,"/cookie")||!strcmp(path,"/page")||!strcmp(path,"/cookie-count")||!strcmp(path,"/tls-cookie-end")){
        const char *page="<!doctype html><title>HTTP</title><h1>Ciuki</h1>";
        if(!strcmp(path,"/tls-cookie-end")){
            CHECK(p->tls && strstr(p->request,"Cookie: tls_session=Ciuki\r\n"));
            puts("fixture_tls_cookie_roundtrip=1");
        }
        if(!strcmp(path,"/cookie")){
            CHECK(strstr(p->request,"Cookie: hop=one"));
            CHECK(!strstr(p->request,"secure=hidden") && !strstr(p->request,"path=hidden") &&
                !strstr(p->request,"domain=hidden") && !strstr(p->request,"expired=hidden"));
        }
        if(!strcmp(path,"/cookie-count")){
            const char *cookie=strstr(p->request,"Cookie: ");CHECK(cookie);unsigned count=0;
            for(const char *s=cookie;*s && *s!='\r';s++)if(*s=='=')count++;
            CHECK(count==200);printf("fixture_cookie_retention=%u\n",count);
        }
        p->response_bytes=(size_t)snprintf((char *)p->response,sizeof(p->response),"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n\r\n%s",strlen(page),page);
    } else if(!strcmp(path,"/large-header")){
        strcpy((char *)p->response,"HTTP/1.1 200 OK\r\nX: ");size_t n=strlen((char *)p->response);
        memset(p->response+n,'x',8193);memcpy(p->response+n+8193,"\r\n\r\n",4);p->response_bytes=n+8197;
    } else if(!strcmp(path,"/gzip64")){
        CHECK(p->tls);
        unsigned char plain[65536],encoded[1024];memset(plain,' ',sizeof(plain));
        const char *page="<!doctype html><title>TLS gzip</title><h1>Ciuki</h1>";
        size_t used=strlen(page);memcpy(plain,page,used);
        for(;used+16<=sizeof(plain);used+=16)memcpy(plain+used,"<!-- padding -->",16);
        z_stream z={0};CHECK(deflateInit2(&z,9,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
        z.next_in=plain;z.avail_in=sizeof(plain);z.next_out=encoded;z.avail_out=sizeof(encoded);
        CHECK(deflate(&z,Z_FINISH)==Z_STREAM_END);size_t n=z.total_out;deflateEnd(&z);
        size_t header=(size_t)snprintf((char *)p->response,sizeof(p->response),"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Encoding: gzip\r\nContent-Length: %zu\r\n\r\n",n);
        memcpy(p->response+header,encoded,n);p->response_bytes=header+n;
    } else if(!strcmp(path,"/gzip")||!strcmp(path,"/deflate")){
        unsigned char encoded[128];z_stream z={0};
        CHECK(deflateInit2(&z,9,Z_DEFLATED,!strcmp(path,"/gzip")?31:15,8,Z_DEFAULT_STRATEGY)==Z_OK);
        z.next_in=(Bytef *)"Ciuki";z.avail_in=5;z.next_out=encoded;z.avail_out=sizeof(encoded);
        CHECK(deflate(&z,Z_FINISH)==Z_STREAM_END);size_t n=z.total_out;deflateEnd(&z);
        size_t header=(size_t)snprintf((char *)p->response,sizeof(p->response),"HTTP/1.1 200 OK\r\nContent-Encoding: %s\r\nContent-Length: %zu\r\n\r\n",path+1,n);
        memcpy(p->response+header,encoded,n);p->response_bytes=header+n;
    } else {p->response_bytes=strlen(wire);memcpy(p->response,wire,p->response_bytes);}
    p->response_sent=0;p->request_bytes=0;
}
static void request_data(struct peer *p,const unsigned char *data,size_t n)
{
    CHECK(n<sizeof(p->request)-p->request_bytes);memcpy(p->request+p->request_bytes,data,n);p->request_bytes+=n;p->request[p->request_bytes]=0;
    char *end=strstr(p->request,"\r\n\r\n");if(!end)return;
    size_t header=(size_t)(end+4-p->request),body=0;
    const char *length=strstr(p->request,"Content-Length: ");if(length)body=(size_t)strtoul(length+16,NULL,10);
    if(p->request_bytes>=header+body)response(p);
}
static void packet(struct peer *p,const void *data,unsigned n,bool fin)
{
    tcp_reply(p->frame,p->next,p->ack,fin?0x19:n?0x18:0x10,data,n);
    p->next+=n+(fin?1:0);if(n||fin)p->waiting=true;
}
static void peer_output(struct peer *p)
{
    if(p->waiting || p->hold_record)return;
    if(p->unsolicited && p->response_sent==p->response_bytes){
        const char *extra="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nWRONG";
        memcpy(p->response,extra,strlen(extra));p->response_bytes=strlen(extra);p->response_sent=0;p->unsolicited=false;
    }
    if(!p->tls){
        if(p->reset_after){p->reset_after=false;p->response_sent=p->response_bytes;tcp_reply(p->frame,p->next,p->ack,0x14,NULL,0);return;}
        size_t n=p->response_bytes-p->response_sent;if(n>1024)n=1024;
        if(n){packet(p,p->response+p->response_sent,(unsigned)n,false);p->response_sent+=n;}
        else if(p->close_after){packet(p,NULL,0,true);p->close_after=false;}
        return;
    }
    for(unsigned step=0;step<8;step++){
        if(p->reset_after){p->reset_after=false;p->response_sent=p->response_bytes;tcp_reply(p->frame,p->next,p->ack,0x14,NULL,0);return;}
        unsigned state=br_ssl_engine_current_state(&p->server->eng);size_t n;
        if(state&BR_SSL_SENDREC){
            unsigned char *record=br_ssl_engine_sendrec_buf(&p->server->eng,&n);if(n>1024)n=1024;
            if(p->record_prefix){CHECK(n>p->record_prefix);n=p->record_prefix;p->record_prefix=0;p->hold_record=true;}
            packet(p,record,(unsigned)n,false);br_ssl_engine_sendrec_ack(&p->server->eng,n);return;
        }
        if(state&BR_SSL_RECVAPP){
            unsigned char *app=br_ssl_engine_recvapp_buf(&p->server->eng,&n);request_data(p,app,n);br_ssl_engine_recvapp_ack(&p->server->eng,n);continue;
        }
        if((state&BR_SSL_SENDAPP) && p->response_sent<p->response_bytes){
            unsigned char *app=br_ssl_engine_sendapp_buf(&p->server->eng,&n);size_t left=p->response_bytes-p->response_sent;if(n>left)n=left;
            memcpy(app,p->response+p->response_sent,n);p->response_sent+=n;br_ssl_engine_sendapp_ack(&p->server->eng,n);br_ssl_engine_flush(&p->server->eng,0);continue;
        }
        if(p->close_after && p->response_sent==p->response_bytes){packet(p,NULL,0,true);p->close_after=false;}
        return;
    }
}
int net_stack_transmit(unsigned index,const void *data,unsigned bytes)
{
    const unsigned char *f=data;
    if(bytes>=42 && get16(f+12)==0x800 && f[23]==17 && get16(f+36)==53 && !strcmp(fixture_mode,"dns")){
        unsigned n=get16(f+38)-8;unsigned char answer[512];CHECK(n<sizeof(answer));memcpy(answer,f+42,n);
        answer[2]=0x81;answer[3]=0x83;put16(answer+6,0);udp_reply(f,53,get16(f+34),answer,n,false);return 0;
    }
    if(bytes<54 || get16(f+12)!=0x800 || f[23]!=6 || (get16(f+36)!=8080 && get16(f+36)!=8443 && get16(f+36)!=9))return f6_10_transmit(index,data,bytes);
    unsigned port=get16(f+34),flags=f[47],header=(f[46]>>4)*4,length=get16(f+16)-20-header;
    if(get16(f+36)==9){tcp_reply(f,0,get32(f+38)+1,0x14,NULL,0);return 0;}
    struct peer *p=NULL;
    for(unsigned i=0;i<16;i++)if(peers[i].port==port){p=&peers[i];break;}
    if(flags&2){
        if(!p)for(unsigned i=0;i<16;i++)if(!peers[i].port){p=&peers[i];break;}
        CHECK(p);memset(p,0,sizeof(*p));p->port=port;p->ack=get32(f+38)+1;p->next=100001;memcpy(p->frame,f,54);
        connections_seen++;p->tls=get16(f+36)==8443;
        if(p->tls){
            p->server=calloc(1,sizeof(*p->server));p->io=malloc(BR_SSL_BUFSIZE_BIDI);CHECK(p->server && p->io);
            br_ssl_server_init_full_ec(p->server,server_certificates,server_depth,BR_KEYTYPE_EC,br_skey_decoder_get_ec(&server_key));
            br_ssl_engine_set_versions(&p->server->eng,!strcmp(fixture_mode,"tls10")?BR_TLS10:BR_TLS12,!strcmp(fixture_mode,"tls10")?BR_TLS10:BR_TLS12);
            unsigned char seed[32];CHECK(!getentropy(seed,sizeof(seed)));br_ssl_engine_inject_entropy(&p->server->eng,seed,sizeof(seed));
            br_ssl_engine_set_buffer(&p->server->eng,p->io,BR_SSL_BUFSIZE_BIDI,1);CHECK(br_ssl_server_reset(p->server));
        }
        tcp_reply(f,100000,p->ack,0x12,NULL,0);return 0;
    }
    if(!p)return 0;
    memcpy(p->frame,f,54);
    if(flags&1){tcp_reply(f,p->next,get32(f+38)+length+1,0x11,NULL,0);free(p->server);free(p->io);memset(p,0,sizeof(*p));return 0;}
    if(flags&4){free(p->server);free(p->io);memset(p,0,sizeof(*p));return 0;}
    if((flags&16) && get32(f+42)==p->next)p->waiting=false;
    if(length && get32(f+38)==p->ack){
        const unsigned char *s=f+34+header;p->ack+=length;
        if(!p->tls)request_data(p,s,length);
        else{
            size_t remaining=length;
            while(remaining){
                size_t n;unsigned char *record=br_ssl_engine_recvrec_buf(&p->server->eng,&n);
                if(!record || !n)break;if(n>remaining)n=remaining;memcpy(record,s,n);br_ssl_engine_recvrec_ack(&p->server->eng,n);s+=n;remaining-=n;
            }
            CHECK(!remaining);
        }
        packet(p,NULL,0,false);
    }
    peer_output(p);return 0;
}
static int result(int n) { if(n<0){errno=-n;return -1;}return n; }
int __wrap_socket(int family,int type,int protocol) { return result(net_socket_create(&client,family,type,protocol)); }
int __wrap_connect(int fd,const struct sockaddr *a,socklen_t n)
{
    CHECK(n==16);struct ciuki_sockaddr_in address;memcpy(&address,a,16);return result(net_socket_connect(&client,fd,&address,NULL));
}
ssize_t __wrap_send(int fd,const void *s,size_t n,int flags)
{
    if(send_blocked){blocked_sends++;errno=EAGAIN;return -1;}if(n>317)n=317;
    /* Split the refusal alert as well: every subsequent send must still see
     * an untransmitted request, including slices past the record header. */
    if(fd==injected_fd){CHECK(injected_transmitted && !*injected_transmitted);if(n>7)n=7;}
    ssize_t r=result(net_socket_io(&client,fd,(void *)s,(unsigned)n,(unsigned)flags,NULL,true,NULL));
    if(r>0 && fd==injected_fd && !injected_output){
        injected_output=true;
        if(*(const unsigned char *)s==21)injected_alerts++;
        if(*(const unsigned char *)s==22)injected_handshakes++;
    }
    return r;
}
void fixture_block_send(bool blocked) { send_blocked=blocked; }
unsigned fixture_blocked_sends(void) { return blocked_sends; }
ssize_t __wrap_recv(int fd,void *s,size_t n,int flags)
{
    /* Inject after clean_socket's poll/MSG_PEEK, inside the pump's first read.
     * The fixture waits on the synthetic transport, never on wall-clock races. */
    if(incoming_injection && !(flags&MSG_PEEK)){
        unsigned injection=incoming_injection;incoming_injection=0;injected_fd=fd;
        if(injection==1)CHECK(tcpip_callback_wait(inject_hello_request,NULL)==ERR_OK);
        else fixture_unsolicited();
        struct ciuki_pollfd ready={fd,POLLIN,0};
        CHECK(net_poll(&client,&ready,1,1000,NULL)==1 && ready.revents==POLLIN);
    }
    if(n>311)n=311;return result(net_socket_io(&client,fd,s,(unsigned)n,(unsigned)flags,NULL,false,NULL));
}
void fixture_incoming_after_check(bool hello,const bool *transmitted)
{ CHECK(!incoming_injection);incoming_injection=hello?1:2;injected_transmitted=transmitted; }
unsigned fixture_injected_alerts(void) { return injected_alerts; }
unsigned fixture_injected_handshakes(void) { return injected_handshakes; }
bool fixture_injected_closed(void) { return injected_closed; }
int __real_close(int);
int __wrap_close(int fd)
{
    if(fd==injected_fd){CHECK(injected_transmitted && !*injected_transmitted);injected_transmitted=NULL;injected_closed=true;injected_fd=-1;}
    if(fd==10000)return 0;if(fd>=32 && fd<CIUKI_OPEN_MAX && client.fds[fd].object)return result(file_close(&client,fd));return __real_close(fd);
}
int __wrap_fcntl(int fd,int cmd,...)
{
    uint32_t *flags=net_socket_flags(file_fd_object(&client,fd));CHECK(flags);
    if(cmd==F_GETFL)return (int)*flags;
    CHECK(cmd==F_SETFL);va_list args;va_start(args,cmd);*flags=(unsigned)va_arg(args,int);va_end(args);return 0;
}
int __wrap_getsockopt(int fd,int level,int option,void *value,socklen_t *bytes)
{ unsigned n=*bytes;int r=net_socket_option(&client,fd,0,level,option,value,&n,NULL);*bytes=n;return result(r); }
int __wrap_poll(struct pollfd *fds,nfds_t n,int timeout)
{
    struct ciuki_pollfd native[128];CHECK(n<=128);
    for(unsigned i=0;i<n;i++)native[i]=(struct ciuki_pollfd){fds[i].fd,fds[i].events,0};
    int r=net_poll(&client,native,(unsigned)n,timeout,NULL);
    for(unsigned i=0;i<n;i++)fds[i].revents=native[i].revents;
    return result(r);
}
int __wrap_getaddrinfo(const char *name,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
    (void)service;(void)hints;*out=NULL;uint32_t address;int status;
    if(!strcmp(name,"blocked.example")){
        lookup_entered=true;while(!lookup_allow)cu_sleep(1);
    }
    int e=0;if(!strcmp(name,"localhost")){address=PP_HTONL(0x7f000001);status=0;}else e=net_resolve(&client,name,&address,&status);
    if(e){errno=-e;return EAI_SYSTEM;}if(status)return status==2?EAI_AGAIN:EAI_NONAME;
    struct addrinfo *a=calloc(1,sizeof(*a));struct sockaddr_in *s=calloc(1,sizeof(*s));CHECK(a && s);
    s->sin_family=AF_INET;s->sin_addr.s_addr=address;a->ai_family=AF_INET;a->ai_socktype=SOCK_STREAM;a->ai_addr=(void *)s;a->ai_addrlen=16;*out=a;
    if(!strcmp(name,"blocked.example"))blocked_lookup_returned=true;
    return 0;
}
void __wrap_freeaddrinfo(struct addrinfo *a) { if(a){free(a->ai_addr);free(a);} }
static unsigned char *load(const char *name,size_t *bytes)
{
    FILE *file=fopen(name,"rb");CHECK(file);CHECK(!fseek(file,0,SEEK_END));long n=ftell(file);CHECK(n>0 && n<65536);rewind(file);
    unsigned char *p=malloc((size_t)n);CHECK(p && fread(p,1,(size_t)n,file)==(size_t)n);fclose(file);*bytes=(size_t)n;return p;
}
void fixture_begin(const char *mode,const char *cert,const char *key)
{
    fixture_mode=mode;client=(struct process){.pid=1,.state=PROC_LIVE,.fds=fds};
    for(unsigned i=0;i<32;i++)fds[i].object=&reserved;
    ciuki_lwip_seed(0x12345678);CHECK(!net_stack_init());
    CHECK(!net_stack_attach(0,client_mac,true));check_address(PP_HTONL(0xc0000264));
    if(!strcmp(mode,"offline")||!strcmp(mode,"hostname-offline")||!strcmp(mode,"loopback-offline")){fixture_offline=true;net_stack_link(0,false);check_address(0);}
    if(strcmp(cert,"-")){
        server_certificates[0].data=certificate_data=load(cert,&server_certificates[0].data_len);
        if(!strcmp(mode,"intermediate")||!strcmp(mode,"malformed-chain")){
            char path[1024];CHECK(strlen(cert)+16<sizeof(path));strcpy(path,cert);strcpy(strrchr(path,'.'),"-chain.der");
            server_certificates[1].data=intermediate_data=load(path,&server_certificates[1].data_len);server_depth=2;
        }
        if(!strcmp(mode,"cert-depth")){server_depth=11;for(unsigned i=1;i<server_depth;i++)server_certificates[i]=server_certificates[0];}
        if(!strcmp(mode,"cert-buffer")){size_t old=server_certificates[0].data_len;server_certificates[0].data_len=32769;certificate_data=realloc(certificate_data,32769);CHECK(certificate_data);memset(certificate_data+old,0,32769-old);server_certificates[0].data=certificate_data;}
        size_t bytes;key_data=load(key,&bytes);br_skey_decoder_init(&server_key);br_skey_decoder_push(&server_key,key_data,bytes);CHECK(br_skey_decoder_get_ec(&server_key));
    }
}
unsigned fixture_requests(void) { return http_requests; }
unsigned fixture_connections(void) { return connections_seen; }
unsigned fixture_loops(void) { return loop_requests; }
void fixture_end(void)
{
    for(unsigned i=32;i<CIUKI_OPEN_MAX;i++)if(fds[i].object)file_close(&client,(int)i);
    cu_sleep(30);cu_host_shutdown();
    for(unsigned i=0;i<16;i++){free(peers[i].server);free(peers[i].io);}
    free(certificate_data);free(key_data);free(intermediate_data);
}

/* Native read-only status boundary: production net.c opens /dev/net and
 * validates the versioned record before using address_configured. */
int __real_open(const char *,int,...);
int __wrap_open(const char *path,int flags,...)
{
    if(!strcmp(path,"/dev/net")){CHECK(flags==O_RDONLY);return 10000;}
    va_list a;va_start(a,flags);mode_t mode=(flags&O_CREAT)?(mode_t)va_arg(a,int):0;va_end(a);
    return __real_open(path,flags,mode);
}
int ciuki_devctl(int fd,uint32_t op,void *record,uint32_t size)
{
    CHECK(fd==10000 && op==NET_GET_STATE && size==sizeof(struct ciuki_net_state));
    struct ciuki_net_state *state=record;CHECK(state->version==CIUKI_NET_VERSION && state->size==size);
    state->address_configured=!fixture_offline;return 0;
}
bool ciuki_host_clock_rtc(void) { return strcmp(fixture_mode,"fallback-clock")!=0; }
bool fixture_lookup_blocked(void) { return lookup_entered; }
void fixture_lookup_release(void) { lookup_allow=true; }
bool fixture_lookup_returned_in_thread(void) { return blocked_lookup_returned; }
static void expire_dns(void *unused) { (void)unused;for(unsigned i=0;i<61;i++)dns_tmr(); }
void fixture_dns_expire(void) { CHECK(tcpip_callback_wait(expire_dns,NULL)==ERR_OK); }
void fixture_dns_change(void) { CHECK(tcpip_callback_wait(dns_servers_reset,NULL)==ERR_OK); }
unsigned fixture_dns_queries(void) { return dns_queries; }

static void inject_unsolicited(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<16;i++)if(peers[i].port){peers[i].unsolicited=true;peer_output(&peers[i]);}
}
void fixture_unsolicited(void) { CHECK(tcpip_callback_wait(inject_unsolicited,NULL)==ERR_OK); }
static void inject_hello_request(void *unused)
{
    (void)unused;unsigned count=0;
    for(unsigned i=0;i<16;i++)if(peers[i].port){
        struct peer *p=&peers[i];CHECK(p->tls && br_ssl_engine_renegotiate(&p->server->eng));
        p->unsolicited=true;peer_output(p);count++;
    }
    CHECK(count==1);
}
static void inject_partial(void *bytes)
{
    for(unsigned i=0;i<16;i++)if(peers[i].port){
        peers[i].record_prefix=*(const size_t *)bytes;peers[i].unsolicited=true;peer_output(&peers[i]);
    }
}
void fixture_unsolicited_prefix(size_t bytes) { CHECK(tcpip_callback_wait(inject_partial,&bytes)==ERR_OK); }
