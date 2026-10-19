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
#include <ciuki/present.h>
#define DESK_VERSION 1u
#define DESK_SHELL_VERSION 2u
#define DESK_RESIZE_MINOR 1u       /* v2.1: header byte 2; v1/v2.0 remain unchanged */
#define DESK_V3_VERSION 3u
#define DESK_V3_HEADER 16u
#define DESK_V3_OVERLAPPED 0u
#define DESK_V3_POPUP 1u
#define DESK_V3_TOOL 2u
#define DESK_V3_TOPMOST 4u
#define DESK_V3_MODAL 8u
#define DESK_V3_STYLES 15u
#define DESK_V3_CAPTURE 1u
#define DESK_V3_CLIP 2u
#define DESK_V3_RELATIVE 4u
#define DESK_V3_HIDDEN 8u
/* WINDOW_ORDER flags; order targets occupy a separate namespace from ids. */
#define DESK_V3_ORDER_Z 1u
#define DESK_V3_ORDER_OWNER 2u
#define DESK_V3_ORDER_TOP 0u
#define DESK_V3_ORDER_BOTTOM 0xffffffffu
#define DESK_V3_ORDER_TOPMOST 0xfffffffeu
#define DESK_V3_ORDER_NOTOPMOST 0xfffffffdu
/* V3 payloads after the 16-byte envelope, all u32 unless noted:
 * HELLO: empty; CREATE: style,owner,owner-generation,x,y,w,h,scale,title-bytes,title;
 * DAMAGE: x,y,w,h; RESIZE: w,h,configure-serial + one surface fd;
 * PLACE: x,y,w,h,scale,show (0 normal,1 minimized,2 hidden),style;
 * TITLE: byte-count,UTF-8; CURSOR: shape,id,visibility-count;
 * CURSOR_BITMAP: id,hot-x,hot-y + one 32x32 ARGB8888 surface fd;
 * POINTER: flags,clip-x,clip-y,clip-w,clip-h; WARP: logical client x,y;
 * LEASE: action (0 release,1 request),w,h,bpp;
 * RESULT: request-opcode,signed errno; CONFIGURE: w,h,scale,configure-serial,physical-client-x,physical-client-y,show,style;
 * ACTIVATE: focus,active; LEASE_EVENT: state,generation,w,h,bpp;
 * additive 72-byte LEASE_EVENT: above + token,fb-generation,mode,scale,
 * source-w,source-h,pitch,size,format,native-w,native-h,native-bpp,signed-error.
 * Legacy 20-byte events remain readable; v1/v2.1 are unchanged.
 * KEY/MOTION/BUTTON/RELATIVE: original 40-byte ciuki_input_event encoding;
 * CRASH: exception,address,module-byte-count,UTF-8 module; RESTART: flags;
 * POINTER_STATE (server): current pointer flags; zero revokes capture/clip.
 * CLOSE_ACK: disposition (0 declined, 1 deferred), per window.
 * WINDOW_ORDER: flags (Z=1,OWNER=2), insert-after id/sentinel,generation,
 * owner id,owner-generation. Unselected fields are zero; no descriptors.
 * CLOSE/CLOSE_REQUEST/CLOSED: empty; PING/PONG: UI progress token.
 * CREATE/DAMAGE keep XRGB8888; only cursor bitmap uses alpha in its high byte.
 */
enum desk_v3_opcode {
    DESK_V3_RESIZE = 64, DESK_V3_PLACE, DESK_V3_CURSOR_BITMAP,
    DESK_V3_POINTER, DESK_V3_WARP, DESK_V3_LEASE, DESK_V3_RESULT,
    DESK_V3_ACTIVATE, DESK_V3_LEASE_EVENT, DESK_V3_RELATIVE_EVENT,
    DESK_V3_CRASH, DESK_V3_RESTART, DESK_V3_POINTER_STATE,
    DESK_V3_WINDOW_ORDER
};
#define DESK_RESIZABLE 1u
#define DESK_CONFIGURE_COMMITTED 1u
#define DESK_CURSOR_API 1u
enum desk_cursor_shape {
    DESK_CURSOR_ARROW, DESK_CURSOR_BUSY, DESK_CURSOR_ARROW_BUSY,
    DESK_CURSOR_TEXT, DESK_CURSOR_FORBIDDEN, DESK_CURSOR_RESIZE_EW,
    DESK_CURSOR_RESIZE_NS, DESK_CURSOR_RESIZE_NWSE, DESK_CURSOR_RESIZE_NESW,
    DESK_CURSOR_MOVE, DESK_CURSOR_HAND, DESK_CURSOR_CROSSHAIR,
    DESK_CURSOR_PRECISION,
    DESK_CURSOR_PENCIL, DESK_CURSOR_BRUSH, DESK_CURSOR_FILL, DESK_CURSOR_PICK,
    DESK_CURSOR_ZOOM, DESK_CURSOR_ERASER, DESK_CURSOR_COUNT
};
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
    DESK_GFX_QUERY, DESK_GFX_INFO, DESK_GFX_BENCH, DESK_GFX_RESULT,
    DESK_GFX_FRAME, DESK_GFX_PRESENTED, /* f2-59: bounded scene region and completion */
    DESK_DOS_FULLSCREEN /* desktop to DOS client, no payload */,
    DESK_SET_CURSOR, /* v2 client to desktop: uint32 shape, own client area only */
    /* v2 client to desktop, 8 bytes, no fds: uint32 disposition (0 declined,
     * 1 closing later). One later reply within 2 s grants 30 s total. */
    DESK_CLOSE_ACK
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
struct desk_lease_tuple {
    uint32_t token, generation, mode, scale;
    struct ciuki_present_surface surface;
    uint32_t native_width, native_height, native_bpp;
};
struct desk_packet {
    struct desk_lease_tuple lease;
    unsigned opcode;
    uint32_t generation, owner, owner_generation, style, scale;
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
    uint32_t source_w, source_h, damage_x, damage_y, damage_w, damage_h;
    uint32_t ui_x, ui_y, ui_w, ui_h;
    uint32_t resize_serial; /* optional GFX_FRAME replacement attachment */
    uint64_t completed_ns, next_frame_ns;
    uint32_t timing_serial,render_us,submit_us,wait_us; /* optional 84-byte FRAME */
    uint64_t convert_ns,copy_ns,slot_ns,refresh_ns; /* optional 64-byte PRESENTED */
};
int desk_utf8_valid(const uint8_t *, unsigned);
void desk_v3_message(struct ciuki_message *, unsigned op, unsigned payload,
                     uint32_t request, uint32_t id, uint32_t generation);
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
