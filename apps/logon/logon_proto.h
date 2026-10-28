/* SPDX-License-Identifier: MIT
 * Private LOGON service protocol (sec-user-03), shared by the sealed LOGON
 * service, LOGON_UI and the session desktop. Not an SDK ABI.
 *
 * The LOGON endpoint (call-101 service 5) keeps operations 1..7 of the main
 * contract (section 4.1) unchanged. sec-user-03 adds two private operations,
 * each authorized by the kernel-retained role of the connected peer:
 *
 *   UI_STATE=8       LOGON_UI only. 32-byte command, 96-byte state reply.
 *   SESSION_EVENT=9  same-session DESKTOP only. 32-byte notice, 64-byte reply.
 *
 * All bodies are little-endian, zero-padded, schema 1, without attachments.
 * Neither operation authenticates, unlocks or creates a session: UI_STATE
 * reports the trusted screen LOGON wants shown and relays the secure-console
 * menu choices that need the user's own desktop; SESSION_EVENT hands those
 * choices to the desktop of the same session (veto-aware Log Off, Task
 * Manager, Shut Down, Restart, password page) and lets it tell LOGON why its
 * session is ending. SESSION_LOCK=5 keeps its 32-byte body; its 16-byte
 * progress reply carries result_status LGN_STATUS_NO_PASSWORD (-EPERM) when
 * the session owner has no password (set-password guidance, no false lock).
 */
#ifndef CIUKI_LOGON_PROTO_H
#define CIUKI_LOGON_PROTO_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define LGN_SERVICE_AUTH 1u
#define LGN_SERVICE_LOGON 5u
#define LGN_ROLE_LOGON 1u
#define LGN_ROLE_UI 2u
#define LGN_ROLE_ACCOUNT 3u
#define LGN_ROLE_DESKTOP 6u

/* LOGON service operations (1..7 frozen in the main contract). */
enum lgn_operation {
    LGN_OP_AUTH_OUTCOME = 1, LGN_OP_LOCK_EVENT = 2, LGN_OP_POWER_EVENT = 3, LGN_OP_BEGIN_AUTH = 4,
    LGN_OP_SESSION_LOCK = 5, LGN_OP_SUBMIT_AUTH = 6, LGN_OP_ENROLL_PLAN = 7,
    LGN_OP_UI_STATE = 8, LGN_OP_SESSION_EVENT = 9
};
/* AUTH (service 1) operations used by LOGON and LOGON_UI. */
enum lgn_auth_operation {
    LGN_AUTH_LIST = 1, LGN_AUTH_LOGON = 2, LGN_AUTH_UNLOCK = 3, LGN_AUTH_ENROLL = 4,
    LGN_AUTH_CHALLENGE = 5, LGN_AUTH_AUTOLOGON = 6, LGN_AUTH_ENROLL_PLAN = 7
};
/* Trusted UI event actions (contract 4.1, 32-byte events). */
#define LGN_EVENT_LOCK 1u
#define LGN_EVENT_MENU_RETURN 2u
#define LGN_EVENT_SHUTDOWN 3u
#define LGN_EVENT_RESTART 4u
/* AUTH_OUTCOME dispositions. */
#define LGN_AUTHENTICATED 1u
#define LGN_ENROLL_STAGED 2u
#define LGN_ENROLL_COMMITTED 3u
#define LGN_FAILED 4u

