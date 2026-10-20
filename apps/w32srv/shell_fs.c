/* f5-07B: the POSIX adapter behind struct ws_fs (Start-menu folders,
 * WININIT.INI, the restart journal). SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __CIUKIOS__
#include <ciuki/runtime.h>
#endif
static int posix_stat(void *o, const char *path, int *is_dir, uint64_t *size)
{
    (void)o; struct stat st;
    if (stat(path, &st)) return errno ? errno : EIO;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    if (size) *size = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
    return 0;
}
static int posix_mkdir(void *o, const char *path) { (void)o; return mkdir(path, 0777) ? (errno ? errno : EIO) : 0; }
static int posix_rmdir(void *o, const char *path) { (void)o; return rmdir(path) ? (errno ? errno : EIO) : 0; }
/* Ciuki fsync includes volume metadata. Linux also needs the parent
 * directory synced after a namespace mutation for the restart journal. */
static int sync_parent(const char *path)
{
#ifndef __CIUKIOS__
    char parent[1100];if(strlen(path)>=sizeof(parent))return ENAMETOOLONG;
    strcpy(parent,path);char *slash=strrchr(parent,'/');
    if(!slash)strcpy(parent,".");else if(slash==parent)slash[1]=0;else *slash=0;
    int fd=open(parent,O_RDONLY|O_DIRECTORY);if(fd<0)return errno?errno:EIO;
    int e=fsync(fd)?(errno?errno:EIO):0;if(close(fd)&&!e)e=errno?errno:EIO;return e;
#else
    (void)path;return 0;
#endif
}
static int posix_unlink(void *o,const char *path)
{
    (void)o;struct stat st;
    if(!stat(path,&st) && !(st.st_mode&(S_IWUSR|S_IWGRP|S_IWOTH)))return EACCES;
    if(unlink(path))return errno?errno:EIO;return sync_parent(path);
}
static int posix_rename(void *o,const char *from,const char *to)
{
    (void)o;struct stat st;
    if(!stat(to,&st) && !(st.st_mode&(S_IWUSR|S_IWGRP|S_IWOTH)))return EACCES;
    if(rename(from,to))return errno?errno:EIO;
    int e=sync_parent(to);if(!e)e=sync_parent(from);return e;
}
static int posix_read(void *o, const char *path, uint8_t **out, size_t *size, size_t limit)
{
    (void)o; *out = NULL; *size = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno ? errno : EIO;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0) { int e = errno ? errno : EIO; close(fd); return e; }
    if ((uint64_t)st.st_size > limit) { close(fd); return EFBIG; }
    size_t length = (size_t)st.st_size; uint8_t *bytes = malloc(length ? length : 1);
    if (!bytes) { close(fd); return ENOMEM; }
    size_t at = 0;
    while (at < length) {
        ssize_t r = read(fd, bytes + at, length - at);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { int e = r < 0 && errno ? errno : EIO; free(bytes); close(fd); return e; }
        at += (size_t)r;
    }
    close(fd);
    *out = bytes; *size = length;
    return 0;
}
static int posix_write(void *o, const char *path, const void *data, size_t size)
{
    (void)o;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return errno ? errno : EIO;
    const uint8_t *bytes = data; int e = 0;
    while (size) {
        ssize_t w = write(fd, bytes, size);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { e = w < 0 && errno ? errno : EIO; break; }
        bytes += w; size -= (size_t)w;
    }
    if (!e && fsync(fd)) e = errno ? errno : EIO;
    if (close(fd) && !e) e = errno ? errno : EIO;
    if(!e)e=sync_parent(path);
    return e;
}
static int posix_list(void *o, const char *path, int (*callback)(void *, const char *, int), void *context)
{
    (void)o;
    DIR *dir = opendir(path);
    if (!dir) return errno ? errno : EIO;
    struct dirent *entry; int stop = 0;
    while (!stop && (entry = readdir(dir))) {
        char full[1100]; struct stat st;
        if (snprintf(full, sizeof(full), "%s/%s", path, entry->d_name) >= (int)sizeof(full)) continue;
        int is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
        stop = callback(context, entry->d_name, is_dir);
    }
    closedir(dir);
    return 0;
}
static void posix_log(void *o, const char *text)
{
    (void)o;
#ifdef __CIUKIOS__
    CU_CALL(DEBUG_WRITE, CU_PTR(text), strlen(text), 0, 0, 0, 0);
#else
    fprintf(stderr, "%s\n", text);
#endif
}
void ws_posix_fs(struct ws_fs *fs)
{
    *fs = (struct ws_fs){NULL, posix_stat, posix_mkdir, posix_rmdir, posix_unlink, posix_rename,
                         posix_read, posix_write, posix_list, posix_log};
}
