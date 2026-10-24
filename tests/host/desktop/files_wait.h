/* SPDX-License-Identifier: MIT
 * Existing frame/script tests wait at action boundaries. Responsiveness tests
 * deliberately do not use these wrappers. No production synchronous mode. */
#include <time.h>
static void files_test_wait(struct files_app *a)
{
    for (unsigned i=0; a->reading && i<5000; i++) {
        struct du_event tick={.type=DU_EV_TICK,.id=3};
        files_event(a->ui,&tick,a);
        if (a->reading) { struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL); }
    }
    assert(!a->reading);
}
static int files_test_setup(struct files_app *a,struct du_ui *u,const struct fm_vfs *fs,const char *path)
{ int r=files_setup(a,u,fs,path); if (!r) files_test_wait(a); return r; }
static int files_test_navigate(struct files_app *a,const char *path)
{ int r=files_navigate(a,path); if (!r) files_test_wait(a); return r; }
static void files_test_event(struct du_ui *u,const struct du_event *e,void *arg)
{
    /* An explicit media tick in a script represents the next one-second poll.
     * Real-time responsiveness/rate-limit tests do not use this wrapper. */
    if (e->type==DU_EV_TICK && e->id==2) u->now+=1000000000ull;
    files_event(u,e,arg); files_test_wait(arg);
}
#define files_setup files_test_setup
#define files_navigate files_test_navigate
#define files_event files_test_event
