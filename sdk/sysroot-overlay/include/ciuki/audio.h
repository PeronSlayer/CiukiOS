/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_AUDIO_H
#define CIUKI_SDK_AUDIO_H
#include <ciuki/abi.h>
#define __CIUKI_AUDIO_VERSION__ 1
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
#endif
