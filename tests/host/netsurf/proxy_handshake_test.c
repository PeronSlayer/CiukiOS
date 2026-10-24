#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sdk/netsurf/port/proxy.h"
#include "apps/control/internet_options.h"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);char auth[264],request[1024];
    assert(!ciuki_proxy_basic(auth,sizeof(auth),"Aladdin","open sesame") && !strcmp(auth,"Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
    assert(ciuki_proxy_connect_request(request,sizeof(request),"origin.example",443,auth)>0);
    assert(strstr(request,"CONNECT origin.example:443 HTTP/1.1\r\nHost: origin.example:443\r\nProxy-Authorization: Basic "));
    assert(ciuki_proxy_connect_request(request,sizeof(request),"bad\r\nInjected",443,auth)==-1);
    const char *success="HTTP/1.1 200 Connection Established\r\n\r\n";
    for(size_t i=0;i<strlen(success);i++)assert(ciuki_proxy_connect_response(success,i)==0);
    assert(ciuki_proxy_connect_response(success,strlen(success))==1);
    const char *denied="HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"test\"\r\n\r\n";
    assert(ciuki_proxy_connect_response(denied,strlen(denied))==-407);
    const char *malformed="HTTP/1.1 200 OK\n\n";assert(!ciuki_proxy_connect_response(malformed,strlen(malformed)));
    assert(!ciuki_proxy_auth_valid("Basic abc\r\n") && !ciuki_proxy_auth_valid("Basic ab=c"));
    struct cp_internet_options o;internet_defaults(&o);assert(internet_valid(&o) && internet_zone(&o,"www.example")==0);
    assert(internet_zone(&o,"server")==1 && internet_zone(&o,"192.168.1.1")==1);
    strcpy(o.trusted,"trusted.example;second.example");strcpy(o.restricted,"blocked.example;trusted.example");
    assert(internet_zone(&o,"TRUSTED.example")==3 && internet_zone(&o,"second.example")==2 && !o.scripts[3]);
    o.proxy_user[0]='x';o.proxy_user[1]=':';assert(!internet_valid(&o));
    puts("PASS G26B CONNECT authority/auth isolation, split success/407/malformed headers and persisted script-zone classification");return 0;
}
