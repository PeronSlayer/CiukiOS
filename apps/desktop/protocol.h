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
#define DESK_V3_CONTEXTHELP 16u   /* G15 WS_EX_CONTEXTHELP: the "?" caption button (it replaces minimise and zoom) */
#define DESK_V3_STYLES 31u
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
 * CLOSE_ACK: disposition (0 declined, 1 deferred), per window. A deferred
 * close has no deadline while the window answers PINGs; an unresponsive one
 * gets the desktop's "This program is not responding" dialog (G3).
 * WINDOW_ORDER: flags (Z=1,OWNER=2), insert-after id/sentinel,generation,
 * owner id,owner-generation. Unselected fields are zero; no descriptors.
 * CLOSE/CLOSE_REQUEST/CLOSED: empty; PING/PONG: UI progress token.
 * HELP (server, G15): empty. The user pressed the "?" caption button of a
 * window created with DESK_V3_CONTEXTHELP; the client enters What's This?
 * (WM_SYSCOMMAND SC_CONTEXTHELP) and answers the next click itself.
 * CREATE/DAMAGE keep XRGB8888; only cursor bitmap uses alpha in its high byte.
 */
enum desk_v3_opcode {
    DESK_V3_RESIZE = 64, DESK_V3_PLACE, DESK_V3_CURSOR_BITMAP,
    DESK_V3_POINTER, DESK_V3_WARP, DESK_V3_LEASE, DESK_V3_RESULT,
    DESK_V3_ACTIVATE, DESK_V3_LEASE_EVENT, DESK_V3_RELATIVE_EVENT,
    DESK_V3_CRASH, DESK_V3_RESTART, DESK_V3_POINTER_STATE,
    DESK_V3_WINDOW_ORDER, DESK_V3_WIN32_HWND,
    DESK_V3_PRESENT, /* 16-byte damage; RESULT after frame, immediate if covered/identical */
    /* G5 system clipboard on the v3 transport: window 0, generation 0,
     * payload exactly as the v2 CLIPBOARD/CLIPBOARD_EVENT below. */
    DESK_V3_CLIPBOARD, DESK_V3_CLIPBOARD_EVENT,
    /* G4 (WM_GETMINMAXINFO): client to desktop, 16 bytes: minimum and maximum
     * track size of the client area, min-w, min-h, max-w, max-h in unscaled
     * pixels, each 0 (default) or 1..2048, max >= min when both are given.
     * PLACE and CONFIGURE state word: 0 normal, 1 minimised, 2 hidden,
     * 3 maximised (the desktop zooms the window; WM_SIZE SIZE_MAXIMIZED). */
    DESK_V3_LIMITS,
    /* G18: TRAY_ICON: action,id,NIF flags,callback,tip-length,UTF-8 tip;
     * optional 16x16 ARGB surface iff NIF_ICON. BALLOON: id,title-length,
     * text-length,timeout-ms(low16)|NIIF-kind(high16),title,text.
     * EVENT: id,callback,mouse-message,screen-x,screen-y.
     * FLASH: invert (0/1). All use the authenticated window tuple. */
    DESK_V3_TRAY_ICON = 85, DESK_V3_TRAY_BALLOON = 86, DESK_V3_TRAY_EVENT = 90, DESK_V3_FLASH = 91,
    DESK_V3_HELP = 89, /* G15: server to client, empty payload */
    /* G16 SOUND (MessageBeep): client to desktop, 4 bytes: the toolkit sound
     * event (apps/desktop/sounds.h, below SOUND_EVENTS) the desktop plays through
     * its system-sound scheme (mute, volume and DOS-window rules apply). There is
     * no reply: like DAMAGE it only needs the window tuple of an open window. */
    DESK_V3_SOUND = 92
};
/* G12: shared settings, v2/v3; no fds. Request: key (0=query), value,
 * flags (SPIF_UPDATEINIFILE=1, SENDCHANGE=2). Snapshot: 11 u32 values, changed key, flags.
 * Reply uses the request/window identity of the request; unsolicited snapshots
 * use request 0. Appended after main's existing clipboard and limits opcodes (80–82). */
