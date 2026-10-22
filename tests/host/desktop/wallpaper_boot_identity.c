/* SPDX-License-Identifier: MIT
 * f2-90b: print the cache name of the request the desktop builds at boot
 * (apps/desktop/desktop.c wallpaper_load) for each FAT mtime given. */
#include "desktop.h"
#include "theme.h"
#include "wallpaper_io.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    struct ciuki_settings cfg; cfg_defaults(&cfg);
    const unsigned sizes[][2]={{800,600},{1024,768},{1280,800},{1280,1024},{1400,1050}};
    if (argc<3) return 2;
    uint64_t size=strtoull(argv[1],NULL,10);
    for (int i=2;i<argc;i++) for (unsigned k=0;k<5;k++) {
        struct desk_wall_request r={0};
        snprintf(r.path,sizeof(r.path),"%s",cfg.wallpaper);
        r.width=sizes[k][0]; r.height=sizes[k][1]; r.top=r.bottom=DESK_BAR; r.mode=cfg.wallpaper_mode;
        r.colour=cfg.desktop_color==CFG_DESKTOP_COLOR ? desk_theme->desktop : cfg.desktop_color;
        char name[DESK_WALL_NAME_MAX]; desk_wall_cache_name(name,&r,size,strtoll(argv[i],NULL,10));
        puts(name);
    }
    return 0;
}
