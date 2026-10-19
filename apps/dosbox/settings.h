/* Per-game launcher preferences under the existing system settings directory.
 * Exact catalog commands identify games; display titles and private paths never
 * become file names or evidence. SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_SETTINGS_H
#define CIUKI_DOSBOX_SETTINGS_H
#include <ciuki/abi.h>
#include "../desktop/catalog.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DOSBOX_SETTINGS_DIR
#define DOSBOX_SETTINGS_DIR "/system/settings"
#endif
static inline unsigned dosbox_game(const char *command)
{
    for (unsigned i=0;i<DESK_GAME_COUNT;i++)
        if (!strcmp(command,desk_games[i].command)) return i+1;
    return 0;
}
static inline unsigned dosbox_kernel_value(const char *value)
{ return value && !strcmp(value,"freedos-1.3") ? CIUKI_DOSVM_FREEDOS13 : CIUKI_DOSVM_CIUKIDOS; }
static inline const char *dosbox_kernel_label(unsigned value)
{ return value==CIUKI_DOSVM_FREEDOS13 ? "FreeDOS 1.3" : "CiukiDOS (default)"; }
static inline int dosbox_setting_path(unsigned game,char *out,size_t size)
{
    if (!game || game>DESK_GAME_COUNT) { errno=EINVAL; return -1; }
    int n=snprintf(out,size,DOSBOX_SETTINGS_DIR "/dos-game-%u.cfg",game);
    if (n<0 || (size_t)n>=size) { errno=ENAMETOOLONG; return -1; }
    return 0;
}
static inline unsigned dosbox_setting_load(unsigned game)
{
    char path[128],text[64];
    if (dosbox_setting_path(game,path,sizeof(path))) return 0;
    FILE *file=fopen(path,"rb"); if (!file) return 0;
    size_t n=fread(text,1,sizeof(text),file);
    int error=ferror(file); fclose(file);
    static const char selected[]="DOS kernel=freedos-1.3\n";
    return !error && n==sizeof(selected)-1 && !memcmp(text,selected,n) ? CIUKI_DOSVM_FREEDOS13 : 0;
}
static inline int dosbox_setting_save(unsigned game,unsigned kernel)
{
    char path[128],temporary[160];
    if (dosbox_setting_path(game,path,sizeof(path)) || kernel>CIUKI_DOSVM_FREEDOS13) { errno=EINVAL; return -1; }
    if (mkdir(DOSBOX_SETTINGS_DIR,0755) && errno!=EEXIST) return -1;
    int n=snprintf(temporary,sizeof(temporary),"%s.%u.new",path,(unsigned)getpid());
    if (n<0 || (size_t)n>=sizeof(temporary)) { errno=ENAMETOOLONG; return -1; }
    FILE *file=fopen(temporary,"wb"); if (!file) return -1;
    const char *text=kernel==CIUKI_DOSVM_FREEDOS13 ? "DOS kernel=freedos-1.3\n" : "DOS kernel=ciukidos\n";
    size_t length=strlen(text);
    int error=0;
    if (fwrite(text,1,length,file)!=length || fflush(file) || fsync(fileno(file))) error=errno ? errno : EIO;
    if (fclose(file) && !error) error=errno;
    if (!error && rename(temporary,path)) error=errno;
    if (error) { unlink(temporary); errno=error; return -1; }
    return 0;
}
#endif
