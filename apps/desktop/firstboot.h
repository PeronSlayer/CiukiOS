/* SPDX-License-Identifier: MIT -- desktop-private, no SDK ABI. */
#ifndef CIUKI_FIRSTBOOT_H
#define CIUKI_FIRSTBOOT_H
#include "../libdeskui/deskui.h"
#include <stddef.h>
#include <stdint.h>
#define FB_STATE_BYTES 16384u
#define FB_STEPS 7u
enum fb_step { FB_ACCEL, FB_SOUND, FB_NETWORK, FB_GAMES, FB_ENTROPY, FB_CLOCK, FB_SUMMARY };
enum fb_result { FB_PENDING, FB_OK, FB_SKIPPED, FB_FAILED };
enum fb_action { FB_DISPLAY, FB_CHANGE_CLOCK };
/* service is a single nonblocking turn: 0=ok, 1=skipped, -EAGAIN=pending,
 * other negative errno=failed. append must commit the complete record. */
struct fb_host {
    void *ctx;
    int (*read)(void *, char *, size_t, size_t *);
    int (*append)(void *, const char *, size_t);
    int (*service)(void *, unsigned, int, char *, size_t);
    void (*cancel)(void *, unsigned);
    void (*record)(void *, const char *);
    void (*action)(void *, unsigned);
    /* sec-user-02 (user-security-contract.md 5.1): the wizard runs in the
     * authenticated administrator's session, never as SYSTEM. 1 when the
     * session credential holds Administrators (544), 0 otherwise, <0 when it
     * cannot be read; NULL keeps the pre-security behaviour (host fixtures). */
    int (*administrator)(void *);
};
struct firstboot {
    struct fb_host host;
    enum fb_result result[FB_STEPS];
    char detail[FB_STEPS][192];
    unsigned step, polls, focus;
    int active, started, finish, error, done;
    uint64_t began;
};
int fb_accel_outcome(unsigned, unsigned, int, char *, size_t);
int fb_register_games(const char *, int (*)(void *,unsigned,int,uint32_t *), void *, char *, size_t);
extern const char *const fb_names[FB_STEPS];
extern const char *const fb_results[4];
int fb_begin(struct firstboot *, const struct fb_host *);
void fb_tick(struct firstboot *, uint64_t ms);
void fb_finish(struct firstboot *);
void fb_action(struct firstboot *, unsigned);
void fb_draw(const struct firstboot *, struct du_canvas *, const uint32_t *portrait);
struct desktop;
int desk_firstboot(struct desktop *); /* normal boot only; returns deferred page bits */
#endif
