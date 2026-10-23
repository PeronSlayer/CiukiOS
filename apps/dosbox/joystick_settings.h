/* Per-game logical joystick subscription. SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_JOYSTICK_SETTINGS_H
#define CIUKI_DOSBOX_JOYSTICK_SETTINGS_H
#include "settings.h"
static inline int dosbox_joystick_path(unsigned game, char *out, size_t size)
{
    if (!game || game>DESK_GAME_COUNT) { errno=EINVAL; return -1; }
    int n=snprintf(out,size,DOSBOX_SETTINGS_DIR "/dos-game-%u-joystick.cfg",game);
    if (n<0 || (size_t)n>=size) { errno=ENAMETOOLONG; return -1; }
    return 0;
}
/* Exact six unsigned decimal words and LF; defaults never overwrite memory
 * or networking. A stored stale generation is rejected by CREATE. */
static inline void dosbox_joystick_load(unsigned game, struct ciuki_dosvm_command *c)
{
    char path[144],text[128],canonical[128];
    if (dosbox_joystick_path(game,path,sizeof(path))) return;
    FILE *f=fopen(path,"rb"); if (!f) return;
    size_t n=fread(text,1,sizeof(text)-1,f); int e=ferror(f); int end=feof(f); fclose(f);
    if (e || !end) return;
    unsigned words[6]={0}; size_t at=0;
    for (unsigned i=0;i<6;i++) {
        size_t begin=at;
        while (at<n && text[at]>='0' && text[at]<='9') {
            unsigned digit=(unsigned)(text[at++]-'0');
            if (words[i]>(UINT32_MAX-digit)/10) return;
            words[i]=words[i]*10+digit;
        }
        if (at==begin || at>=n || text[at++]!=(i==5 ? '\n' : ' ')) return;
    }
    unsigned d=words[0],g=words[1],b=words[2],bg=words[3],mode=words[4],session=words[5];
    if (at!=n || mode>1 || (d!=CIUKI_JOY_DISABLED && d>CIUKI_JOYSTICK_LAST) || b>CIUKI_JOYSTICK_LAST) return;
    if ((!d || d==CIUKI_JOY_DISABLED) ? (g || b || bg || mode || session) :
        (!g || !session || (b ? (!mode || !bg) : bg))) return;
    int size=snprintf(canonical,sizeof(canonical),"%u %u %u %u %u %u\n",d,g,b,bg,mode,session);
    if (size<0 || (size_t)size!=n || memcmp(text,canonical,n)) return;
    c->launch.reserved[1]=d; c->launch.reserved[2]=g;
    c->launch.reserved[3]=b; c->launch.reserved[4]=bg; c->launch.reserved[5]=mode; c->launch.reserved[6]=session;
}
static inline int dosbox_joystick_save(unsigned game, const struct ciuki_joystick_control *c)
{
    char path[144],text[128];
    if (dosbox_joystick_path(game,path,sizeof(path))) return -errno;
    if (c->device && c->device!=CIUKI_JOY_DISABLED && !c->session) return -EAGAIN;
    int n=snprintf(text,sizeof(text),"%u %u %u %u %u %u\n",c->device,c->generation,c->second_device,c->second_generation,c->mode,c->session);
    if (n<0 || (size_t)n>=sizeof(text)) return -EINVAL;
    FILE *f=fopen(path,"wb"); if (!f) return -errno;
    int error=fwrite(text,1,(size_t)n,f)!=(size_t)n || fflush(f) || fsync(fileno(f));
    if (fclose(f)) error=1;
    return error ? -EIO : 0;
}
#endif
