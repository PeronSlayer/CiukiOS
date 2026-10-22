/* Real NetSurf core/frontend with only OS boundaries and input replaced.
 * Inspect the current rendered content, including its actual error page.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <libnsfb.h>
#include <libnsfb_event.h>
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "content/content.h"
#include "framebuffer/gui.h"
#include "framebuffer/ciuki/chrome.h"
#include "content/hlcache.h"
#include "content/handlers/html/private.h"
#include "content/handlers/html/form_internal.h"
#include "sdk/netsurf/port/budget.h"
#include "sdk/netsurf/port/tls.h"
#include "fixture-anchors.h"
#include <errno.h>
extern void fixture_begin(const char *,const char *,const char *),fixture_end(void);
extern unsigned fixture_requests(void),fixture_loops(void),fixture_connections(void);
extern bool fixture_lookup_blocked(void);
extern bool fixture_lookup_returned_in_thread(void);
extern void fixture_lookup_release(void);
static bool blocked,navigated,completed,printed;
static _Atomic bool lookup_idle;
static bool capture_held,lookup_released,late_checked;
static unsigned late_turns,completed_requests,completed_connections;
static const char *capture_path;
static struct hlcache_handle *completed_page;
static struct timespec lookup_started;
static const char *fixture_mode;
static bool reload_entropy,reloaded;
/* f6-20 browser-budget: a remote page rejected by the layout time limit
 * leaves the browser running on its error page; Try Again re-fetches it. */
static bool budget,retried,retry_done;
static struct hlcache_handle *error_page;
static void try_again(struct hlcache_handle *page)
{
    /* The error page's own "Try Again" submit button, through form_submit
     * as a click on it submits it (no chrome shortcut). */
    html_content *html=(html_content *)hlcache_handle_get_content(page);
    assert(content_get_type(page)==CONTENT_HTML && html->forms);
    struct form_control *retry=NULL;
    for(struct form_control *control=html->forms->controls;control;control=control->next)
        if(control->name && !strcmp(control->name,"retry"))retry=control;
    assert(retry && retry->type==GADGET_SUBMIT);
    assert(form_submit(hlcache_handle_get_url(page),window_list->bw,html->forms,retry)==NSERROR_OK);
}
static _Atomic bool entropy_ready;
static time_t monotonic_shift,eagain_uptime=-1;
static bool monotonic_shifted;
int __real_getentropy(void *,size_t);
int __wrap_getentropy(void *buffer,size_t bytes)
{
    if(!strncmp(fixture_mode,"browser-entropy",15) && !entropy_ready){
        /* Record the simulated uptime the port sees for this refusal. */
        struct timespec now;assert(!clock_gettime(CLOCK_MONOTONIC,&now));eagain_uptime=now.tv_sec;
        errno=EAGAIN;return -1;
    }
    return __real_getentropy(buffer,bytes);
}
struct ciuki_tls *__real_ciuki_tls_create(const char *,const br_x509_trust_anchor *,size_t,enum ciuki_tls_error *);
struct ciuki_tls *__wrap_ciuki_tls_create(const char *host,const br_x509_trust_anchor *anchors,size_t count,enum ciuki_tls_error *error)
{
    /* Only this host harness can add its private fixture CA. Production
     * objects and the installed resource store retain the pinned roots. */
    if(strcmp(fixture_mode,"browser-untrusted")){anchors=ciuki_anchors;count=CIUKI_ANCHOR_COUNT;}
    return __real_ciuki_tls_create(host,anchors,count,error);
}
/* The page must state the remaining part of the first boot minute for the
 * uptime seen at the EAGAIN refusal (a one-second tick in between is
 * tolerated), or that the estimate has elapsed. */
