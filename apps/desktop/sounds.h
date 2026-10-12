/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_SOUNDS_H
#define CIUKI_DESKTOP_SOUNDS_H
#include <stddef.h>
#include <stdint.h>
#define SOUND_RATE 22050u
#define SOUND_FILE_MAX (SOUND_RATE * 4u + 4096u)
#define SOUND_TEXT_MAX 4096u
#define SOUND_NAME_MAX 64u
#define SOUND_DIR "/system/sounds"
#define SOUND_CFG "/system/settings/sounds.cfg"
enum sound_event {
    SOUND_STARTUP, SOUND_SHUTDOWN, SOUND_MENU_OPEN, SOUND_MENU_SELECT,
    SOUND_WINDOW_OPEN, SOUND_WINDOW_CLOSE, SOUND_MINIMISE, SOUND_RESTORE,
    SOUND_ERROR, SOUND_INFORMATION, SOUND_QUESTION, SOUND_END_TASK,
    SOUND_VOLUME, SOUND_EMPTY_RECYCLE, SOUND_EVENTS
};
extern const char *const sound_keys[SOUND_EVENTS], *const sound_labels[SOUND_EVENTS];
struct sound_scheme { int enabled; char file[SOUND_EVENTS][SOUND_NAME_MAX]; };
struct sound_clip { const uint8_t *pcm; unsigned frames; };
struct sound_voice { struct sound_clip clip; unsigned position; };
struct sound_player {
    struct sound_voice voice[2];
    int16_t pending[256]; unsigned count, offset;
    int error;
};
void sound_defaults(struct sound_scheme *);
int sound_parse(struct sound_scheme *, const char *, size_t);
size_t sound_format(const struct sound_scheme *, char *, size_t);
int sound_path(const struct sound_scheme *, unsigned event, char *, size_t);
int sound_wav(const void *, size_t, struct sound_clip *);
void sound_reset(struct sound_player *);
int sound_play(struct sound_player *, struct sound_clip);
int sound_active(const struct sound_player *);
/* sink returns accepted frames, -EAGAIN/-EINTR, or a permanent -errno. */
int sound_pump(struct sound_player *, int (*sink)(void *, const int16_t *, unsigned), void *);
void sound_tone(int16_t *pcm); /* SOUND_RATE samples, exactly 1 kHz for 1 s */
#endif
