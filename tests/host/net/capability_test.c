/* Exercise the production exact-path delegation policy. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "../security/subject_fixture.h"
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct process supervisor,parent,child;
static struct desktop_description display,input;
static bool have_display,have_input;
struct process *proc_supervisor(void){return &supervisor;}
struct desktop_description *desktop_fd(const struct process *p,int32_t fd)
{
    if(p!=&parent)return NULL;
    return fd==3 && have_display ? &display : fd==4 && have_input ? &input : NULL;
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);parent.pid=42;display.owner=input.owner=42;
    display.kind=DESKTOP_DISPLAY;input.kind=DESKTOP_INPUT;
    security_fixture_identity(&child,1000,1,true,0);
    const char *paths[]={"/Programs/desktop","/Programs/control","/Programs/control-copy","/Programs/netsurf","/Programs/terminal","/Programs/msconfig"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        supervisor_dosvm_delegate(&supervisor,&child,paths[i]);
        assert(child.network_capability && child.dhcp_capability && child.time_capability && child.recovery_capability);
    }
    security_fixture_identity(&child,1001,2,false,0);
    have_display=have_input=true;
    parent.network_capability=parent.dhcp_capability=parent.time_capability=parent.recovery_capability=true;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        supervisor_dosvm_delegate(&parent,&child,paths[i]);
        assert(!child.network_capability && !child.dhcp_capability && !child.time_capability && !child.recovery_capability);
    }
    puts("PASS sec-k04 capability compatibility: credential rights independent of path, parent flags and grants");return 0;
}
