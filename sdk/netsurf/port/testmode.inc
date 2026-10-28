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
 * every TLS connection; the requested navigation's outcome is reported once
 * through probe call 3, "case=browser-page status=<n> bytes=<n> tls=<verdict>
 * url=<sanitised>", which the kernel's f6:browser controller validates and
 * republishes as evidence. fx-11c: the outcome is the first throbber stop
 * with a document in the window AND an observed http/https fetch; the
 * navigation's own initial stop (empty window, "about:blank") is not one.
 * The bound is the fetcher's 30 s request limit (then the error page) inside
 * the probe's 120 s. SPDX-License-Identifier: MIT */
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

/* Verdict tokens: [a-z_] only (kernel report_token). f6-17's
 * CIUKI_TLS_NOT_READY (getentropy EAGAIN) is "not_ready" (https-not-ready case). */
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
    case CIUKI_TLS_NOT_READY:return "not_ready";
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

/* fx-11c: the terminal http/https fetches, newest last. fx-acc1: the
 * acceptance run 56762a60 (site 2: the 52,761-byte document, then 19 more
 * fetches, most of them 404 images the snapshot does not hold) reported
 * "status=404 bytes=27": a history of 8 had dropped the document's own
 * record and the fallback took the newest fetch. The history now holds 256
 * fetches keyed by a 64-bit FNV-1a hash of the URL, and a shown http/https
 * document without a record of its own reports nothing rather than another
 * fetch's outcome. */
#define CIUKI_TEST_SEEN 256
struct ciuki_test_seen { uint64_t url; unsigned status; size_t bytes; int tls; bool error; };
static struct ciuki_test_seen test_seen[CIUKI_TEST_SEEN];
static unsigned test_seen_count;

static uint64_t ciuki_test_url_hash(const char *url)
{
    uint64_t h=0xcbf29ce484222325ull;
    for(const unsigned char *p=(const unsigned char *)(url?url:"");*p;p++){h^=*p;h*=0x100000001b3ull;}
    return h;
}
static void ciuki_test_seen_add(const char *url,unsigned status,size_t bytes,int tls_error,bool error)
{
    if(test_seen_count==CIUKI_TEST_SEEN) {
        memmove(test_seen,test_seen+1,sizeof(test_seen)-sizeof(*test_seen));test_seen_count--;
    }
    test_seen[test_seen_count++]=(struct ciuki_test_seen){ciuki_test_url_hash(url),status,bytes,tls_error,error};
}
/* The fetch behind the shown document: the newest one for its URL; for an
 * internal page (the error page of a failed fetch) the newest failure,
 * else the newest fetch. -1: nothing to report yet. */
static int ciuki_test_outcome(const char *shown)
{
    bool remote=shown && (!strncmp(shown,"http://",7) || !strncmp(shown,"https://",8));
    uint64_t key=ciuki_test_url_hash(shown);
    for(int i=(int)test_seen_count-1;i>=0;i--)
        if(remote ? test_seen[i].url==key : test_seen[i].error)return i;
    return !remote && test_seen_count ? (int)test_seen_count-1 : -1;
}

#ifndef CIUKI_TESTMODE_PURE
#include "testmode.h"
#include "test_anchors.h"
#ifndef CIUKI_HOST
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#endif
static bool test_requested,test_active,test_reported;

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
void ciuki_test_fetch_observe(const char *url,unsigned status,size_t bytes,int tls_error,const char *error)
{
    if(!test_active)return;
    ciuki_test_seen_add(url,status,bytes,tls_error,error!=NULL);
}

static void ciuki_test_send(const char *line,size_t n)
{
#ifdef CIUKI_HOST
    fprintf(stderr,"[browser-test] %s\n",line);(void)n;
#else
    (void)ciuki_raw_probe_report(CU_PTR(line),(uint32_t)n,0,0,0,0);
#endif
}

/* Called when the throbber stops; the kernel accepts exactly one report per
 * case. fx-11c: browser_window_navigate stops the window before it fetches,
 * which also stops the throbber; with no document yet the window "is"
 * about:blank and no fetch has finished, so that stop is not the outcome. */
static void ciuki_test_page_done(void)
{
    if(!test_active || test_reported || !test_seen_count)return;
    struct browser_window *bw=browser();
    if(!bw || !browser_window_has_content(bw))return;
    nsurl *current=browser_window_access_url(bw);
    const char *url=current?nsurl_access(current):NULL;
    int i=ciuki_test_outcome(url);
    if(!url || i<0)return;
    char line[CIUKI_TEST_LINE_MAX+1];
    int n=ciuki_test_format(line,sizeof(line),test_seen[i].status,test_seen[i].bytes,test_seen[i].tls,
                            test_seen[i].error?"error":NULL,url);
    test_reported=true;
    if(n>0)ciuki_test_send(line,(size_t)n);
}
#endif
