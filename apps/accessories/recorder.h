/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_RECORDER_H
#define CIUKI_RECORDER_H
#include "acc.h"
#define REC_RATE 48000u
#define REC_FRAMES (REC_RATE * 60u)
#define REC_W 480
#define REC_H 250
enum { REC_RECORD=1600, REC_PLAY, REC_STOP, REC_SAVE, REC_OPEN, REC_NEW,
    REC_REVERSE, REC_LOUDER, REC_QUIETER, REC_TRIM_BEFORE, REC_TRIM_AFTER,
    REC_POSITION, REC_SOURCE, REC_GAIN, REC_STATUS, REC_D_SAVE, REC_D_OPEN,
    REC_D_CLOSE, REC_TICK };
struct recorder_backend {
    int (*start)(void *, unsigned source, unsigned gain);
    int (*read)(void *, int16_t *stereo, unsigned frames);
    int (*write)(void *, const int16_t *stereo, unsigned frames);
    int (*stop)(void *);
    void *ctx;
};
struct recorder_app {
    struct du_ui *ui; struct du_window *window;
    struct recorder_backend backend;
    int16_t *pcm;
    unsigned frames, position, source, gain;
    int recording, playing, dirty, closing;
    char status[160], path[256];
};
void recorder_attach(struct recorder_app *, struct du_ui *, struct du_window *, const struct recorder_backend *);
void recorder_destroy(struct recorder_app *);
void recorder_pump(struct recorder_app *);
void recorder_event(struct du_ui *, const struct du_event *, void *);
int recorder_close(struct recorder_app *);
int recorder_save(struct recorder_app *, const char *);
int recorder_open(struct recorder_app *, const char *);
#endif
