/* SPDX-License-Identifier: MIT
 * Host-only image-build tool. Uses exactly the desktop decoder/renderer/key. */
#define _POSIX_C_SOURCE 200809L
#include "desktop.h"
#include "wallpaper_io.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc!=4) return 2; /* source PNG, output directory, FAT UTC mtime */
    struct stat s; if (stat(argv[1],&s) || s.st_size<=0 || s.st_size>DESK_IMAGE_FILE_MAX) return 1;
    FILE *f=fopen(argv[1],"rb"); if (!f) return 1;
    size_t n=(size_t)s.st_size; uint8_t *data=malloc(n);
    if (!data) { fclose(f); return 1; }
    int error=fread(data,1,n,f)!=n; fclose(f);
    struct desk_image image={0};
    if (!error) error=desk_image_decode(data,n,&image);
    free(data); if (error) return 1;
    struct ciuki_settings cfg; cfg_defaults(&cfg);
    const uint32_t sizes[][2]={{800,600},{1024,768},{1280,800},{1280,1024},{1400,1050}};
    char *end; int64_t mtime=strtoll(argv[3],&end,10); if (!*argv[3] || *end) { desk_image_free(&image); return 2; }
    for (unsigned i=0;i<5;i++) {
        struct desk_wall_request r={.width=sizes[i][0],.height=sizes[i][1],.top=DESK_BAR,.bottom=DESK_BAR,
            .colour=cfg.desktop_color,.mode=cfg.wallpaper_mode};
        snprintf(r.path,sizeof(r.path),"%s",cfg.wallpaper);
        size_t bytes=(size_t)r.width*r.height*4;
        uint32_t *pixels=malloc(bytes); if (!pixels) { error=1; break; }
        error=desk_wallpaper_render(pixels,r.width,r.height,r.top,r.bottom,r.path,r.mode,r.colour,&image,NULL);
        char name[DESK_WALL_NAME_MAX], path[512];
        desk_wall_cache_name(name,&r,(uint64_t)s.st_size,mtime);
        int length=snprintf(path,sizeof(path),"%s/%s",argv[2],name);
        if (length<0 || (size_t)length>=sizeof(path)) error=1;
        f=error ? NULL : fopen(path,"wb");
        if (!f) error=1;
        else { if (fwrite(pixels,1,bytes,f)!=bytes) error=1; if (fclose(f)) error=1; }
        free(pixels); if (error) break;
        puts(name);
    }
    desk_image_free(&image); return error ? 1 : 0;
}
