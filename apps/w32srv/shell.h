/* f5-07B: w32srv side of the PROGMAN DDE conversation (group folders and
 * .lnk files under C:\WINDOWS\Start Menu\Programs) and of the virtual
 * restart (WININIT.INI [rename], RunServicesOnce/RunOnce queue, journal).
 * The DDE messages themselves are answered in the client process by USER
 * (sdk/win32/user32/progman.c); only their effects reach this service.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32SRV_SHELL_H
#define W32SRV_SHELL_H
#include "registry.h"
#include <stddef.h>
#include <stdint.h>
/* Native file operations behind both services. Paths use '/'. Every call
 * returns 0 or a positive errno; stat reports ENOENT for an absent entry.
 * Host tests inject faults through this table; the guest uses ws_posix_fs.
 * The VFS resolves case and 8.3 aliases itself (vfs-storage-contract), so
 * paths are passed through without a second resolver. */
struct ws_fs {
    void *opaque;
    int (*stat)(void *, const char *, int *is_dir, uint64_t *size);
    int (*mkdir)(void *, const char *);
    int (*rmdir)(void *, const char *);
    int (*unlink)(void *, const char *);
    int (*rename)(void *, const char *from, const char *to); /* replaces an existing file */
    int (*read)(void *, const char *, uint8_t **, size_t *, size_t limit); /* caller frees; EFBIG above limit */
    int (*write)(void *, const char *, const void *, size_t); /* create or truncate, then fsync */
    int (*list)(void *, const char *, int (*cb)(void *, const char *, int is_dir), void *);
    void (*log)(void *, const char *);
};
void ws_posix_fs(struct ws_fs *);
/* CiukiOS .lnk v3 (apps/desktop/shell_link.h, CIUKLNK3): the bytes equal
 * desk_link_encode's for the same record (host test). ws_link_store is the
 * seam f5-15A's IShellLink/IPersistFile writer replaces; the desktop keeps
 * decoding the same format. */
struct ws_link {
    const char *label, *target, *arguments, *directory, *icon;
    uint32_t show;
    int32_t icon_index;
};
#define WS_LINK_BYTES 4096u
unsigned ws_link_encode(const struct ws_link *, uint8_t *out, unsigned cap);
/* Decoded copy (bounded fields) for the group description request. */
struct ws_link_record { char label[64], target[260], arguments[1024], directory[260], icon[260]; uint32_t show; int32_t icon_index; };
int ws_link_decode(const uint8_t *, unsigned count, struct ws_link_record *);
/* One parsed PROGMAN command. ws_parse_next advances *cursor over
 * "[opcode(args...)]" and returns 1, 0 at the end of the string (or at text
 * that does not begin another bracketed command, which Windows ignores), or
 * -1 on a syntax error (missing bracket or parenthesis, unterminated quote,
 * argument that ends in neither comma nor parenthesis). Quoted arguments
 * keep commas, parentheses and brackets. */
#define WS_ARGS 10u
#define WS_ARG 1024u
struct ws_command { char opcode[32]; unsigned argc; char args[WS_ARGS][WS_ARG]; };
int ws_parse_next(const char **cursor, struct ws_command *out);
/* DDEACK status bits of a WM_DDE_ACK low word. */
#define WS_DDE_FACK 0x8000u
#define WS_DDE_FBUSY 0x4000u
#define WS_DDE_FNOTPROCESSED 0x0000u
struct ws_config {
    const char *programs; /* native folder of C:\WINDOWS\Start Menu\Programs */
    const char *windows;  /* native folder of C:\WINDOWS */
};
struct ws_shell;
/* Registers nothing: the caller sets wr_server.shell_call/shell_exit/shell. */
struct ws_shell *ws_create(const struct ws_fs *, const struct ws_config *, struct wr_server *);
void ws_destroy(struct ws_shell *);
/* sec-user-05: whether the session user is an administrator (common
 * program groups). The guest asks the kernel for its own credential at
 * ws_create; host tests set it here. */
void ws_set_administrator(struct ws_shell *, int);
/* wr_server hooks: W32R_PROGMAN_* and W32R_SHUTDOWN restart actions. */
uint32_t ws_call(void *, uint32_t pid, const struct w32r_request *, const char *path,
                 const char *name, struct w32r_reply *, uint8_t *bytes, size_t cap);
void ws_exit(void *, uint32_t pid);
/* Direct entry points (tests, the service loop). execute: DDEACK bits of
 * the whole string, *executed = commands that succeeded before the first
 * failure. request: 0 and CF_TEXT bytes (with NUL) in out, 2 for an
 * unknown item, 234 when the item does not fit. */
uint32_t ws_progman_execute(struct ws_shell *, uint32_t pid, const char *commands, unsigned *executed);
uint32_t ws_progman_request(struct ws_shell *, const char *item, char *out, size_t cap, size_t *n);
/* Restart phases (W32R_RESTART_*) and the journal C:\WINDOWS\CIUKIRST.LOG.
 * ws_restart_tick does at most one WININIT.INI line (or the closing rename)
 * per call and returns 1 while more work remains; the service loop calls it
 * on every iteration, tests call it until it returns 0. */
unsigned ws_restart_phase(const struct ws_shell *);
int ws_restart_pending(struct ws_shell *);
uint32_t ws_restart_begin(struct ws_shell *, uint32_t flags);
int ws_restart_tick(struct ws_shell *);
uint32_t ws_restart_next(struct ws_shell *, char *command, size_t cap, uint32_t *type, uint32_t *handed);
uint32_t ws_restart_end(struct ws_shell *);
#define WS_WININIT_LIMIT 65536u  /* Windows profile files are at most 64 KiB */
#define WS_WININIT_LINES 512u
#define WS_RUNS_MAX 128u         /* RunOnce entries per restart (an entry may re-add itself) */
#define WS_GROUPS 64u
#define WS_ITEMS 128u
#endif
