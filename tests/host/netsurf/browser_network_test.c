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
{ int r=__real_clock_gettime(clock,out);if(!r && clock==CLOCK_REALTIME)out->tv_sec=1790000000;return r; }
bool __real_nsfb_event(nsfb_t *,nsfb_event_t *,int);
bool __wrap_nsfb_event(nsfb_t *fb,nsfb_event_t *event,int timeout)
{
    if(blocked && !capture_held){
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
    if(page && content_get_status(page)==CONTENT_STATUS_DONE && !printed){
        size_t bytes;const uint8_t *source=content_get_source_data(page,&bytes);assert(source);
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
    blocked=!strcmp(mode,"browser-blocked");fixture_begin(mode,"-","-");
    int result=__real_main(argc,argv);
    assert(printed);
    if(blocked){assert(navigated && completed && lookup_released && lookup_idle && late_checked);puts("fixture_navigation_blocked=1 completed=1 late_ignored=1");}
    printf("fixture_requests=%u fixture_loop_requests=%u\n",fixture_requests(),fixture_loops());
    fixture_end();return result;
}
