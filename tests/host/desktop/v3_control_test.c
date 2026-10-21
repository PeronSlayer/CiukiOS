/* SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include <errno.h>
static unsigned removed;
static int unavailable;
static int entries(void *ctx,struct desk_uninstall_entry *out,unsigned cap)
{
    (void)ctx; assert(cap==CP_PROGRAM_MAX); if (unavailable) return -ENOSYS;
    out[0]=(struct desk_uninstall_entry){.key="HKLM/Software/Microsoft/Windows/CurrentVersion/Uninstall/Game",.name="Example Game",.command="\"C:\\Games\\Game\\uninstall.exe\" /remove"};
    out[1]=(struct desk_uninstall_entry){.key="HKCU/Software/Microsoft/Windows/CurrentVersion/Uninstall/Tools",.name="Example Tools",.command="C:\\Tools\\remove.exe"};
    return 2;
}
static int remove_program(void *ctx,const char *key)
{ (void)ctx; assert(strstr(key,"/Tools")); removed++; return 0; }
int main(int argc,char **argv)
{
    assert(argc==2); static struct cp_app a; static struct du_ui u; static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
    assert(cp_page_index("programs")==CP_UNINSTALL);
    const char *themes[]={"ciuki-blue","platinum"};
    for (unsigned t=0;t<2;t++) {
        memset(&a,0,sizeof(a)); a.host.uninstall_list=entries; a.host.uninstall_run=remove_program;
        du_init(&u,pixels,CP_WIDTH,CP_HEIGHT); u.focused_window=1; assert(!cp_attach(&a,&u,CP_UNINSTALL)); du_theme_use(&u,themes[t]);
        du_render(&u); char name[80]; snprintf(name,sizeof(name),"%s-add-remove",themes[t]); at_save(argv[1],name,&u);
        assert(a.uninstall_count==2 && du_get(&u,982)->count==2);
        struct du_event select={.type=DU_EV_SELECT,.id=982,.index=1}; u.event(&u,&select,u.user);
        struct du_event run={.type=DU_EV_ACTIVATE,.id=983}; u.event(&u,&run,u.user); assert(removed==t && du_dialog_id(&u)==987);
        struct du_rect button=du_dialog_button(&u,0);at_click(&u,button.x+button.w/2,button.y+button.h/2);
        assert(removed==t+1);
        unavailable=1; struct du_event refresh={.type=DU_EV_ACTIVATE,.id=984}; u.event(&u,&refresh,u.user);
        assert(a.uninstall_count==0 && du_get(&u,983)->disabled); unavailable=0;
        cp_detach(&a);
    }
    puts("PASS Add/Remove RPC view, selected key launch, missing service, two themes"); return 0;
}
