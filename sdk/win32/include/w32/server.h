/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#ifndef W32_SERVER_H
#define W32_SERVER_H
#include <stdint.h>
#define W32R_VERSION 1u
#define W32R_NAME 1021u /* 255 UTF-8 scalars, plus NUL */
#define W32R_PATH 4096u
#define W32R_DATA 65536u
#define W32R_TEXT (4u * 1024u * 1024u)
#define W32R_MAX (W32R_DATA + W32R_PATH + W32R_NAME + 64u)
#define W32R_HKCR 0x80000000u
#define W32R_HKCU 0x80000001u
#define W32R_HKLM 0x80000002u
#define W32R_HKU 0x80000003u
#define W32R_HKCC 0x80000005u
#define W32R_HKDD 0x80000006u
enum w32r_op {
    W32R_OPEN = 1, W32R_CREATE, W32R_CLOSE, W32R_SET, W32R_QUERY,
    W32R_DELETE_KEY, W32R_DELETE_VALUE, W32R_ENUM_KEY, W32R_ENUM_VALUE,
    W32R_INFO, W32R_FLUSH, W32R_USERNAME,
    W32R_ATOM_ADD, W32R_ATOM_FIND, W32R_ATOM_DELETE, W32R_ATOM_NAME,
    W32R_WINDOW_ALLOC, W32R_WINDOW_SET, W32R_WINDOW_GET, W32R_WINDOW_ENUM,
    W32R_WINDOW_FREE, W32R_SHUTDOWN, W32R_COMPAT,
    W32R_PROFILE_ACQUIRE = 24, W32R_PROFILE_RELEASE,
    /* Ephemeral clipboard, copied data; key=HWND, index=TID, type=format.
     * Reply key=owner or prior viewer, type=head viewer, disposition=old owner.
     * OPEN/CLOSE are authenticated PID+TID locks, never persisted. */
    W32R_CLIP_OPEN=0x100, W32R_CLIP_CLOSE, W32R_CLIP_EMPTY, W32R_CLIP_SET,
    W32R_CLIP_GET, W32R_CLIP_QUERY, W32R_CLIP_OWNER, W32R_CLIP_VIEWER,
    W32R_CLIP_CHAIN, W32R_WINDOW_CLASS,
    W32R_USER_CLEANUP, /* index=TID, or zero for all USER identities of the PID */
    /* f5-07B PROGMAN DDE server side (apps/w32srv/progman.c). The client
     * process's USER answers the DDE messages in-process and forwards the
     * conversation's effects here. OPEN: path=service, name=topic, reply key
     * is a durable non-reused conversation ID. EXECUTE/REQUEST/CLOSE use that
     * ID in index, authenticated to the caller PID. EXECUTE: path = the UTF-8 command string
     * ("[CreateGroup(x)][AddItem(...)]"), name = "", reply key = DDEACK bits
     * (0x8000 fAck) and type = commands executed. REQUEST: name = item
     * ("Groups", "PROGMAN" or a group name), reply data = CF_TEXT bytes with
     * a NUL. CLOSE ends that conversation; the focused group is shared. Typed
     * refusals: 87 malformed, 120 service absent, 8 bounds. */
    W32R_PROGMAN_EXECUTE = 0x140, W32R_PROGMAN_REQUEST, W32R_PROGMAN_CLOSE, W32R_PROGMAN_OPEN,
    W32R_ENDSESSION, W32R_CLIENT,
    /* f5-10C2 PE-to-native font bridge (docs/design/fonts-contract.md,
     * "PE-to-native font bridge"): w32srv rasterises with libfont (FreeType
     * 2.13.3, v35, grey). path = native face path in the system or install
     * store, name = "", key/access/flags zero. GLYPHS: index = em size in
     * 26.6 pixels (64..8192), type = glyph count (1..W32R_FONT_BATCH), data =
     * uint32 glyph indices; reply data = count records (struct w32r_glyph
     * then width*height coverage bytes), as many as fit W32R_DATA. CMAP:
     * index = first code point (multiple of 256, below 0x110000), no data;
     * reply data = 256 uint16 glyph indices (0 = missing). STATS: path "",
     * reply data = struct w32r_font_stats. Face replies carry the face's
     * resident bytes in key. Typed refusals: 13 not a store TrueType face
     * (CFF, collection, damaged directory), 50 unsupported em size, 8 face
     * larger than the shared budget, 5 path outside the stores, 2 missing. */
    W32R_FONT_GLYPHS = 0x180, W32R_FONT_CMAP, W32R_FONT_STATS
};
/* ENDSESSION: desktop BEGIN(index=1,flags=EWX), STATUS(4), RESET(5).
 * BEGIN may include <=128 little-endian HWND/PID pairs in compositor order;
 * the broker validates ownership and appends unranked hidden windows.
 * USER POLL(2,index is action, type=TID), REPLY(3,key=HWND,type=TID,
 * flags=query answer). reply POLL: key=HWND,type=message,count=wParam,
 * disposition=lParam. STATUS: key=0 pending,1 accepted,2 vetoed,3 held.
 * Notifications are serialized, delivered on the owning UI thread. */