/* Screens LOGON asks LOGON_UI to show (UI_STATE reply mode). */
enum lgn_mode {
    LGN_MODE_STARTING = 1,   /* waiting for the account service / automatic logon */
    LGN_MODE_WELCOME = 2,    /* Welcome: enabled accounts and typed names */
    LGN_MODE_LOADING = 3,    /* authenticated: profile and session starting */
    LGN_MODE_HIDDEN = 4,     /* the user's desktop owns the console */
    LGN_MODE_LOCKED = 5,     /* the session is locked; owner password required */
    LGN_MODE_SECURITY = 6,   /* secure attention menu of an unlocked session */
    LGN_MODE_ENDING = 7,     /* the session is ending (logging off) */
    LGN_MODE_UNAVAILABLE = 8,/* logon is not available (account service) */
    LGN_MODE_POWER = 9       /* shutting down or restarting */
};
/* UI_STATE flags. */
#define LGN_STATE_BLANK 1u      /* the session owner has no password */
#define LGN_STATE_SAFE 2u       /* this boot is Safe Mode */
#define LGN_STATE_SESSION 4u    /* an interactive session exists */
#define LGN_STATE_DESKTOP 8u    /* the session's desktop answers SESSION_EVENT */
#define LGN_STATE_RESTART 16u   /* LGN_MODE_POWER: restarting, not shutting down */
/* UI_STATE message codes: a one-shot notice for the trusted screen. */
enum lgn_message {
    LGN_MSG_NONE = 0,
    LGN_MSG_SESSION_ENDED = 1,   /* the desktop stopped unexpectedly */
    LGN_MSG_START_FAILED = 2,    /* the session could not be started */
    LGN_MSG_PROFILE_FAILED = 3,  /* the profile/session could not be prepared */
    LGN_MSG_POWER_FAILED = 4,    /* shutdown/restart was refused */
    LGN_MSG_BLANK_LOCK = 5,      /* lock refused: set a password first */
    LGN_MSG_NO_EMPTY_UNLOCK = 6, /* a passwordless owner cannot unlock */
    LGN_MSG_SERVICE_RESTART = 7  /* a trusted service restarted: locked */
};
/* UI_STATE commands (request u32@4). */
enum lgn_command {
    LGN_CMD_QUERY = 0,
    LGN_CMD_ATTENTION = 1,       /* CIUKI_INPUT_ATTENTION observed on the trusted grant */
    LGN_CMD_LOGOFF = 2,          /* security menu: veto-aware Log Off by the desktop */
    LGN_CMD_TASKMGR = 3,         /* security menu: Task Manager on the desktop */
    LGN_CMD_FORCE_LOGOFF = 4,    /* confirmed: end the session without asking programs */
    LGN_CMD_SET_PASSWORD = 5,    /* security menu: the desktop opens the password page */
    LGN_CMD_ACKNOWLEDGE = 6,     /* the screen showed the one-shot message */
    LGN_CMD_SHUTDOWN = 7,        /* security menu: veto-aware Shut Down by the desktop */
    LGN_CMD_RESTART = 8          /* security menu: veto-aware Restart by the desktop */
};
/* SESSION_EVENT actions for the desktop (reply u32@8). */
enum lgn_action {
    LGN_ACT_NONE = 0, LGN_ACT_LOGOFF = 1, LGN_ACT_TASKMGR = 2, LGN_ACT_SHUTDOWN = 3,
    LGN_ACT_RESTART = 4, LGN_ACT_SET_PASSWORD = 5
};
/* SESSION_EVENT notices from the desktop (request u32@4). */
enum lgn_notice {
    LGN_NOTICE_QUERY = 0,
    LGN_NOTICE_LOGOFF = 1,       /* its own query/veto finished: the session ends now */
    LGN_NOTICE_SHUTDOWN = 2,     /* ...then LOGON shuts the machine down */
    LGN_NOTICE_RESTART = 3,      /* ...then LOGON restarts the machine */
    LGN_NOTICE_PASSWORD = 4      /* the owner set a password (sec-user-04 hook) */
};
/* SESSION_EVENT reply flags. */
#define LGN_EVENT_BLANK 1u
#define LGN_EVENT_RECOVERY 2u

/* Progress result_status of SESSION_LOCK/LOCK_EVENT for a passwordless owner. */
#define LGN_STATUS_NO_PASSWORD (-EPERM)
#define LGN_UI_COMMAND_BYTES 32u
#define LGN_UI_STATE_BYTES 96u
#define LGN_SESSION_NOTICE_BYTES 32u
#define LGN_SESSION_EVENT_BYTES 64u
#define LGN_PROGRESS_BYTES 16u
#define LGN_SUBMIT_BYTES 208u