#define DESK_INPUT_SETTINGS 83u
#define DESK_INPUT_STATE 84u
#define DESK_INPUT_VALUES 11u
enum desk_input_key { DESK_INPUT_LAYOUT=1, DESK_INPUT_DELAY, DESK_INPUT_SPEED,
    DESK_INPUT_BLINK, DESK_INPUT_SWAP, DESK_INPUT_TRAILS, DESK_INPUT_WHEEL,
    DESK_INPUT_SCHEME, DESK_INPUT_MOUSE_SPEED, DESK_INPUT_DOUBLE_CLICK, DESK_INPUT_TOGGLE };
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
    DESK_CURSOR_ZOOM, DESK_CURSOR_ERASER,
    /* f2-99: append only; the existing native/v3 cursor IDs stay stable. */
    DESK_CURSOR_OPEN_HAND, DESK_CURSOR_CLOSED_HAND, DESK_CURSOR_HELP,
    /* fx-19: drag-and-drop feedback (IDropSource::GiveFeedback defaults);
     * no-drop uses DESK_CURSOR_FORBIDDEN. */
    DESK_CURSOR_DRAG_MOVE, DESK_CURSOR_DRAG_COPY, DESK_CURSOR_DRAG_LINK,
    DESK_CURSOR_COUNT
};
#define DESK_TITLE_MAX 63u
#define DESK_ARG_MAX 160u   /* LAUNCH argument: 12 + name + 4 + argument always fits one message.
                           * G15B: server-to-Help LAUNCH reuses its window;
                           * v3 client LAUNCH payload is u32 byte count + Help argument. */
enum desk_opcode {
    /* DAMAGE remains a rectangle in every version. A legacy producer with
     * no finer damage sends its full backing bounds; no new negotiation is
     * required for precise Win32, DOS-window or NetSurf updates. */
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
     * delegated /dev/mixer control handle (F4) and answers TASK_RESULT.
     * G22B also accepts 28 bytes: append line (1..5) and recording (0/1).
     * The flag word is mute for playback, Select for recording. A 20-byte
     * request retains the original master semantics. */
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
    /* v2 client to desktop, 8 bytes, no fds: uint32 disposition
     * (DESK_CLOSE_DECLINED, DESK_CLOSE_LATER, DESK_CLOSE_DOS_ACTIVE). G3:
     * no close deadline while the program answers PINGs (Windows 9x
     * WM_CLOSE); a program unresponsive for DESK_HUNG_NS gets the desktop's
     * "This program is not responding" dialog with End Task and Cancel. */
    DESK_CLOSE_ACK,
    DESK_GFX_QUERY_ENGINE, DESK_GFX_INFO_ENGINE, /* v1 generic engine payload, 256 bytes */
    DESK_JOYSTICK, DESK_JOYSTICK_RESULT /* v2 Control Panel broker, serial + 100-byte record (+ result on reply) */,
    /* fx-19 shell drag and drop (OLE DoDragDrop roles), version 2, no fds.
     * The data object is /system/settings/files.drag (fm_drag_save); paths
     * never travel in messages. All u32 after the 4-byte header:
     * DRAG_BEGIN (desktop-launched Files to desktop): button (1 left,
     *   2 right), item count 1..4096; the desktop then owns the pointer.
     * DRAG_OVER (desktop to a Files target): serial, phase (0 enter/over or
     *   the 100 ms DragOver repeat, 1 leave), client x, y, keys (FM_KEY_*).
     * DRAG_STATUS (Files target to desktop): serial, effect (DROPEFFECT
     *   bits, one or none), allowed effects (mask).
     * DRAG_DROP (desktop to the Files target): serial, client x, y, effect.
     * DRAG_END (desktop to the Files source): effect performed (0 none).
     * ACCEPT_FILES (any v2 client to desktop): 0/1, DragAcceptFiles.
     * DROP_FILES (desktop to an accepting client): serial, client x, y;
     *   WM_DROPFILES, the v2 client reads the data object (du_drop_file).
     * G18 v3: ACCEPT_FILES has one 0/1 word; DROP_FILES has the three
     * words above plus one read-only 1024-wide XRGB surface grant containing
     * a u32 byte count and an independent X\n<type>/path\n snapshot. The
     * receiver owns the transferred descriptor until it copies the data. */
    DESK_DRAG_BEGIN, DESK_DRAG_OVER, DESK_DRAG_STATUS, DESK_DRAG_DROP, DESK_DRAG_END,
    DESK_ACCEPT_FILES, DESK_DROP_FILES,
    /* G3 end of session (WM_QUERYENDSESSION / WM_ENDSESSION), version 2,
     * no fds, all u32 after the 4-byte header:
     * QUERY_END_SESSION (desktop to client, 12 bytes): serial (nonzero),
     *   flags (DESK_ENDSESSION_*, the WM_QUERYENDSESSION lParam). The client
     *   answers once with SESSION_ANSWER; it may show its own save prompt
     *   first (answer ASKING, then AGREE or VETO when the user decides).
     *   A registered client that answers a PING sent after the query
     *   without answering it agrees, as DefWindowProc returns TRUE.
     * END_SESSION (desktop to client, 16 bytes): serial, ending (1: every
     *   program agreed, exit now without prompting; 0: the session goes
     *   on), flags. Sent only to clients that answered the query.
     * SESSION_ANSWER (client to desktop, 12 bytes): serial of the query,
     *   answer (DESK_SESSION_*). Serial 0 with AGREE, sent once after
     *   CREATE_WINDOW, registers the window for queries; the desktop never
     *   sends QUERY_END_SESSION or END_SESSION to an unregistered client
     *   (it agrees by default and gets CLOSE_REQUEST/CLOSED at the end). */
    DESK_QUERY_END_SESSION, DESK_END_SESSION, DESK_SESSION_ANSWER,
    /* G11B keeps main's allocated wire numbers; integration opcodes follow. */
    DESK_POWER_EVENT = 87,
    DESK_POWER_REPLY = 88,
    /* G5 one system clipboard owned by the desktop (Windows 98 clipboard
     * model; docs/design/system-clipboard.md), version 2, any client.
     * CLIPBOARD (client to desktop), u32 after the header: serial (nonzero,
     *   echoed), op (DESK_CLIP_QUERY..NAME), format, flags, byte count,
     *   owner token, name byte count, name (REGISTER only, 1..127 UTF-8).
     *   A PUT with bytes and without DELAYED carries one surface holding the
     *   bytes from offset 0 (desk_clip_dims); other requests carry none.
     * CLIPBOARD_EVENT (desktop to client): serial (0 unsolicited), kind
     *   (DESK_CLIP_REPLY..DESTROY), op answered (REPLY) or 0, signed
     *   result, format, byte count, sequence, owner token, item count,
     *   items: count u32 formats, or count name bytes for a NAME reply.
     *   A successful GET reply with bytes carries one read-only surface. */
    DESK_CLIPBOARD = 92, DESK_CLIPBOARD_EVENT = 93,
    DESK_DOS_CLIPBOARD = 94 /* v2 desktop to DOS window: u32 action 1 Mark, 2 Copy, 3 Paste, 4 toggle QuickEdit, 5 toggle Exclusive mode */
};
enum desk_clip_op { DESK_CLIP_QUERY = 1, DESK_CLIP_EMPTY, DESK_CLIP_PUT, DESK_CLIP_CLOSE,
                    DESK_CLIP_GET, DESK_CLIP_WATCH, DESK_CLIP_REGISTER, DESK_CLIP_NAME, DESK_CLIP_OPEN, DESK_CLIP_RELEASE, DESK_CLIP_DISPLAY, DESK_CLIP_DISPLAY_REPLY };