#define W32R_SESSION_BEGIN 1u
#define W32R_SESSION_POLL 2u
#define W32R_SESSION_REPLY 3u
#define W32R_SESSION_STATUS 4u
#define W32R_SESSION_RESET 5u
/* G3 (the desktop's Shut Down, Restart, Log Off): BEGIN with flags |
 * HOLD has no 10 s deadline and, once every window agreed, holds (STATUS
 * key 3) until COMMIT (WM_ENDSESSION TRUE to the queried windows, then key
 * 1) or CANCEL (FALSE to those that agreed, then key 2; CANCEL also stops a
 * running query). While a query runs, STATUS also returns the window in
 * turn: count = its PID, type = HWND, disposition = 1 once its USER thread
 * fetched the message (POLL). Both are desktop-only (compat_writer_pid). */
#define W32R_SESSION_CANCEL 6u
#define W32R_SESSION_COMMIT 7u
#define W32R_SESSION_HOLD 0x100u
#define W32R_FONT_BATCH 64u
/* f5-07B end-session and virtual restart (W32R_SHUTDOWN; index selects the
 * action; key and data_bytes are zero, access is ignored).
 * REQUEST (any client): flags = EWX_* bits (0..0x1f), type = delay in seconds
 * (InitiateSystemShutdown; zero for ExitWindowsEx without a window), path =
 * message text or ""; one pending request per service (1115 when one is
 * pending). ABORT (any client): clears a pending request that is not yet due
 * (1116 if none). The remaining actions need the authenticated desktop
 * (compat_writer_pid, else 5):
 * POLL consumes a due request: reply key = requesting PID, type = flags,
 * data = message with NUL (data_bytes != 0: consumed), count = milliseconds
 * until due when nothing was consumed; with flags = 1 (the desktop's boot
 * check only: it stats WININIT.INI) disposition = 1 when restart work is
 * pending (see STATUS). LIST: reply count = number of authenticated clients,
 * data = that many little-endian PIDs, for the processes the desktop must
 * close. STATUS: key = restart phase (W32R_RESTART_*), disposition = 1 when
 * work is pending: a journalled restart interrupted earlier, a
 * C:\WINDOWS\WININIT.INI, or RunServicesOnce/RunOnce entries. BEGIN (flags =
 * EWX bits, zero at boot): commits the hives, journals the restart in
 * C:\WINDOWS\CIUKIRST.LOG and starts the incremental WININIT.INI [rename]
 * pass (one line per service tick, then WININIT.BAK); repeating BEGIN while a
 * restart is running or resumed is accepted. NEXT: 170 (busy) while the
 * WININIT pass runs; then deletes and returns (data = UTF-8 command with NUL,
 * type = queue 0 RunServicesOnce / 1 HKLM RunOnce / 2 HKCU RunOnce, key =
 * entries handed out) the next entry in registry order, 259 when none remain.
 * END: 170 until NEXT has returned 259; commits the hives and removes the
 * journal. */
