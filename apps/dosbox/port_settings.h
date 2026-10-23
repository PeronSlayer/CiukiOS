/* Worker-only user-side persistence. SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_PORT_SETTINGS_H
#define CIUKI_DOSBOX_PORT_SETTINGS_H
#include "settings.h"
#include "../../src/kernel/include/ciuki/game_ports_settings.h"
static inline int dosbox_ports_path(unsigned game,unsigned slot,char path[160])
{
    if (!game || game>DESK_GAME_COUNT || slot>1) return -EINVAL;
    snprintf(path,160,DOSBOX_SETTINGS_DIR "/dos-game-%u-ports.%u.ini",game,slot);return 0;
}
/* Defaults or previous complete slot on corruption; never rewrite on load. */
static inline int dosbox_ports_load(unsigned game,struct game_ports_settings *out,int *rejected)
{
    game_ports_defaults(out); *rejected=0;
    for (unsigned slot=0;slot<2;slot++) {
        char path[160],text[GAME_PORTS_TEXT_MAX];struct game_ports_settings p;
        int e=dosbox_ports_path(game,slot,path);if (e) return e;
        FILE *f=fopen(path,"rb");if (!f) { if (errno==ENOENT) continue;return -errno; }
        size_t n=fread(text,1,sizeof(text),f);int error=ferror(f);int closed=fclose(f);
        if (error || closed || !game_ports_decode(text,n,&p) || (p.sequence&1)!=slot) { *rejected=1;continue; }
        if (p.sequence>out->sequence) *out=p;
    }
    return 0;
}
static inline int dosbox_ports_save(unsigned game,struct game_ports_settings *p)
{
    if (!game_ports_valid(p)) return -EINVAL;
    struct game_ports_settings old;int rejected;
    int e=dosbox_ports_load(game,&old,&rejected);if (e) return e;
    if (old.sequence==UINT32_MAX) return -EOVERFLOW;
    p->sequence=old.sequence+1;
    char path[160],text[GAME_PORTS_TEXT_MAX];e=dosbox_ports_path(game,p->sequence&1,path);if (e) return e;
    size_t n=game_ports_encode(p,text);
    FILE *f=fopen(path,"wb");if (!f) return -errno;
    int error=fwrite(text,1,n,f)!=n || fflush(f) || fsync(fileno(f));
    if (fclose(f)) error=1;
    return error ? -EIO : 0;
}
#endif
