/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CD_MODEL_H
#define CD_MODEL_H
#include <ciuki/abi.h>
struct cd_model {
  struct ciuki_media_cd_toc toc;
  uint32_t generation, present, drive, caps, force, handles;
  uint32_t reads, plays, actions, positions, tray, analog, audio_status, absolute, play_from, play_to;
  uint32_t last_notify, notifications;
  int read_error, play_error;
  void (*during_read)(void);
};
extern struct cd_model cd_model;
void cd_reader_add(void *);
void cd_tick(void);
void cd_load(const char *);
void cd_destroy(void);
#endif