static bool estimate_ok(const uint8_t *source,size_t bytes)
{
    assert(eagain_uptime>=0);
    if(eagain_uptime>=60)return memmem(source,bytes,"The initial one-minute estimate has elapsed; readiness is not guaranteed. Reload to retry.",90);
    char text[96];
    for(time_t seconds=eagain_uptime;seconds<=eagain_uptime+1 && seconds<60;seconds++){
        int n=snprintf(text,sizeof(text),"Estimated initial wait remaining: up to %u seconds. Reload to retry.",(unsigned)(60-seconds));
        if(memmem(source,bytes,text,(size_t)n))return true;
    }
    return false;
}
int __real_pthread_cond_wait(pthread_cond_t *,pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *cond,pthread_mutex_t *lock)
{
    /* The resolver has processed the returned answer and discarded the
     * cancelled slot before its next idle wait. Observe that on its thread. */
    if(fixture_lookup_returned_in_thread())lookup_idle=true;
    return __real_pthread_cond_wait(cond,lock);
}
int __real_main(int,char **);
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *out)
{
    int r=__real_clock_gettime(clock,out);if(!r && clock==CLOCK_REALTIME)out->tv_sec=1790000000;
    /* Simulated boot uptime: shift the real clock instead of freezing it,
     * so NetSurf's scheduler and the fetch timeouts keep advancing. */
    if(!r && clock==CLOCK_MONOTONIC && fixture_mode && !strncmp(fixture_mode,"browser-entropy",15)){
        if(!monotonic_shifted){monotonic_shift=(!strcmp(fixture_mode,"browser-entropy-late")?75:12)-out->tv_sec;monotonic_shifted=true;}
        out->tv_sec+=monotonic_shift;
    }
    return r;
}
bool __real_nsfb_event(nsfb_t *,nsfb_event_t *,int);
bool __wrap_nsfb_event(nsfb_t *fb,nsfb_event_t *event,int timeout)
{
    if((blocked || reload_entropy || budget) && !capture_held){
        assert(ciuki_capture_path);capture_path=ciuki_capture_path;ciuki_capture_path=NULL;capture_held=true;
        __real_clock_gettime(CLOCK_MONOTONIC,&lookup_started);
    }
    if(blocked && !late_checked){
        struct timespec now;__real_clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-lookup_started.tv_sec<10);
    }
    if(blocked && !navigated && fixture_lookup_blocked()){
        assert(window_list);nsurl *url=NULL;
        assert(nsurl_create("http://192.0.2.1:8080/page",&url)==NSERROR_OK);
        struct timespec before,after;__real_clock_gettime(CLOCK_MONOTONIC,&before);
        assert(browser_window_navigate(window_list->bw,url,NULL,BW_NAVIGATE_HISTORY,NULL,NULL,NULL)==NSERROR_OK);
        __real_clock_gettime(CLOCK_MONOTONIC,&after);
        assert((after.tv_sec-before.tv_sec)*1000000000LL+after.tv_nsec-before.tv_nsec<100000000);
        assert(fixture_lookup_blocked());nsurl_unref(url);navigated=true;
    }
    struct hlcache_handle *page=window_list?browser_window_get_content(window_list->bw):NULL;
    if(reload_entropy && page && content_get_status(page)==CONTENT_STATUS_DONE && !reloaded){
        size_t bytes;const uint8_t *source=content_get_source_data(page,&bytes);assert(source);
        assert(memmem(source,bytes,"Secure connections are not ready yet",36) && estimate_ok(source,bytes));
        assert(!fixture_connections() && !fixture_requests());
        reloaded=true;entropy_ready=true;completed_page=page;
        /* The chrome's Reload action, as the user triggers it. */
        ciuki_chrome_reload();page=NULL;
    }
    if(reload_entropy && reloaded && page==completed_page)page=NULL;
    /* The rejected page can reach DONE before its deferred error replaces it. */
    if(budget && page && content_is_layout_rejected(page)){assert(!retried);page=NULL;}
    if(budget && page && content_get_status(page)==CONTENT_STATUS_DONE && !retried){
        /* The rejected page is gone; the browser shows the core's error page. */
        assert(window_list && !strncmp(nsurl_access(hlcache_handle_get_url(page)),"about:query/fetcherror",22));
        size_t bytes;const uint8_t *source=content_get_source_data(page,&bytes);assert(source);
        assert(memmem(source,bytes,"needs more layout time than the browser allows for web content",62));
        assert(fixture_requests()>=1 && ciuki_layout_rejections==1);
        printf("fixture_budget_rejected requests=%u\n",fixture_requests());
        ciuki_layout_clock_scale=1;retried=true;error_page=page;
        try_again(page);page=NULL;
    }
    if(budget && retried && page==error_page)page=NULL;
    if(page && content_get_status(page)==CONTENT_STATUS_DONE && !printed){
        size_t bytes;const uint8_t *source=content_get_source_data(page,&bytes);assert(source);
        if(!strncmp(fixture_mode,"browser-entropy",15) && !reload_entropy){
            assert(!fixture_connections() && !fixture_requests());
            assert(memmem(source,bytes,"Secure connections are not ready yet",36) && estimate_ok(source,bytes));
            puts("fixture_entropy_estimate_ok=1");
        }
        if(reload_entropy){
            assert(reloaded && memmem(source,bytes,"<h1>Ciuki</h1>",14));
            ciuki_capture_path=capture_path;puts("fixture_entropy_reload=1");
        }
        if(budget){
            assert(retried && memmem(source,bytes,"<h1>Ciuki</h1>",14) && ciuki_layout_rejections==1);
            assert(!strcmp(nsurl_access(browser_window_access_url(window_list->bw)),"http://fixture.example:8080/page"));
            retry_done=true;ciuki_capture_path=capture_path;printf("fixture_budget_retry=1 requests=%u\n",fixture_requests());
        }
        if(strstr(nsurl_access(browser_window_access_url(window_list->bw)),"/gzip64")){
            assert(bytes==65536 && memmem(source,bytes,"<h1>Ciuki</h1>",14));
            printf("fixture_tls_gzip_bytes=%zu\n",bytes);
        }
        puts("fixture_page_begin");fwrite(source,1,bytes,stdout);puts("\nfixture_page_end");printed=true;
        if(blocked){
            assert(navigated && memmem(source,bytes,"<h1>Ciuki</h1>",14));
            completed=true;completed_page=page;
        }
    }
    if(blocked && completed){
        assert(page==completed_page && content_get_status(page)==CONTENT_STATUS_DONE);
        assert(!strcmp(nsurl_access(browser_window_access_url(window_list->bw)),"http://192.0.2.1:8080/page"));
        if(!lookup_released){
            /* NetSurf also fetches /favicon.ico. Let that expected request
             * arrive before taking the pre-release network baseline. */
            assert(fixture_requests()<=2);
            if(fixture_requests()==2){
                completed_requests=fixture_requests();completed_connections=fixture_connections();
                lookup_released=true;fixture_lookup_release();
            }
        } else assert(fixture_requests()==completed_requests && fixture_connections()==completed_connections);
        /* Keep the ordinary browser event/fetch pump running after the late
         * result has been consumed. Capture/exit only after further UI turns. */
        if(lookup_idle && ++late_turns>=5){late_checked=true;ciuki_capture_path=capture_path;}
    }
    return __real_nsfb_event(fb,event,timeout);
}
int __wrap_main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    const char *mode=getenv("CIUKI_FIXTURE_MODE");if(!mode)mode="browser";
    fixture_mode=mode;reload_entropy=!strcmp(mode,"browser-entropy-reload");
    budget=!strcmp(mode,"browser-budget");
    /* Every remote layout exceeds 75 ms of budget time; local pages, such as
     * the error page, are never time-limited. */
    if(budget)ciuki_layout_clock_scale=1000000;
    const char *cert=getenv("CIUKI_FIXTURE_CERT"),*key=getenv("CIUKI_FIXTURE_KEY");
    blocked=!strcmp(mode,"browser-blocked");fixture_begin(mode,cert?cert:"-",key?key:"-");
    int result=__real_main(argc,argv);
    assert(printed);
    if(blocked){assert(navigated && completed && lookup_released && lookup_idle && late_checked);puts("fixture_navigation_blocked=1 completed=1 late_ignored=1");}
    if(budget)assert(retried && retry_done);
    printf("fixture_requests=%u fixture_loop_requests=%u\n",fixture_requests(),fixture_loops());
    fixture_end();return result;
}
