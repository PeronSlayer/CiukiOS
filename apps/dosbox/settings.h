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
static inline unsigned dosbox_memory_value(const char *value)
{ return value && !strcmp(value,"locked") ? CIUKI_DOSVM_MEMORY_LOCKED : CIUKI_DOSVM_MEMORY_PAGED; }
static inline const char *dosbox_memory_label(unsigned value)
{ return value==CIUKI_DOSVM_MEMORY_LOCKED ? "locked (compatibility)" : "paged (default)"; }
/* Exactly "DOS kernel=<k>\n", optionally followed by "DOS memory=locked\n"
 * (f3-46, the Windows 98 PIF "XMS/EMS memory locked" role); any other
 * content is the default: CiukiDOS with the paged pool. */
static inline void dosbox_settings_load(unsigned game,unsigned *kernel,unsigned *memory)
{
    *kernel=CIUKI_DOSVM_CIUKIDOS; *memory=CIUKI_DOSVM_MEMORY_PAGED;
    char path[128],text[96];
    if (dosbox_setting_path(game,path,sizeof(path))) return;
    FILE *file=fopen(path,"rb"); if (!file) return;
    size_t n=fread(text,1,sizeof(text)-1,file);
    int error=ferror(file); fclose(file);
    if (error) return;
    text[n]=0;
    static const char *const kernels[]={"DOS kernel=ciukidos\n","DOS kernel=freedos-1.3\n"};
    static const char locked[]="DOS memory=locked\n";
    for (unsigned k=0;k<2;k++) {
        size_t m=strlen(kernels[k]);
        if (n<m || memcmp(text,kernels[k],m)) continue;
        if (n==m) { *kernel=k; return; }
        if (n==m+sizeof(locked)-1 && !memcmp(text+m,locked,sizeof(locked)-1)) {
            *kernel=k; *memory=CIUKI_DOSVM_MEMORY_LOCKED;
        }
        return;
    }
}
static inline unsigned dosbox_setting_load(unsigned game)
{ unsigned kernel,memory; dosbox_settings_load(game,&kernel,&memory); return kernel; }
static inline unsigned dosbox_memory_load(unsigned game)
{ unsigned kernel,memory; dosbox_settings_load(game,&kernel,&memory); return memory; }
static inline int dosbox_settings_save(unsigned game,unsigned kernel,unsigned memory)
{
    char path[128],temporary[160];
    if (dosbox_setting_path(game,path,sizeof(path)) || kernel>CIUKI_DOSVM_FREEDOS13 ||
        memory>CIUKI_DOSVM_MEMORY_LOCKED) { errno=EINVAL; return -1; }
    if (mkdir(DOSBOX_SETTINGS_DIR,0755) && errno!=EEXIST) return -1;
    int n=snprintf(temporary,sizeof(temporary),"%s.%u.new",path,(unsigned)getpid());
    if (n<0 || (size_t)n>=sizeof(temporary)) { errno=ENAMETOOLONG; return -1; }
    FILE *file=fopen(temporary,"wb"); if (!file) return -1;
    char text[64];
    snprintf(text,sizeof(text),"%s%s",kernel==CIUKI_DOSVM_FREEDOS13 ? "DOS kernel=freedos-1.3\n" : "DOS kernel=ciukidos\n",
             memory==CIUKI_DOSVM_MEMORY_LOCKED ? "DOS memory=locked\n" : "");
    size_t length=strlen(text);
    int error=0;
    if (fwrite(text,1,length,file)!=length || fflush(file) || fsync(fileno(file))) error=errno ? errno : EIO;
    if (fclose(file) && !error) error=errno;
    if (!error && rename(temporary,path)) error=errno;
    if (error) { unlink(temporary); errno=error; return -1; }
    return 0;
}
/* A kernel choice keeps the game's memory choice, and vice versa. */
static inline int dosbox_setting_save(unsigned game,unsigned kernel)
{
    unsigned old_kernel,memory;
    if (!game || game>DESK_GAME_COUNT) { errno=EINVAL; return -1; }
    dosbox_settings_load(game,&old_kernel,&memory);
    return dosbox_settings_save(game,kernel,memory);
}
static inline int dosbox_memory_save(unsigned game,unsigned memory)
{
    unsigned kernel,old_memory;
    if (!game || game>DESK_GAME_COUNT) { errno=EINVAL; return -1; }
    dosbox_settings_load(game,&kernel,&old_memory);
    return dosbox_settings_save(game,kernel,memory);
}
#endif
