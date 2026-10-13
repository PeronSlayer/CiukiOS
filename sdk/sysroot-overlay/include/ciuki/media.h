/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_MEDIA_H
#define CIUKI_SDK_MEDIA_H
#include <ciuki/abi.h>
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
#endif