struct lgn_ui_command { uint32_t command, session, console; };
struct lgn_state {
    uint32_t mode, flags, session, console, owner_uid, message;
    int32_t error;
    uint64_t generation;
    char owner_name[32];
};
struct lgn_session_notice { uint32_t notice, session; };
struct lgn_session_event { uint32_t action, flags, uid, session; char name[32]; };
struct lgn_progress { uint32_t disposition; int32_t status; uint32_t retry_after_ms; };

int lgn_ui_command_encode(const struct lgn_ui_command *, void *, size_t);
int lgn_ui_command_decode(struct lgn_ui_command *, const void *, size_t);
int lgn_state_encode(const struct lgn_state *, void *, size_t);
int lgn_state_decode(struct lgn_state *, const void *, size_t);
int lgn_notice_encode(const struct lgn_session_notice *, void *, size_t);
int lgn_notice_decode(struct lgn_session_notice *, const void *, size_t);
int lgn_event_encode(const struct lgn_session_event *, void *, size_t);
int lgn_event_decode(struct lgn_session_event *, const void *, size_t);
int lgn_progress_encode(const struct lgn_progress *, void *, size_t);
int lgn_progress_decode(struct lgn_progress *, const void *, size_t);
/* 208-byte SUBMIT_AUTH (contract 4.1) from a decoded 64-byte challenge;
 * name empty for a UID selector. Secrets are copied; the caller wipes. */
int lgn_submit_encode(const void *challenge64, const char *name, const char *secret,
                      const char *new_secret, void *out208);
/* The account service's name rule (acct_name_valid): 1-31 ASCII letters,
 * digits, '_' and '-'. A typed name failing it is refused before any request. */
int lgn_name_syntax(const char *);
/* Printable ASCII 0x20..0x7e, at most 63 bytes (acct_password_valid, short allowed). */
int lgn_secret_syntax(const char *);
void lgn_wipe(void *, size_t);

/* ---- CSC1 client (UI and desktop) over call 101 links ---------------- */
struct ciuki_security_args;
struct ciuki_message;
/* System seam: the target binds it to the SDK (logon_client_ciuki.c), the
 * host harness to the kernel host harness. 0/-errno as the SDK calls. */
struct lgn_client_sys {
    void *ctx;
    int (*security)(void *, struct ciuki_security_args *, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes);
    int (*send)(void *, int fd, const struct ciuki_message *);
    int (*recv)(void *, int fd, struct ciuki_message *); /* nonblocking: 1, 0 end, -EAGAIN, -errno */
    int (*wait)(void *, int fd, unsigned timeout_ms);    /* >0 readable, 0 timeout, -errno */
    int (*close)(void *, int fd);
    uint64_t (*now_ms)(void *);
};
struct lgn_client {
    const struct lgn_client_sys *sys;
    int fd;
    uint32_t service, server_role, endpoint_generation, service_generation, peer_pid, session;
    uint64_t next_id;
};
void lgn_client_init(struct lgn_client *, const struct lgn_client_sys *, uint32_t service, uint32_t server_role);
/* CONNECT once; 0, -ESRCH while the server is not registered, -errno. */
int lgn_client_connect(struct lgn_client *);
void lgn_client_close(struct lgn_client *);
/* One request/reply. reply_status is the frame status (0 or -errno); the
 * return value is the transport result (0, -ETIMEDOUT, -ESRCH...). The
 * request body is never kept; the frame copy is wiped. */
int lgn_client_call(struct lgn_client *, uint32_t operation, uint64_t generation, const void *body, size_t bytes,
                    void *reply, size_t capacity, size_t *used, int *reply_status, unsigned timeout_ms);
#endif
