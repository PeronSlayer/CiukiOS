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
    W32R_USER_CLEANUP /* index=TID, or zero for all USER identities of the PID */
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
