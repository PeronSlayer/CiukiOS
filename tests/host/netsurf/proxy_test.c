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
    struct ciuki_net net={0};
    struct ciuki_connection c={.port=3128,.job=j};assert(resolve(&net,&c));
    assert(ntohs(connected.sin_port)==3128 && connected.sin_addr.s_addr==0x0100007f && c.state==CONNECTING);
    assert(!dns_name[0]);
    ciuki_net_job_free(j);assert(!ciuki_net_pending_bytes());
    r.proxy_host="proxy.example";j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error);
    c=(struct ciuki_connection){.port=3128,.job=j};assert(resolve(&net,&c));
    assert(!strcmp(dns_name,"proxy.example") && j->dns_token==42 && c.state==RESOLVING);ciuki_net_job_free(j);
    r.proxy_host="bad\r\nInjected";j=ciuki_net_job(&r,NULL,NULL);assert(j && j->error);ciuki_net_job_free(j);
    r.proxy_host="proxy.example";r.proxy_port=0;j=ciuki_net_job(&r,NULL,NULL);assert(j && j->error);ciuki_net_job_free(j);
    r.tls=true;r.port=443;r.proxy_port=3128;j=ciuki_net_job(&r,NULL,NULL);assert(j && !j->error && !j->proxy_port && !j->proxy_host[0]);
    assert(admit(j));wire=malloc(j->request_bytes+1);assert(wire);memcpy(wire,j->request,j->request_bytes);wire[j->request_bytes]=0;
    assert(strstr(wire,"GET /page?q=woof HTTP/1.1\r\nHost: origin.example\r\n"));free(wire);
    c=(struct ciuki_connection){.port=443,.job=j};assert(resolve(&net,&c));assert(!strcmp(dns_name,"origin.example"));ciuki_net_job_free(j);
    puts("PASS G26 HTTP proxy: absolute target/origin Host+cookies/owned input/proxy endpoint/injection rejection/HTTPS direct");return 0;
}
