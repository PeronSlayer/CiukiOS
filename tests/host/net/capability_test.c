/* Exercise the production exact-path delegation policy. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
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
    supervisor_dosvm_delegate(&supervisor,&child,"/Programs/desktop");assert(child.network_capability);
    supervisor_dosvm_delegate(&supervisor,&child,"/Programs/control");assert(child.network_capability);
    supervisor_dosvm_delegate(&supervisor,&child,"/Programs/control-copy");assert(!child.network_capability);
    supervisor_dosvm_delegate(&supervisor,&child,"/Programs/netsurf");assert(!child.network_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Programs/control");assert(!child.network_capability);
    parent.network_capability=true;have_display=true;
    supervisor_dosvm_delegate(&parent,&child,"/Programs/control");assert(!child.network_capability);
    have_input=true;input.owner=99;
    supervisor_dosvm_delegate(&parent,&child,"/Programs/control");assert(!child.network_capability);
    input.owner=42;supervisor_dosvm_delegate(&parent,&child,"/Programs/control");assert(child.network_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Programs/netsurf");assert(!child.network_capability);
    puts("PASS f2-100 capability delegation: supervisor/exact paths, both owner-bound desktop grants, no inheritance through Control Panel or ordinary programs");return 0;
}
