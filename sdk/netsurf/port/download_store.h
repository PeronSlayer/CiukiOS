/* Streamed download storage; no network/toolkit dependency. SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOWNLOAD_STORE_H
#define CIUKI_DOWNLOAD_STORE_H
#include <stddef.h>
struct ciuki_download_store { int fd, complete; size_t bytes; char temporary[560], name[128]; };
int ciuki_download_begin(struct ciuki_download_store *,const char *directory,const char *suggested);
int ciuki_download_write(struct ciuki_download_store *,const void *,size_t);
int ciuki_download_complete(struct ciuki_download_store *);
int ciuki_download_save(struct ciuki_download_store *,const char *destination);
void ciuki_download_discard(struct ciuki_download_store *);
#endif
