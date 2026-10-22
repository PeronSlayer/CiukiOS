/* Independent installer observations. No installer-supplied success counts.
 * The authenticated desktop queries w32srv; the kernel reads the files at
 * each synchronous checkpoint before letting the driver advance.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_INSTALLER_CONTROLLER_H
#define CIUKI_INSTALLER_CONTROLLER_H
#include <stdint.h>
#define INSTALLER_FIXTURE_NAME "ciuki-installer-fixture"
#define INSTALLER_RESULT_VA 0x100f0000u
#define INSTALLER_CHECKPOINT_MAGIC 0x49353146u
struct installer_checkpoint {
    uint32_t magic,stage,child,exit_code,registry_errors,ui_keys;
};
#define INSTALLER_PATH "/System/Win32/Fixtures/ciuki-installer-fixture.exe"
#define INSTALLER_DIR "C:\\Program Files\\Ciuki Installer Fixture"
#define INSTALLER_UNINSTALL "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CiukiInstallerFixture"
#define INSTALLER_RUNONCE "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"
#define INSTALLER_PAYLOAD "CiukiOS authored installer fixture. No commercial application data.\n"
enum { IF_PAYLOAD=1, IF_CHILD=2, IF_UNINSTALL=4, IF_DESKTOP=8, IF_MENU=16,
    IF_COPY=32, IF_REMOVE=64, IF_RESTARTED=128, IF_INI=256, IF_BACKUP=512,
    IF_JOURNAL=1024, IF_DIRECTORY=2048,
    IF_LINE_APPLIED=4096, IF_RETIRED=8192 }; /* verified journal progress, contents only */
#define IF_BASE (IF_PAYLOAD|IF_CHILD|IF_UNINSTALL|IF_DESKTOP|IF_MENU)
#define IF_CONTENT IF_BASE
struct installer_observation { uint32_t files, contents, registry_errors, exit_code; };
struct installer_result { uint32_t stage, errors[4], seen_journal, complete; };
static inline unsigned installer_observe(struct installer_result *r,unsigned stage,const struct installer_observation *o)
{
    if(stage!=r->stage+1 || stage>8)return 0;
    unsigned errors=o->registry_errors,slot=stage==1?1:stage==2?0:stage<=5?2:3;
    static const uint32_t required[]={0,
        IF_BASE|IF_DIRECTORY,
        IF_BASE|IF_DIRECTORY|IF_REMOVE|IF_INI,
        IF_BASE|IF_DIRECTORY|IF_REMOVE|IF_INI|IF_JOURNAL,
        IF_BASE|IF_DIRECTORY|IF_RESTARTED|IF_BACKUP,
        IF_COPY|IF_INI|IF_BACKUP|IF_RESTARTED,
        IF_COPY|IF_INI|IF_BACKUP|IF_RESTARTED|IF_JOURNAL,
        IF_BACKUP|IF_RESTARTED};
    uint32_t expected=required[stage-1],content=expected&~IF_DIRECTORY;
    /* w32srv advances WININIT independently of the desktop. Before RunOnce
     * handoff the journal may describe any of these three coherent states. */
    if(stage==4 || stage==7){
        errors+=!!(o->contents&IF_RETIRED) && !(o->contents&IF_LINE_APPLIED);
        if(o->contents&IF_LINE_APPLIED)expected&=~(stage==4?IF_REMOVE:IF_COPY);
        if(o->contents&IF_RETIRED){expected&=~IF_INI;expected|=IF_BACKUP;}
        content=expected&~IF_DIRECTORY;
    }
    errors+=o->files!=expected || (o->contents&content)!=content || o->exit_code!=(stage==1?1u:0u);
    if(stage==4 || stage==7){
        if((o->contents&IF_JOURNAL) && (o->files&IF_JOURNAL))r->seen_journal|=stage==4?1u:2u;
        else errors++;
    }
    if(stage==5)errors+=!(r->seen_journal&1);
    if(stage==8)errors+=!(r->seen_journal&2);
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
