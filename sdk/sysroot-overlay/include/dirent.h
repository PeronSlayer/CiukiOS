/* SPDX-License-Identifier: MIT */
#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>
#define dirent ciuki_dirent
#define CIUKI_DIRENT_METADATA 1 /* d_type, d_size, d_mtime from GETDENTS */
typedef struct ciuki_DIR DIR;
DIR *opendir(const char *);
DIR *fdopendir(int);
struct dirent *readdir(DIR *);
void rewinddir(DIR *);
int closedir(DIR *);
int dirfd(DIR *);
#endif
