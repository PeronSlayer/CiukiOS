/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32SRV_REGISTRY_H
#define W32SRV_REGISTRY_H
#include <w32/server.h>
#include <stddef.h>
#include <stdint.h>
struct wr_value {
    struct wr_value *next;
    char *name;
    uint32_t type, bytes;
    uint8_t *data;
};
struct wr_key {
    struct wr_key *next, *parent;
    char *name;
    struct wr_value *values;
    unsigned hive, deleted, seen;
};
struct wr_handle {
    struct wr_handle *next;
    struct wr_key *key;
    uint32_t id, pid, access;
};
struct wr_hive { uint64_t sequence, dirty_since; unsigned slot, dirty; size_t text; };
/* I/O is injectable to fault the exact production write/fsync stages.
 * read: 0 absent, 1 present, -1 I/O error; caller frees returned bytes.
 * write: opens/truncates only the inactive slot; patch changes its marker.
 * fsync must flush data AND metadata. Diagnostics contain no private data. */
struct wr_io {
    void *opaque;
    int (*read)(void *, const char *, uint8_t **, size_t *);
    int (*write)(void *, const char *, const void *, size_t);
    int (*patch)(void *, const char *, size_t, const void *, size_t);
    int (*sync)(void *, const char *);
    void (*log)(void *, const char *);
};
#define WR_LIVE 64u
struct wr_end;
struct wr_server {
    struct wr_key *keys, *roots[4];
    struct wr_handle *handles;
    struct wr_hive hive[2];
    struct wr_io io;
    char user_path[128], user_name[32];
    uint32_t next_id, reserved_id, profile_owner;
    uint32_t compat_writer_pid; /* authenticated desktop owner; zero denies writes */
    uint64_t now;
    void *directory;
    /* f5-10C2 font bridge (fonts.c); NULL refuses W32R_FONT_* with 120.
     * Set after wr_init, which clears the server. */
    uint32_t (*font_call)(void *, uint32_t, const struct w32r_request *, const char *,
                          const void *, struct w32r_reply *, uint8_t *, size_t);
    void *fonts;
    /* f5-07B shell services (shell.h): PROGMAN DDE effects and the virtual
     * restart. NULL refuses W32R_PROGMAN_* / W32R_SHUTDOWN restart actions
     * with 120. shell_exit drops a dead client's conversation state. */
    uint32_t (*shell_call)(void *, uint32_t, const struct w32r_request *, const char *,
                           const char *, struct w32r_reply *, uint8_t *, size_t);
    void (*shell_exit)(void *, uint32_t);
    void (*shell_profile)(void *, const char *);
    void *shell;
    /* One pending end-session request (ExitWindowsEx without a desktop
     * window, InitiateSystemShutdown): consumed by the desktop's POLL. */
    struct { uint32_t pid, flags; uint64_t due; char message[256]; } session;
    /* Announced Win32 clients (ntdll sends W32R_CLIENT, wr_exit removes
     * them): the processes a virtual restart must close. */
    uint32_t live[WR_LIVE];
    unsigned live_count;
    struct wr_end *ending;
    struct wr_power *power_query;
    struct wr_power *power;   /* G11: the WM_POWERBROADCAST in progress */
};
int wr_init(struct wr_server *, const struct wr_io *, uint64_t now);
#define WR_CLIENT_MARKER "CiukiOS Win32 Client (f5-07B)"
uint32_t wr_client_join(struct wr_server *, uint32_t pid);
uint32_t wr_clients_reset(struct wr_server *); /* fresh desktop: prior-boot PIDs are not clients */
void wr_destroy(struct wr_server *);
uint32_t wr_flush(struct wr_server *);
uint32_t wr_tick(struct wr_server *, uint64_t now);
uint32_t wr_exit(struct wr_server *, uint32_t pid);
uint32_t wr_dispatch(struct wr_server *, uint32_t pid, const void *, size_t,
                     void *, size_t, size_t *);
uint32_t wr_crc(const void *, size_t);
int wr_equal(const char *, const char *);
int wr_name(const char *);
struct wr_key *wr_child(struct wr_server *, struct wr_key *, const char *);
struct wr_key *wr_add_key(struct wr_server *, struct wr_key *, const char *, unsigned);
uint32_t wr_put(struct wr_server *, struct wr_key *, const char *, uint32_t, const void *, uint32_t);
/* Removes one value: 0, or 2 when absent. Dirties the hive (commit follows). */
uint32_t wr_remove_value(struct wr_server *, struct wr_key *, const char *);
struct wr_key *wr_walk(struct wr_server *, unsigned hive, const char *path);
int wr_defaults(struct wr_server *, unsigned width, unsigned height, unsigned bpp);
int wr_load(struct wr_server *);
int wr_load_hive(struct wr_server *, unsigned);
uint32_t wr_select_profile(struct wr_server *, const char *, const char *);
uint32_t wr_commit(struct wr_server *, unsigned);
int wr_directory_init(struct wr_server *);
void wr_directory_free(struct wr_server *);
uint32_t wr_directory_call(struct wr_server *, uint32_t, const struct w32r_request *,
                           const char *, const void *, struct w32r_reply *, uint8_t *);
uint32_t wr_directory_exit(struct wr_server *, uint32_t);
uint32_t wr_reconcile(struct wr_server *, int (*alive)(void *, uint32_t), void *);
uint32_t wr_new_id(struct wr_server *);
void wr_posix_io(struct wr_io *, const char *directory);
/* Per authenticated channel. A malformed fragment discards only its request. */
struct wr_session {
    uint32_t pid, id, total, received;
    uint8_t *input, *output;
    size_t output_bytes;
};
void wr_session_close(struct wr_session *);
int wr_packet(struct wr_server *, struct wr_session *, const struct w32r_packet *,
              struct w32r_packet *);
#endif
