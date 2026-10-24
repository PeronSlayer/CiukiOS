/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_AUDIO_H
#define CIUKI_SDK_AUDIO_H
#include <ciuki/abi.h>
#define __CIUKI_AUDIO_VERSION__ 1
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
/* MIDI software output on an O_WRONLY /dev/audio description. Returns the
 * number of accepted bytes, or -1 and errno. Retain a short batch's suffix. */
int ciuki_midi_write(int fd, const uint8_t *bytes, uint32_t size);
#endif
