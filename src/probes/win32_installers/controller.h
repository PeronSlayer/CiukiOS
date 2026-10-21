/* Independent installer observations. No installer-supplied success counts.
 * The authenticated desktop queries w32srv; the kernel reads the files at
 * each synchronous checkpoint before letting the driver advance.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_INSTALLER_CONTROLLER_H
#define CIUKI_INSTALLER_CONTROLLER_H
#include <stdint.h>
#define INSTALLER_PATH "/System/Win32/Fixtures/ciuki-installer-fixture.exe"
#define INSTALLER_DIR "C:\\Program Files\\Ciuki Installer Fixture"
#define INSTALLER_UNINSTALL "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CiukiInstallerFixture"
#define INSTALLER_RUNONCE "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"
#define INSTALLER_PAYLOAD "CiukiOS authored installer fixture. No commercial application data.\n"
enum { IF_PAYLOAD=1, IF_CHILD=2, IF_UNINSTALL=4, IF_DESKTOP=8, IF_MENU=16,
    IF_COPY=32, IF_REMOVE=64, IF_RESTARTED=128, IF_INI=256, IF_BACKUP=512,
    IF_JOURNAL=1024, IF_DIRECTORY=2048 };
#define IF_BASE (IF_PAYLOAD|IF_CHILD|IF_UNINSTALL|IF_DESKTOP|IF_MENU)
#define IF_CONTENT (IF_PAYLOAD|IF_DESKTOP|IF_MENU)
struct installer_observation { uint32_t files, contents, registry_errors, exit_code; };
struct installer_result { uint32_t stage, errors[4], seen_journal, complete; };
static inline unsigned installer_observe(struct installer_result *r,unsigned stage,const struct installer_observation *o)
{
    if(stage!=r->stage+1 || stage>8)return 0;
    unsigned errors=o->registry_errors,slot=stage==1?1:stage==2?0:stage<=5?2:3;
    if(stage==1)errors+=!!(o->files&(IF_BASE|IF_DIRECTORY)) || o->exit_code!=1;
    if(stage==2 || stage==3)errors+=(o->files&IF_BASE)!=IF_BASE || (o->contents&IF_CONTENT)!=IF_CONTENT || o->exit_code;
    if(stage==3)errors+=(o->files&(IF_REMOVE|IF_INI))!=(IF_REMOVE|IF_INI) || !(o->contents&IF_INI);
    if(stage==4 || stage==7){if(o->files&IF_JOURNAL)r->seen_journal|=stage==4?1u:2u;else errors++;}
    if(stage==5)errors+=!(r->seen_journal&1) || !(o->files&IF_RESTARTED) || !(o->files&IF_BACKUP) ||
        !!(o->files&(IF_REMOVE|IF_INI|IF_JOURNAL));
    if(stage==6)errors+=o->exit_code || !!(o->files&(IF_BASE|IF_DIRECTORY)) ||
        (o->files&(IF_COPY|IF_INI))!=(IF_COPY|IF_INI) || !(o->contents&IF_INI);
    if(stage==8)errors+=!(r->seen_journal&2) || !!(o->files&(IF_BASE|IF_DIRECTORY|IF_COPY|IF_INI|IF_JOURNAL)) || !(o->files&IF_BACKUP);
    r->errors[slot]+=errors;r->stage=stage;r->complete=stage==8;return 1;
}
static inline void installer_publish(const struct installer_result *r,void (*emit)(const char *,const char *,const char *,...))
{
    const char *names[]={"installer_install","installer_cancel","installer_restart_request","installer_uninstall"};
    for(unsigned i=0;i<4;i++){
        unsigned errors=r->errors[i]+!r->complete;
        emit("w32-installer-nsis","DATA","case=%s errors=%u manifest=%u",names[i],errors,errors?0u:1u);
    }
}
#endif
