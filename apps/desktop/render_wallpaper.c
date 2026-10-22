/* SPDX-License-Identifier: MIT
 * Host-only image-build tool. Uses exactly the desktop decoder/renderer/key. */
#define _POSIX_C_SOURCE 200809L
#include "desktop.h"
#include "wallpaper_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>

static int transfer(int fd, void *data, size_t bytes, int output)
{
    size_t done=0;
    while (done<bytes) {
        size_t n=bytes-done<65536u ? bytes-done : 65536u;
        ssize_t got=output ? write(fd,(const char *)data+done,n) : read(fd,(char *)data+done,n);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return 1;
        done+=(size_t)got;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc!=4) return 2; /* source PNG, output directory, FAT UTC mtime (or thumbnail) */
    struct stat s; if (stat(argv[1],&s) || s.st_size<=0 || s.st_size>DESK_IMAGE_FILE_MAX) return 1;
    int fd=open(argv[1],O_RDONLY); if (fd<0) return 1;
    size_t n=(size_t)s.st_size; uint8_t *data=malloc(n);
    if (!data) { close(fd); return 1; }
    int error=transfer(fd,data,n,0); if (close(fd)) error=1;
    struct desk_image image={0};
    if (!error) error=desk_image_decode(data,n,&image);
    free(data); if (error) return 1;
    if (!strcmp(argv[3], "thumbnail")) {
        uint32_t pixels[192 * 144];
        desk_image_scale(&image, pixels, 192, 144, CFG_WALL_STRETCH, 0);
        for (unsigned i = 0; i < 192 * 144; i++) pixels[i] &= 0x00ffffffu;
        fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        error = fd < 0;
        if (!error) { error = transfer(fd, pixels, sizeof(pixels), 1); if (close(fd)) error = 1; }
        desk_image_free(&image);
        return error ? 1 : 0;
    }
    /* Same resolution as desktop.c wallpaper_load: the default colour is the active theme's. */
    struct ciuki_settings cfg; cfg_defaults(&cfg);
    const uint32_t sizes[][2]={{800,600},{1024,768},{1280,800},{1280,1024},{1400,1050}};
    char *end; int64_t mtime=strtoll(argv[3],&end,10); if (!*argv[3] || *end) { desk_image_free(&image); return 2; }
    for (unsigned i=0;i<5;i++) {
        struct desk_wall_request r={.width=sizes[i][0],.height=sizes[i][1],.top=DESK_BAR,.bottom=DESK_BAR,
            .colour=cfg.desktop_color==CFG_DESKTOP_COLOR ? desk_theme->desktop : cfg.desktop_color,.mode=cfg.wallpaper_mode};
        snprintf(r.path,sizeof(r.path),"%s",cfg.wallpaper);
        size_t bytes=(size_t)r.width*r.height*4;
        uint32_t *pixels=malloc(bytes); if (!pixels) { error=1; break; }
        error=desk_wallpaper_render(pixels,r.width,r.height,r.top,r.bottom,r.path,r.mode,r.colour,&image,NULL);
        char name[DESK_WALL_NAME_MAX], path[512];
        desk_wall_cache_name(name,&r,(uint64_t)s.st_size,mtime);
        int length=snprintf(path,sizeof(path),"%s/%s",argv[2],name);
        if (length<0 || (size_t)length>=sizeof(path)) error=1;
        fd=error ? -1 : open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if (fd<0) error=1;
        else {
            if (transfer(fd,pixels,bytes,1)) error=1;
            if (close(fd)) error=1;
            if (error) unlink(path);
        }
        free(pixels); if (error) break;
        puts(name);
    }
    desk_image_free(&image); return error ? 1 : 0;
}
