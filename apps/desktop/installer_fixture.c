/* Runtime-only authored installer driver. The child never supplies its
 * manifest. Registry reads use w32srv's copied directory protocol.
 * SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include "installer_fixture.h"
#include "../../src/probes/win32_installers/controller.h"
#include <ciuki/spawn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern void desk_v3_fixture_report(const char *);
extern int ciuki_win32_shell_restart(uint32_t,uint32_t);
static struct { unsigned active,stage,pid,ui_keys; uint64_t deadline,next_ui; } install;
static struct installer_checkpoint *result;
int desk_installer_fixture_active(void){return (int)install.active;}
int desk_installer_fixture_begin(struct desktop *d){
    if(result || install.active)return -EBUSY;
    /* The selected kernel probe maps this private page before issuing the
     * shared gate command. Native mmap's arena starts at 0x20000000. */
    result=(void *)(uintptr_t)INSTALLER_RESULT_VA;
    memset(result,0,sizeof(*result));
    install=(__typeof__(install)){.active=1,.deadline=d->now_ns+15000000000ull};return 0;
}
static void finish(void)
{ install.active=0;result=0; } /* kernel tears down the owning desktop */
static void checkpoint(unsigned stage,unsigned status,unsigned errors)
{
    *result=(struct installer_checkpoint){INSTALLER_CHECKPOINT_MAGIC,stage,install.pid,status,errors,install.ui_keys};
    char line[128];snprintf(line,sizeof(line),"case=w32-installer stage=%u child=%u exit=%u registry_errors=%u ui_keys=%u",stage,install.pid,status,errors,install.ui_keys);
    desk_v3_fixture_report(line);
}
static int spawn(unsigned stage)
{
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs",NULL};
    char *args[]={INSTALLER_PATH,stage==2?NULL:"/S",stage==1?"/CANCEL":stage==3?"/RESTART":NULL,NULL};
    if(stage==6){args[0]="/WINDOWS/TEMP/CiukiInstallerFixture-uninstall.exe";args[2]="_?=" INSTALLER_DIR;}
    int pid=ciuki_spawn(args[0],args,env,NULL,0,0);
    if(pid>0){install.pid=(unsigned)pid;install.ui_keys=0;install.next_ui=0;char line[96];snprintf(line,sizeof(line),"case=w32-installer-child child=%u",install.pid);desk_v3_fixture_report(line);}return pid;
}
static void pages(struct desktop *d)
{
    /* The normal install displays components/directory/instfiles. Drive
     * the actual dialog input path once a frame exists; no script-side
     * reentrant SendMessage and no synthesized completion. */
    if(install.stage!=2 || d->now_ns<install.next_ui)return;
    for(int i=0;i<DESK_CLIENTS;i++){
        struct desk_client *c=&d->clients[i];
        if(c->pid!=install.pid || !c->window || !c->v3_win32_hwnd || c->awaiting_frame || c->closing || c->minimized)continue;
        desk_focus(d,i);
        struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.monotonic_ns=d->now_ns,.code=28,.value=1};
        desk_input(d,&e);e.value=0;desk_input(d,&e);
        install.ui_keys++;install.next_ui=d->now_ns+500000000ull;return;
    }
}
static int copy_uninstaller(void)
{
    int in=open("/Program Files/Ciuki Installer Fixture/uninstall.exe",O_RDONLY);if(in<0)return -1;
    int out=open("/WINDOWS/TEMP/CiukiInstallerFixture-uninstall.exe",O_WRONLY|O_CREAT|O_EXCL,0600);if(out<0){close(in);return -1;}
    char data[1024];ssize_t n;int e=0;
    while((n=read(in,data,sizeof(data)))>0){ssize_t at=0;while(at<n){ssize_t w=write(out,data+at,(size_t)(n-at));if(w<=0){e=1;break;}at+=w;}if(e)break;}
    if(n<0 || fsync(out))e=1;close(in);if(close(out))e=1;return e?-1:0;
}
void desk_installer_fixture_tick(struct desktop *d)
{
    if(!install.active)return;
    if(d->now_ns>=install.deadline){checkpoint(99,~0u,1);finish();return;}
    if(!install.stage){
        if(!desk_installer_ready())return;
        install.stage=1;install.deadline=d->now_ns+120000000000ull;
        if(spawn(1)<0){checkpoint(99,~0u,1);finish();}return;
    }
    if(install.stage==4 || install.stage==7){
        struct stat st;if(!stat("/WINDOWS/CIUKIRST.LOG",&st)){checkpoint(install.stage,0,0);install.stage++;}return;
    }
    if(install.stage==5 || install.stage==8){
        struct stat st;int ready=stat("/WINDOWS/CIUKIRST.LOG",&st)<0 && errno==ENOENT;
        if(install.stage==5)ready=ready && !stat("/WINDOWS/TEMP/CiukiInstallerFixture.restarted",&st) && stat("/WINDOWS/TEMP/CiukiInstallerFixture.remove",&st)<0 && errno==ENOENT;
        else ready=ready && stat("/WINDOWS/TEMP/CiukiInstallerFixture-uninstall.exe",&st)<0 && errno==ENOENT;
        if(!ready)return;
        checkpoint(install.stage,0,desk_installer_registry(install.stage));
        if(install.stage==8){finish();return;}
        install.stage=6;
        if(copy_uninstaller() || spawn(6)<0){checkpoint(99,~0u,1);finish();}return;
    }
    pages(d);
    int status=0,pid=waitpid((int)install.pid,&status,WNOHANG);if(!pid)return;
    unsigned code=pid<0 || !WIFEXITED(status)?~0u:(unsigned)WEXITSTATUS(status);
    unsigned errors=desk_installer_registry(install.stage);
    checkpoint(install.stage,code,errors);
    if(code!=(install.stage==1?1u:0u) || errors){finish();return;}
    if(install.stage==3 || install.stage==6){
        install.stage++;if(ciuki_win32_shell_restart(0,2)<0){checkpoint(99,~0u,1);finish();}return;
    }
    install.stage++;if(spawn(install.stage)<0){checkpoint(99,~0u,1);finish();}
}
