/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_MUSIC_PLAYLIST_H
#define CIUKI_MUSIC_PLAYLIST_H
#include "decoder.h"
#define MUSIC_PATH 512
#define MUSIC_TRACKS 256
struct music_playlist { char path[MUSIC_TRACKS][MUSIC_PATH]; unsigned count; };
int music_is_audio(const char *);
int music_is_playlist(const char *);
int music_path(char out[MUSIC_PATH], const char *base_file, const char *entry);
int music_playlist_append(struct music_playlist *, const char *);
int music_playlist_parse(struct music_playlist *, const char *base_file, const char *, size_t);
int music_playlist_add(struct music_playlist *, const char *, music_cancel, void *);
int music_playlist_save(const struct music_playlist *, const char *, music_cancel, void *);
#endif
