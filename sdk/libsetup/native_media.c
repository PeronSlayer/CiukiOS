/* Native boot-CD control uses the existing public media ABI. No guessed
 * device slot: bind the mounted Setup ISO (D:) before retaining its slot.
 * POWER currently refuses Setup; preserve that error for the lead to fix.
 * SPDX-License-Identifier: MIT */
#include "internal.h"
#include <ciuki/abi.h>
#include <errno.h>
int open(const char *, int, ...);
int close(int);
int ciuki_devctl(int, uint32_t, void *, uint32_t);
int ciuki_power(uint32_t);
int st_native_media(void *ctx,bool tray,bool *removed,uint32_t *generation)
{
    (void)ctx;
    static unsigned boot_slot=DISK_SLOTS;
    int h=open("/dev/media",O_RDONLY|O_CLOEXEC); if(h<0)return -errno;
    int e=0;
    struct ciuki_media_cd_state state={0};
    if(boot_slot==DISK_SLOTS) {
        for(unsigned slot=0;slot<DISK_SLOTS;slot++) {
            state=(struct ciuki_media_cd_state){.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(state),.slot=slot};
            if(ciuki_devctl(h,MEDIA_CD_STATE,&state,sizeof(state))) { e=-errno; break; }
            if(state.present && state.mounted && state.drive==3) { boot_slot=slot; break; }
        }
        if(!e && boot_slot==DISK_SLOTS)e=-ENODEV;
    }
    if(!e && tray) {
        struct ciuki_media_cd_tray q={.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(q),.slot=boot_slot,.action=CIUKI_CD_TRAY_OPEN};
        if(ciuki_devctl(h,MEDIA_CD_TRAY,&q,sizeof(q)))e=-errno;
    }
    if(!e) {
        state=(struct ciuki_media_cd_state){.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(state),.slot=boot_slot};
        if(ciuki_devctl(h,MEDIA_CD_STATE,&state,sizeof(state)))e=-errno;
        else { *removed=!state.present || state.state==CIUKI_CD_NO_MEDIUM; *generation=state.generation; }
    }
    close(h); return e;
}
int st_native_restart(void *ctx) { (void)ctx; return ciuki_power(CIUKI_POWER_REBOOT)<0 ? -errno : 0; }
