/* Private authored fixture protocol. SPDX-License-Identifier: MIT */
#ifndef CIUKI_W32_RESTART_CONTROLLER_H
#define CIUKI_W32_RESTART_CONTROLLER_H
#include <stdint.h>
#define W32_RESTART_RESULT_VA 0x10050000u
#define W32_RESTART_MAGIC 0x54535257u
#define W32_RESTART_PATH "/System/Win32/Fixtures/restart-exercise.exe"
#define W32_RESTART_EXE "C:\\System\\Win32\\Fixtures\\restart-exercise.exe"
#define W32_RESTART_TEMP "C:\\WINDOWS\\TEMP\\"
#define W32_RESTART_STATE W32_RESTART_TEMP "f507d.state"
#define W32_RESTART_ORDER W32_RESTART_TEMP "f507d.order"
#define W32_RESTART_JOURNAL "C:\\WINDOWS\\CIUKIRST.LOG"
#define W32_RESTART_INI "C:\\WINDOWS\\WININIT.INI"
#define W32_RESTART_GROUP "C:\\WINDOWS\\Start Menu\\Programs\\F507D"
enum { RESTART_DDE, RESTART_SESSION, RESTART_WININIT, RESTART_RUNONCE,
       RESTART_RECOVERY, RESTART_DESKTOP, RESTART_CASES };
struct w32_restart_case { uint32_t checked, errors; };
struct w32_restart_result {
    uint32_t magic, version, done, release, phase, first_error;
    struct w32_restart_case cases[RESTART_CASES];
    uint32_t raw_commands, ddeml_commands, groups, links;
    uint32_t queries, cancelled, accepted, logoff;
    uint32_t registry_absent, helper, interrupt, checkpoint;
};
struct w32_restart_state { uint32_t magic, version, next; struct w32_restart_result result; };
/* Kernel facts are independent of the fixture's checked flags. */
struct w32_restart_facts {
    uint32_t dde, session, wininit, runonce, recovery, desktop;
};
static inline int w32_restart_valid(const struct w32_restart_result *r)
{ return r->magic==W32_RESTART_MAGIC && r->version==1 && r->done==1 && r->phase==3; }
static inline int w32_restart_publish(const struct w32_restart_result *r,
    const struct w32_restart_facts *f, void (*emit)(const char *,const char *,const char *,...))
{
    static const char *const names[]={"progman_dde","end_session","wininit_restart","runonce_restart","restart_recovery","desktop_resumed"};
    const uint32_t facts[]={f->dde,f->session,f->wininit,f->runonce,f->recovery,f->desktop};
    int pass=w32_restart_valid(r);
    for(unsigned i=0;i<RESTART_CASES;i++){
        uint32_t checked=pass && r->cases[i].checked==1 && facts[i]==1;
        uint32_t errors=r->cases[i].errors+!checked;
        emit("w32-restart","DATA","case=%s errors=%u checked=%u",names[i],errors,checked);
    }
    for(unsigned i=0;i<RESTART_CASES;i++)if(r->cases[i].checked!=1 || r->cases[i].errors || facts[i]!=1)pass=0;
    return pass;
}
_Static_assert(sizeof(struct w32_restart_result)==120,"fixture/controller layout, version 1");
#endif
