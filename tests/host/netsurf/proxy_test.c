/* RFC 9112 absolute-form and endpoint selection through production net.c.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include "sdk/netsurf/port/net.c"
static struct sockaddr_in connected;
static char dns_name[254];
uint64_t ciuki_dns_submit(struct ciuki_dns *dns,const char *host)
{ (void)dns;assert(strlen(host)<sizeof(dns_name));strcpy(dns_name,host);return 42; }
int __wrap_socket(int family,int type,int protocol)
{ assert(family==AF_INET && type==SOCK_STREAM && !protocol);return 77; }
int __wrap_fcntl(int fd,int operation,...)
{
    assert(fd==77);if(operation==F_GETFL)return O_RDWR;
    assert(operation==F_SETFL);va_list args;va_start(args,operation);int flags=va_arg(args,int);va_end(args);assert(flags&O_NONBLOCK);return 0;
}
int __wrap_connect(int fd,const struct sockaddr *address,socklen_t n)
{ assert(fd==77 && n==sizeof(connected));connected=*(const struct sockaddr_in *)address;errno=EINPROGRESS;return -1; }
static char tunnel_sent[2048];static size_t tunnel_sent_bytes,tunnel_received;
static const char *tunnel_reply="HTTP/1.1 200 Connection established\r\n\r\n";
ssize_t __wrap_send(int fd,const void *data,size_t bytes,int flags)
{
    assert(fd==77 && flags==MSG_NOSIGNAL);if(bytes>7)bytes=7;
    assert(bytes<=sizeof(tunnel_sent)-tunnel_sent_bytes-1);memcpy(tunnel_sent+tunnel_sent_bytes,data,bytes);
    tunnel_sent_bytes+=bytes;tunnel_sent[tunnel_sent_bytes]=0;return (ssize_t)bytes;
}
ssize_t __wrap_recv(int fd,void *data,size_t bytes,int flags)
{
    assert(fd==77 && flags==MSG_DONTWAIT);size_t remain=strlen(tunnel_reply)-tunnel_received;
    if(bytes>3)bytes=3;if(bytes>remain)bytes=remain;memcpy(data,tunnel_reply+tunnel_received,bytes);tunnel_received+=bytes;return (ssize_t)bytes;
}
bool ciuki_tls_clock(uint32_t *days,uint32_t *seconds) { *days=1;*seconds=1;return true; }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    char proxy[32]="127.0.0.1";
    struct ciuki_request r={.host="origin.example",.target="/page?q=woof",.port=8080,.proxy_host=proxy,.proxy_port=3128,.cookie="session=Ciuki"};
    struct ciuki_job *j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error && !strcmp(j->proxy_host,proxy));
    strcpy(proxy,"changed.example");assert(!strcmp(j->proxy_host,"127.0.0.1"));
    assert(admit(j));char *wire=malloc(j->request_bytes+1);assert(wire);memcpy(wire,j->request,j->request_bytes);wire[j->request_bytes]=0;
    assert(strstr(wire,"GET http://origin.example:8080/page?q=woof HTTP/1.1\r\nHost: origin.example:8080\r\n"));
    assert(strstr(wire,"Cookie: session=Ciuki\r\n"));free(wire);
    static struct ciuki_net net;
    static struct ciuki_connection c;c.port=3128;c.job=j;assert(resolve(&net,&c));
    assert(ntohs(connected.sin_port)==3128 && connected.sin_addr.s_addr==0x0100007f && c.state==CONNECTING);
    assert(!dns_name[0]);
    ciuki_net_job_free(j);assert(!ciuki_net_pending_bytes());
    r.proxy_host="proxy.example";j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error);
    memset(&c,0,sizeof(c));c.port=3128;c.job=j;assert(resolve(&net,&c));
    assert(!strcmp(dns_name,"proxy.example") && j->dns_token==42 && c.state==RESOLVING);ciuki_net_job_free(j);
    r.proxy_host="bad\r\nInjected";j=ciuki_net_job(&r,NULL,NULL);assert(j && j->error);ciuki_net_job_free(j);
    r.proxy_host="proxy.example";r.proxy_port=0;j=ciuki_net_job(&r,NULL,NULL);assert(j && j->error);ciuki_net_job_free(j);
    r.tls=true;r.port=443;r.proxy_port=3128;j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error && j->proxy_port==3128 && !strcmp(j->proxy_host,"proxy.example"));
    assert(admit(j));wire=malloc(j->request_bytes+1);assert(wire);memcpy(wire,j->request,j->request_bytes);wire[j->request_bytes]=0;
    assert(strstr(wire,"GET /page?q=woof HTTP/1.1\r\nHost: origin.example\r\n"));free(wire);
    memset(&c,0,sizeof(c));c.port=3128;c.job=j;assert(resolve(&net,&c));assert(!strcmp(dns_name,"proxy.example"));ciuki_net_job_free(j);
    char auth[264],request_[1024];assert(!ciuki_proxy_basic(auth,sizeof(auth),"Aladdin","open sesame") && !strcmp(auth,"Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
    assert(ciuki_proxy_basic(auth,sizeof(auth),"user\r\n","secret")==-1);
    assert(!ciuki_proxy_basic(auth,sizeof(auth),"Aladdin","open sesame"));
    assert(ciuki_proxy_connect_request(request_,sizeof(request_),"origin.example",443,auth)>0);
    assert(strstr(request_,"CONNECT origin.example:443 HTTP/1.1\r\n") && strstr(request_,"Proxy-Authorization: Basic "));
    assert(ciuki_proxy_connect_request(request_,sizeof(request_),"evil\r\n",443,auth)==-1);
    assert(ciuki_proxy_connect_response("HTTP/1.1 200 Connection established\r\n\r\n",39)==1);
    const char *ok="HTTP/1.1 200 OK\r\n\r\n";
    for(size_t i=0;i<strlen(ok);i++)assert(!ciuki_proxy_connect_response(ok,i));
    assert(ciuki_proxy_connect_response(ok,strlen(ok))==1);
    const char *denied="HTTP/1.1 407 Proxy Authentication Required\r\n\r\n";
    assert(ciuki_proxy_connect_response(denied,strlen(denied))==-407);
    r.proxy_auth=auth;j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error && admit(j));
    memset(&c,0,sizeof(c));c.port=3128;c.fd=77;c.state=TUNNEL;c.job=j;
    for(unsigned i=0;i<100 && c.state==TUNNEL && !j->error;i++)tunnel(&c);
    assert(!j->error && c.state==TRANSFER && j->stage==CIUKI_FETCH_TLS && !j->sent);
    assert(strstr(tunnel_sent,"CONNECT origin.example:443 HTTP/1.1\r\n") && strstr(tunnel_sent,"Proxy-Authorization: Basic "));
    wire=malloc(j->request_bytes+1);assert(wire);memcpy(wire,j->request,j->request_bytes);wire[j->request_bytes]=0;
    assert(!strstr(wire,"Proxy-Authorization"));free(wire);ciuki_net_job_free(j);
    tunnel_reply="HTTP/1.1 407 Proxy Authentication Required\r\n\r\n";tunnel_sent_bytes=tunnel_received=0;
    j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error && admit(j));memset(&c,0,sizeof(c));c.fd=77;c.state=TUNNEL;c.job=j;
    for(unsigned i=0;i<100 && !j->error;i++)tunnel(&c);
    assert(j->error && strstr(j->error,"Proxy authentication") && c.state==TUNNEL);ciuki_net_job_free(j);
    puts("PASS G26B native CONNECT state machine: partial writes/reads, TLS gated on 2xx, 407 refusal, proxy credentials excluded from origin request");
    puts("PASS G26 HTTP proxy: absolute target/origin Host+cookies/owned input/proxy endpoint/injection rejection/HTTPS through the CONNECT tunnel");return 0;
}
