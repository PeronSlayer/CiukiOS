/* SPDX-License-Identifier: MIT
 * F2 desktop protocol v1, with negotiated v2 close requests and the v2
 * task-manager extension (f2-33): window list, End Task, Switch To, launch,
 * and the v2 settings request of the Control Panel (desktop shell).
 * Minor v2.1 negotiates client-allocated replacement surfaces for resizing.
 * Byte encoding, independent of C alignment/padding. SDK: ciuki/desktop.h. */
#ifndef CIUKI_APP_DESKTOP_PROTOCOL_H
#define CIUKI_APP_DESKTOP_PROTOCOL_H
#include <ciuki/abi.h>
#include <stdint.h>
#define DESK_VERSION 1u
#define DESK_SHELL_VERSION 2u
#define DESK_RESIZE_MINOR 1u       /* v2.1: header byte 2; v1/v2.0 remain unchanged */
#define DESK_RESIZABLE 1u
#define DESK_CONFIGURE_COMMITTED 1u
#define DESK_TITLE_MAX 63u
#define DESK_ARG_MAX 160u   /* LAUNCH argument: 12 + name + 4 + argument always fits one message */
enum desk_opcode {
    DESK_HELLO = 1, DESK_CREATE_WINDOW, DESK_DAMAGE, DESK_MOVE, DESK_CLOSE,
    DESK_PING, DESK_CONFIGURE, DESK_KEY, DESK_MOTION, DESK_BUTTON,
    DESK_FOCUS, DESK_PONG, DESK_CLOSED, DESK_CLOSE_REQUEST,
    /* f2-33, version 2 only. WINDOW_INFO and TASK_RESULT flow desktop to
     * client; the other four flow client to desktop. */
    DESK_WINDOWS_QUERY, DESK_WINDOW_INFO, DESK_END_TASK, DESK_SWITCH_TO,
    DESK_LAUNCH, DESK_TASK_RESULT,
    /* Desktop shell, version 2 only, client to desktop: only /Programs/control
     * launched by the desktop may send it (others get TASK_RESULT EPERM). */
    DESK_SETTINGS,
    /* Legacy recovery (Volume Control), version 2, client to desktop: serial,
     * left, right (0..100), mute (0/1). The desktop applies it through its
     * delegated /dev/mixer control handle (F4) and answers TASK_RESULT. */
    DESK_MIXER_SET,
    /* f2-36, version 2, client to desktop: uint32 length, then 1..63
     * printable ASCII bytes. Updates only the sending client's window. */
    DESK_SET_TITLE,
    DESK_SOUND, /* v2: uint32 event; toolkit menu and dialog events */
    DESK_FILE_DROP /* v2: copy flag, client-relative release x/y; Files only */,
    DESK_SYSTEM_QUERY, DESK_SYSTEM_INFO /* f1-25: explicit opt-in, v2 desktop performance sample */,
    /* f2-46: desktop-launched Terminal only, v2: serial, action (1 reboot, 2 off).
     * The client confirms first; desktop runs the existing close/flush/power path. */
    DESK_TERMINAL_POWER,
    /* f2-52: desktop-launched Graphics Demo only; query or one fixed sample.
     * No client coordinates, display grants or hardware commands. */
    DESK_GFX_QUERY, DESK_GFX_INFO, DESK_GFX_BENCH, DESK_GFX_RESULT
};
#define DESK_SETTINGS_RELOAD 1u   /* re-read /system/settings/desktop.cfg and apply it */
#define DESK_SETTINGS_DISPLAY 3u /* VBE mode id in value[0]; value[1..2]=0; desktop persists after frame */
#define DESK_SETTINGS_MASTER 2u   /* set the master volume: left, right 0..100, mute 0/1 */
#define DESK_SETTINGS_SOUND_RELOAD 4u
#define DESK_SETTINGS_SOUND_TEST 5u /* values[0]: event index */
#define DESK_SETTINGS_AUDIO_TEST 6u /* full tone accepted: asynchronous TASK_RESULT */
#define DESK_WINDOW_UNRESPONSIVE 1u
#define DESK_WINDOW_MINIMIZED 2u
#define DESK_WINDOW_FOCUSED 4u
#define DESK_WINDOW_INTERNAL 8u
#define DESK_WINDOW_CLOSING 16u
#define DESK_WINDOW_FLAGS 31u
struct desk_packet {
    unsigned opcode;
    int32_t x, y;
    uint32_t width, height, serial, focused;
    char title[DESK_TITLE_MAX + 1];   /* also WINDOW_INFO title and LAUNCH name */
    char argument[DESK_ARG_MAX + 1];  /* LAUNCH: optional single program argument (empty when absent) */
    struct ciuki_input_event event;
    uint32_t window_id, pid, flags, index, count, request;
    int32_t result;
    uint32_t values[3];               /* SETTINGS: left/right/mute or mode id/0/0 */
    uint32_t min_width, min_height, max_width, max_height, step_width, step_height;
    uint64_t performance[9]; /* frames, damage px, presents, LFB bytes, composite,
                             * present, input, max input lag (all ns), interval ns */
    struct ciuki_accel_info accelerator;
    uint32_t bpp;
    uint64_t benchmark[3]; /* destination bytes, elapsed ns, completed kernel ops */
};
uint32_t desk_get32(const uint8_t *p);
void desk_put32(uint8_t *p, uint32_t v);
void desk_message(struct ciuki_message *m, unsigned opcode, unsigned length);
void desk_event_message(struct ciuki_message *m, unsigned opcode,
                        const struct ciuki_input_event *e);
/* Surface info is required for CREATE and DAMAGE (the attachment on a swap). */
int desk_parse(const struct ciuki_message *m, int from_client,
               const struct ciuki_surface_info *surface, struct desk_packet *out);
int desk_surface_valid(const struct ciuki_surface_info *s);
#endif