enum desk_clip_kind { DESK_CLIP_REPLY = 1, DESK_CLIP_RENDER, DESK_CLIP_CHANGED, DESK_CLIP_DESTROY, DESK_CLIP_DISPLAY_EVENT };
#define DESK_CLIP_DELAYED 1u   /* PUT: SetClipboardData(format, NULL); owner only, no bytes */
#define DESK_CLIP_RENDERED 2u  /* PUT: the owner's answer to RENDER; zero bytes declines */
#define DESK_CLIP_OWNER 1u     /* QUERY reply flags in byte count: the clipboard has an owner */
#define DESK_CLIP_MINE 2u      /* ... and it is the caller */
#define DESK_CLIP_NAME_MAX 127u
#define DESK_CLIP_LIST_MAX 48u
#define DESK_CLIP_FORMATS_MAX 32u              /* stored formats */
#define DESK_CLIP_NAMES_MAX 256u               /* registered formats */
#define DESK_CLIP_ITEM_MAX 16777216u           /* one format: the surface limit */
#define DESK_CLIP_BYTES_MAX 16777216u          /* all stored formats */
#define DESK_CLIP_RENDER_NS 2000000000ull      /* WM_RENDERFORMAT answer */
/* Windows clipboard format ids (Winuser.h). */
#define DESK_CF_TEXT 1u
#define DESK_CF_BITMAP 2u
#define DESK_CF_METAFILEPICT 3u
#define DESK_CF_SYLK 4u
#define DESK_CF_DIF 5u
#define DESK_CF_TIFF 6u
#define DESK_CF_OEMTEXT 7u
#define DESK_CF_DIB 8u
#define DESK_CF_PALETTE 9u
#define DESK_CF_PENDATA 10u
#define DESK_CF_RIFF 11u
#define DESK_CF_WAVE 12u
#define DESK_CF_UNICODETEXT 13u
#define DESK_CF_ENHMETAFILE 14u
#define DESK_CF_HDROP 15u
#define DESK_CF_LOCALE 16u
#define DESK_CF_DIBV5 17u
#define DESK_CF_OWNERDISPLAY 0x80u
#define DESK_CF_DSPTEXT 0x81u
#define DESK_CF_PRIVATEFIRST 0x200u
#define DESK_CF_PRIVATELAST 0x2ffu
#define DESK_CF_REGISTERED 0xc000u
#define DESK_CLIP_PREFERRED_DROPEFFECT "Preferred DropEffect"
struct desk_clip_wire {
    uint32_t serial;
    uint32_t op;
    uint32_t kind;
    uint32_t format;
    uint32_t flags;
    uint32_t bytes;
    uint32_t token;
    uint32_t sequence;
    uint32_t count;
    int32_t result;
    union {   /* formats: QUERY reply; name: REGISTER request, NAME reply */
        uint32_t formats[DESK_CLIP_LIST_MAX];
        char name[DESK_CLIP_NAME_MAX + 1];
    };
};
#define DESK_ENDSESSION_CLOSEAPP 0x00000001u   /* lParam bits, as WinUser.h */
#define DESK_ENDSESSION_CRITICAL 0x40000000u
#define DESK_ENDSESSION_LOGOFF 0x80000000u
#define DESK_ENDSESSION_FLAGS 0xc0000001u
#define DESK_SESSION_VETO 0u      /* WM_QUERYENDSESSION returned FALSE (the user cancelled) */
#define DESK_SESSION_AGREE 1u     /* TRUE: data saved or discarded by the user */
#define DESK_SESSION_ASKING 2u    /* the program shows its own prompt; the session waits */
#define DESK_SESSION_DOS_ACTIVE 3u /* a DOS window whose program is running: "cannot shut down automatically" */
#define DESK_SESSION_ANSWERS 4u
/* CLOSE_ACK dispositions (v2): 0 declined (the user cancelled the program's
 * own prompt), 1 the program asks the user and closes later (no deadline
 * while it keeps answering PINGs), 2 a DOS window whose program is running
 * (the desktop shows the Windows 98 "cannot shut down this program
 * automatically" warning). V3 accepts 0 and 1 only. */
