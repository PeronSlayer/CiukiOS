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
/* Retired ABI values and old configuration files never select another kernel. */
static inline unsigned dosbox_kernel_value(const char *value)
{ (void)value; return CIUKI_DOSVM_UNIFIED; }
static inline const char *dosbox_kernel_label(unsigned value)
{ (void)value; return "CiukiDOS unified"; }
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
/* Exactly "DOS kernel=unified\n", optionally followed by "DOS memory=locked\n"
 * (f3-46, the Windows 98 PIF "XMS/EMS memory locked" role); any other
 * content is the default: the unified CiukiDOS kernel with the paged pool. */
static inline void dosbox_settings_load(unsigned game,unsigned *kernel,unsigned *memory)
{
    *kernel=CIUKI_DOSVM_UNIFIED; *memory=CIUKI_DOSVM_MEMORY_PAGED;
    char path[128],text[96];
    if (dosbox_setting_path(game,path,sizeof(path))) return;
    FILE *file=fopen(path,"rb"); if (!file) return;
    size_t n=fread(text,1,sizeof(text)-1,file);
    int error=ferror(file); fclose(file);
    if (error) return;
    text[n]=0;
    static const char unified[]="DOS kernel=unified\n";
    static const char locked[]="DOS memory=locked\n";
    /* Exact grammar, including EOF: malformed, embedded NUL, oversized or
     * retired kernel tokens fall back to the unified pageable profile. */
    size_t m=sizeof(unified)-1;
    if (n==m+sizeof(locked)-1 && !memcmp(text,unified,m) &&
        !memcmp(text+m,locked,sizeof(locked)-1)) *memory=CIUKI_DOSVM_MEMORY_LOCKED;
}
static inline unsigned dosbox_setting_load(unsigned game)
{ unsigned kernel,memory; dosbox_settings_load(game,&kernel,&memory); return kernel; }
static inline unsigned dosbox_memory_load(unsigned game)
{ unsigned kernel,memory; dosbox_settings_load(game,&kernel,&memory); return memory; }
#endif
