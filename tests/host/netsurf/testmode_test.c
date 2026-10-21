/* f6-18: the browser test profile's gate and report formatting, built from the
 * same .inc the browser compiles (CIUKI_TESTMODE_PURE leaves out NetSurf and the
 * kernel syscalls). SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define CIUKI_TESTMODE_PURE 1
#include "../../../sdk/netsurf/port/testmode.inc"
static void gate_cases(void)
{
    const char *good[]={"f6:browser run=12345678 url=https://fixture.test/page",
        "f6:browser run=0000ABCD url=http://fixture.test/upgrade now=1"};
    for(unsigned i=0;i<2;i++)assert(ciuki_test_gate(good[i],strlen(good[i])));
    /* The argument alone never arms the profile: an empty or foreign request refuses. */
    const char *bad[]={"","f6:net-arp run=12345678","f5:browser run=12345678 url=https://fixture.test/page",
        "f6:browser run=1234567g url=https://fixture.test/page","f6:browser run=12345678 title=x",
        "f6:browser run=12345678 url=","f6:browser run=12345678"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!ciuki_test_gate(bad[i],strlen(bad[i])));
    assert(!ciuki_test_gate(NULL,10));
    char long_request[80];memset(long_request,'a',sizeof(long_request));
    memcpy(long_request,"f6:browser run=12345678 url=",28);
    assert(!ciuki_test_gate(long_request,65));
    assert(ciuki_test_gate(long_request,64));
}
static void sanitise_cases(void)
{
    char out[CIUKI_TEST_URL_MAX+1];
    assert(ciuki_test_sanitise_url("https://fixture.test/cookie-end",out,sizeof(out))==31 && !strcmp(out,"https://fixture.test/cookie-end"));
    assert(ciuki_test_sanitise_url("about:query/fetcherror?url=x&a=b c\"",out,sizeof(out))==35 && !strcmp(out,"about:query/fetcherror_url_x_a_b_c_"));
    char many[200];memset(many,'z',sizeof(many)-1);many[sizeof(many)-1]=0;
    assert(ciuki_test_sanitise_url(many,out,sizeof(out))==CIUKI_TEST_URL_MAX && strlen(out)==CIUKI_TEST_URL_MAX);
    assert(ciuki_test_sanitise_url(NULL,out,sizeof(out))==0 && !out[0]);
    char tiny[4];assert(ciuki_test_sanitise_url("abcdef",tiny,sizeof(tiny))==3 && !strcmp(tiny,"abc"));
}
static void format_cases(void)
{
    char line[241];
    assert(ciuki_test_format(line,sizeof(line),200,65536,CIUKI_TLS_OK,NULL,"https://fixture.test/gzip64")>0);
    assert(!strcmp(line,"case=browser-page status=200 bytes=65536 tls=ok url=https://fixture.test/gzip64"));
    assert(ciuki_test_format(line,sizeof(line),0,0,CIUKI_TLS_CERT,"TLS certificate validation failed","about:query/fetcherror?u=1")>0);
    assert(!strcmp(line,"case=browser-page status=0 bytes=0 tls=cert url=about:query/fetcherror_u_1"));
    assert(ciuki_test_format(line,sizeof(line),200,42,-1,NULL,"http://fixture.test/page")>0);
    assert(!strcmp(line,"case=browser-page status=200 bytes=42 tls=none url=http://fixture.test/page"));
    assert(ciuki_test_format(line,sizeof(line),200,42,CIUKI_TLS_OK,"an error with a 200","")>0);
    assert(!strcmp(line,"case=browser-page status=200 bytes=42 tls=error url=none"));
    assert(ciuki_test_format(line,sizeof(line),5000,1,CIUKI_TLS_ENTROPY,"e",NULL)>0 && !strcmp(line,"case=browser-page status=999 bytes=1 tls=entropy url=none"));
    assert(ciuki_test_format(line,sizeof(line),0,0,99,"x","u")>0 && strstr(line," tls=other "));
    /* Every verdict token stays inside the kernel's [a-z_] check. */
    for(int e=-1;e<=CIUKI_TLS_EOF+1;e++){const char *v=ciuki_test_verdict(e,NULL);for(;*v;v++)assert((*v>='a'&&*v<='z')||*v=='_');}
    char small[40];assert(ciuki_test_format(small,sizeof(small),200,1,CIUKI_TLS_OK,NULL,"https://fixture.test/page")<0);
}
int main(void)
{
    gate_cases();sanitise_cases();format_cases();
    puts("PASS browser test profile: gate, sanitiser, report format");
    return 0;
}