#define DESK_CLOSE_DECLINED 0u
#define DESK_CLOSE_LATER 1u
#define DESK_CLOSE_DOS_ACTIVE 2u
#define DESK_DRAG_FILE "/system/settings/files.drag"
#define DESK_DRAG_ITEMS 4096u
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
    struct ciuki_joystick_control joystick;
    struct desk_lease_tuple lease;
    unsigned opcode;
    uint32_t generation, owner, owner_generation, style, scale;
    int32_t x, y;
    uint32_t width, height, serial, focused;
    char title[DESK_TITLE_MAX + 1];   /* also WINDOW_INFO title and LAUNCH name */
    char argument[DESK_ARG_MAX + 1];  /* LAUNCH: optional single program argument (empty when absent) */
    char tooltip[128]; /* G18 tray tooltip (UTF-8 bytes) */
    struct ciuki_input_event event;
    uint32_t window_id, pid, flags, index, count, request;
    int32_t result;
    uint32_t values[3];               /* SETTINGS: left/right/mute or mode id/0/0 */
    uint32_t min_width, min_height, max_width, max_height, step_width, step_height;
    uint64_t performance[9]; /* frames, damage px, presents, LFB bytes, composite,
                             * present, input, max input lag (all ns), interval ns */
    struct ciuki_accel_info accelerator;
    struct ciuki_display_engine_info engine;
    uint32_t bpp;
    uint64_t benchmark[3]; /* destination bytes, elapsed ns, completed kernel ops */
    uint32_t source_w, source_h, damage_x, damage_y, damage_w, damage_h;
    uint32_t ui_x, ui_y, ui_w, ui_h;
    uint32_t resize_serial; /* optional GFX_FRAME replacement attachment */
    uint64_t completed_ns, next_frame_ns;
    uint32_t timing_serial,render_us,submit_us,wait_us; /* optional 84-byte FRAME */
    uint32_t input_settings[DESK_INPUT_VALUES];
    uint64_t convert_ns,copy_ns,slot_ns,refresh_ns; /* optional 64-byte PRESENTED */
    int attachment_fd; /* owned immutable DROP_FILES snapshot, -1 when absent */
    struct desk_clip_wire clip; /* G5 CLIPBOARD / CLIPBOARD_EVENT */
    int clipboard_fd; /* v3 GET reply only; receiver closes it */
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
/* G5: encode a clipboard request (client) or event (desktop) for protocol
 * version 2 or 3 (v3: window 0, generation 0, request token = serial for a
 * request); fd < 0 for none. Returns 0, or -EINVAL when desk_parse would
 * refuse the message. */
int desk_clip_message(struct ciuki_message *, unsigned version, int client,
                      const struct desk_clip_wire *, int fd);
/* Surface geometry carrying n clipboard bytes from offset 0. */
void desk_clip_dims(uint32_t n, uint32_t *width, uint32_t *height);
#endif
