/* Check the authored caller's real queue callback contract/lifetime.
 * This is a caller unit check, not evidence of INF engine execution.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <w32/shell.h>
#include "../../../win32/fixtures/installers/inf_sections.h"
static unsigned calls,created,closed,fail_at,init_failure;
static int token;
static void *W32API init(HANDLE owner){assert(!owner);created++;return init_failure?0:&token;}
static void W32API term(void *context){assert(context==&token);closed++;}
static DWORD W32API callback(void *context,DWORD notification,uintptr_t p1,uintptr_t p2){
    assert(context==&token && notification==1 && !p1 && !p2);return 1;
}
static BOOL W32API install(HANDLE owner,HANDLE hinf,const char *section,DWORD flags,HANDLE root,
                          const char *tree,DWORD copy,inf_file_callback cb,void *context,HANDLE device,void *data){
    /* SPINST_INIFILES is 2; 1 requests device LogConfig and silently skips INI. */
    static const DWORD expected[]={0x10,4,2};
    assert(!owner && hinf==(HANDLE)17 && !strcmp(section,"DefaultInstall") &&
           root==(HANDLE)(uintptr_t)0x80000002u && !strcmp(tree,"fixture tree") && !copy && !device && !data);
    assert(calls<3 && flags==expected[calls]);calls++;
    if(flags==0x10){assert(cb==callback && context==&token);assert(cb(context,1,0,0));}
    else assert(!cb && !context);
    return calls!=fail_at;
}
int main(void){
    const struct inf_queue_api queue={init,callback,term};
    for(fail_at=0;fail_at<=3;fail_at++){
        calls=created=closed=0;
        assert(install_sections((HANDLE)17,"fixture tree",install,&queue)==(fail_at?7+(int)fail_at:0));
        assert(created==1 && closed==1 && calls==(fail_at?fail_at:3));
    }
    calls=created=closed=0;init_failure=1;
    assert(install_sections((HANDLE)17,"fixture tree",install,&queue)==27);
    assert(created==1 && !closed && !calls);
    puts("PASS INF caller supplies the required file callback/context and closes it on every path");
}