#define W32R_SHUTDOWN_STATUS 1u
#define W32R_SHUTDOWN_BEGIN 2u
#define W32R_SHUTDOWN_NEXT 3u
#define W32R_SHUTDOWN_END 4u
#define W32R_SHUTDOWN_REQUEST 5u
#define W32R_SHUTDOWN_POLL 6u
#define W32R_SHUTDOWN_ABORT 7u
#define W32R_SHUTDOWN_LIST 8u
#define W32R_RESTART_IDLE 0u
#define W32R_RESTART_WININIT 1u
#define W32R_RESTART_RUNONCE 2u
#define W32R_SESSION_MESSAGE 256u /* InitiateSystemShutdown message bytes, with NUL */
/* Win32 orientation: top is the distance from the baseline up to the first
 * coverage row (GLYPHMETRICS.gmptGlyphOrigin.y, FreeType bitmap_top). */
struct w32r_glyph { uint32_t glyph; uint16_t width, height; int16_t left, top; int32_t advance; };
struct w32r_font_stats {
    uint32_t version, faces, open_faces, face_bytes, face_peak, budget;
    uint32_t requests, glyphs, cmap_pages, opens, evictions, refusals;
};
/* No pointers on the wire. Request tail = path NUL, name NUL, data bytes.
 * Strings are UTF-8; value bytes are canonical UTF-16LE for string types.
 * action-specific words: index, type, access, flags. Reply tail = name NUL,
 * data; counts are bytes, excluding name NUL. Authenticated PID is supplied
 * by the accepted channel, never by this record. A deleted key handle gives
 * ERROR_KEY_DELETED (1018) until closed. Handles are private to one PID. */
struct w32r_request {
    uint32_t version, op, key, index, type, access, flags;
    uint32_t path_bytes, name_bytes, data_bytes;
};
struct w32r_reply {
    uint32_t version, error, key, disposition, type;
    uint32_t count, max_name, max_data, name_bytes, data_bytes;
};
/* WINDOW_SET data is this fixed record followed by title UTF-8 NUL.
 * Only the owning PID can set/free; ENUM index visits top-level windows
 * (flags bit 0 includes children); WINDOW_CLASS returns UTF-8 class name.
 * HWND generations and atom identities are checkpointed before publication.
 * Atom index selects 0=global or 1=process-local; key carries an atom ID.
 * Names compare case-insensitively; atoms 1..0xbfff are integer atoms. */
struct w32r_window { uint32_t hwnd, pid, tid, parent, class_atom, flags; };
/* One native message, exactly 256 bytes. Stop-and-wait fragments avoid
 * exhausting the 64-message kernel queue. Each client has one in-flight
 * transaction. BEGIN resets a prior partial transaction; READ pulls replies.
 * No mutation is retried automatically after an ambiguous disconnect. */
#define W32R_BEGIN 1u
#define W32R_WRITE 2u
#define W32R_READ 3u
struct w32r_packet {
    uint32_t version, id, kind, total, offset, error;
    uint8_t bytes[232];
};
_Static_assert(sizeof(struct w32r_request) == 40, "registry request");
_Static_assert(sizeof(struct w32r_reply) == 40, "registry reply");
_Static_assert(sizeof(struct w32r_packet) == 256, "registry packet");
_Static_assert(sizeof(struct w32r_glyph) == 16, "font glyph record");
_Static_assert(sizeof(struct w32r_font_stats) == 48, "font statistics");
/* Frozen f5-04 service record, repeated without kernel-private declarations.
 * CIUKI_SYS_W32=96, operation=0x200. REGISTER=1 CONNECT=2 ACCEPT=3.
 * The production kernel layout is checked against this prefix by host tests. */
struct w32r_service {
    uint32_t version, size, operation, reserved;
    uint32_t action, service, pid, endpoint, result, error;
};
_Static_assert(sizeof(struct w32r_service) == 40, "service record");
/* Process-wide serialized transport shared by USER, kernel32 and advapi32.
 * ntdll holds FastPebLock across every fragment, including response reads. */
uint32_t w32_server_exchange(const void *,uint32_t,void *,uint32_t,uint32_t *);
#endif
