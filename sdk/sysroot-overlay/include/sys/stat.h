/* SPDX-License-Identifier: MIT */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#define stat ciuki_stat
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
/* Informational mode bits; CiukiOS performs no Unix permission enforcement. */
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISCHR(m) (((m)&S_IFMT)==S_IFCHR)
int stat(const char *, struct stat *);
int fstat(int, struct stat *);
int mkdir(const char *, mode_t);
int chmod(const char *, mode_t);
int fchmod(int, mode_t);
mode_t umask(mode_t);
#endif
