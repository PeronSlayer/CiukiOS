/* f6-18 browser test profile, included by chrome.c (CIUKI_TESTMODE_PURE builds
 * only the parsing/sanitising helpers for the host unit test).
 *
 * Gate: `--test=browser` on the command line AND a kernel test request that
 * starts with "f6:browser run=" (probe call 5, CIUKI_SYS_PROBE_QUERY). The
 * argument alone enables nothing: a normal boot has no test request, so the
 * fixture CA never joins the trust store and no report is sent.
 *
 * Active profile: the fixture CA (test_anchors.h, generated at build time from
 * tests/suites/fixtures/f6-browser/ca.pem) replaces the production bundle for
 * every TLS connection; the first completed navigation reports one line
 * through probe call 3, "case=browser-page status=<n> bytes=<n> tls=<verdict>
 * url=<sanitised>", which the kernel's f6:browser controller validates and
 * republishes as evidence. SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "tls.h"

#define CIUKI_TEST_URL_MAX 90
#define CIUKI_TEST_REQUEST_MAX 64
#define CIUKI_TEST_LINE_MAX 240 /* CIUKI_PROBE_REPORT_MAX, kernel probe call 3 */

/* The record grammar's value charset (scripts/test/evidence.py): every other
 * byte becomes '_' so the URL can travel in a kernel record verbatim. */
static size_t ciuki_test_sanitise_url(const char *in,char *out,size_t cap)
{
    size_t n=0;
    if(!cap)return 0;
    for(;in && in[n] && n+1<cap && n<CIUKI_TEST_URL_MAX;n++){
        char c=in[n];
        bool ok=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c==':'||c=='.'||c==','||c=='/'||c=='+'||c=='-';
        out[n]=ok?c:'_';
    }
    out[n]=0;
    return n;
}

static bool ciuki_test_gate(const char *request,size_t length)
{
    /* The kernel has already enforced the full grammar (selector.c); the
     * browser only needs the probe identity: 8-hex run id, then " url=" and
     * at least the kernel's 8 URL bytes. */
    static const char prefix[]="f6:browser run=";
    if(!request || length<sizeof(prefix)-1+8+5+8 || length>CIUKI_TEST_REQUEST_MAX)return false;
    if(memcmp(request,prefix,sizeof(prefix)-1))return false;
    for(size_t i=0;i<8;i++){
        char c=request[sizeof(prefix)-1+i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;
    }
    return !memcmp(request+sizeof(prefix)-1+8," url=",5);
}

/* Verdict tokens: [a-z_] only (kernel report_token). A merged f6-17 adds the
 * CIUKI_TLS_NOT_READY value: map it to "not_ready" here (f6-18 report). */
static const char *ciuki_test_verdict(int tls_error,const char *error)
{
    if(tls_error<0)return "none";
    switch((enum ciuki_tls_error)tls_error){
    case CIUKI_TLS_OK:return error?"error":"ok";
    case CIUKI_TLS_MEMORY:return "memory";
    case CIUKI_TLS_CLOCK:return "clock";
    case CIUKI_TLS_ENTROPY:return "entropy";
    case CIUKI_TLS_HOST:return "host";
    case CIUKI_TLS_CERT:return "cert";
    case CIUKI_TLS_DATE:return "date";
    case CIUKI_TLS_PROTOCOL:return "protocol";
    case CIUKI_TLS_IO:return "io";
    case CIUKI_TLS_EOF:return "eof";
    default:return "other";
    }
}

static int ciuki_test_format(char *line,size_t cap,unsigned status,size_t bytes,int tls_error,const char *error,const char *url)
{
    char clean[CIUKI_TEST_URL_MAX+1];
    ciuki_test_sanitise_url(url,clean,sizeof(clean));
    int n=snprintf(line,cap,"case=browser-page status=%u bytes=%zu tls=%s url=%s",status>999?999:status,bytes,
                   ciuki_test_verdict(tls_error,error),clean[0]?clean:"none");
    return n>0 && (size_t)n<cap ? n : -1;
}

#ifndef CIUKI_TESTMODE_PURE
#include "testmode.h"
#include "test_anchors.h"
#ifndef CIUKI_HOST
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#endif
static bool test_requested,test_active,test_reported;
static unsigned test_status;
static size_t test_bytes;
static int test_tls=-1;
static const char *test_error;

static size_t ciuki_test_request(char *out,size_t cap)
{
#ifdef CIUKI_HOST
    /* Host replay seam: the harness supplies the request it wants to prove. */
    const char *env=getenv("CIUKI_TEST_REQUEST");
    size_t n=env?strlen(env):0;
    if(!n || n>=cap)return 0;
    memcpy(out,env,n+1);
    return n;
#else
    uint8_t buffer[16+CIUKI_TEST_REQUEST_MAX];
    int32_t n=(int32_t)ciuki_raw_probe_query(CU_PTR(buffer),sizeof(buffer),0,0,0,0);
    if(n<16)return 0;
    uint32_t length;memcpy(&length,buffer+12,4);
    if(length>CIUKI_TEST_REQUEST_MAX || 16+length!=(uint32_t)n || length>=cap)return 0;
    memcpy(out,buffer+16,length);out[length]=0;
    return length;
#endif
}

static void ciuki_test_arm(void)
{
    char request[CIUKI_TEST_REQUEST_MAX+1];
    size_t n=ciuki_test_request(request,sizeof(request));
    test_active=test_requested && ciuki_test_gate(request,n);
}

bool ciuki_test_active(void) { return test_active; }
const br_x509_trust_anchor *ciuki_test_anchors(size_t *count)
{
    if(!test_active){*count=0;return NULL;}
    *count=CIUKI_TEST_ANCHOR_COUNT;
    return ciuki_test_anchors_table;
}
void ciuki_test_fetch_observe(unsigned status,size_t bytes,int tls_error,const char *error)
{
    if(!test_active)return;
    test_status=status;test_bytes=bytes;test_tls=tls_error;test_error=error;
}

static void ciuki_test_send(const char *line,size_t n)
{
#ifdef CIUKI_HOST
    fprintf(stderr,"[browser-test] %s\n",line);(void)n;
#else
    (void)ciuki_raw_probe_report(CU_PTR(line),(uint32_t)n,0,0,0,0);
#endif
}

/* Called when the throbber stops: the first completed navigation is the
 * case's outcome; the kernel accepts exactly one report per case. */
static void ciuki_test_page_done(void)
{
    if(!test_active || test_reported)return;
    struct browser_window *bw=browser();
    const char *url=NULL;
    if(bw){nsurl *current=browser_window_access_url(bw);if(current)url=nsurl_access(current);}
    char line[CIUKI_TEST_LINE_MAX+1];
    int n=ciuki_test_format(line,sizeof(line),test_status,test_bytes,test_tls,test_error,url);
    test_reported=true;
    if(n>0)ciuki_test_send(line,(size_t)n);
}
#endif
