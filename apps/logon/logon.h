/* SPDX-License-Identifier: MIT
 * Sealed SYSTEM LOGON coordinator (/System/Services/logon), sec-user-03.
 *
 * PID 1 starts it with the kernel LOGON role (sec-k01). It owns the trusted
 * gate: the boot secure console, the frozen AUTH relay to ACCOUNT through
 * apps/accounts/logon.c, session creation and launch (SESSION_CREATE,
 * SESSION_LAUNCH w32srv then desktop, LAUNCH_POLL), lock/unlock
 * (SECURE_ENTER/SECURE_LEAVE), secure attention, Log Off collection
 * (SESSION_END) and the logon screen's fixed power requests. It is a single
 * nonblocking event loop: no call waits for a client, ACCOUNT's AUTH_OUTCOME
 * is accepted while an AUTH request of its own is outstanding, and every
 * kernel call it makes is nonblocking (call 101) except the final power call.
 *
 * Not an SDK ABI. The system seam lets the host harness drive the production
 * loop over the kernel host harness (tests/host/security/logon_service_test.c).
 */
#ifndef CIUKI_LOGON_H
#define CIUKI_LOGON_H
#include "logon_proto.h"
#include "../accounts/accounts.h"
#include <ciuki/abi.h>

#define LGN_LINKS 16u
#define LGN_BODY_MAX (32u + 40u * 16u)   /* largest accepted request: ENROLL_PLAN */
#define LGN_REPLY_MAX (16u + 144u * 18u) /* largest AUTH reply: LIST */
#define LGN_QUEUE 4u
#define LGN_POLL_MS 20u
#define LGN_COLLECT_MS 50u
#define LGN_LIST_RETRY_MS 250u
#define LGN_UNAVAILABLE_MS 30000u
#define LGN_CREATE_BUSY_MS 10000u
#define LGN_SUBMIT_MS 30000u

/* System seam. security() places in/out like the account service's seam
 * (spawn_address/output_address); SESSION_CREATE passes credential[64]
 * followed by profile[112] as in (credential_address/profile_address). */
struct lgn_sys {
    void *ctx;
    int (*security)(void *, struct ciuki_security_args *, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes);
    int (*send)(void *, int fd, const struct ciuki_message *);
    int (*recv)(void *, int fd, struct ciuki_message *);   /* nonblocking: 1, 0 end, -EAGAIN, -errno */
    int (*close)(void *, int fd);
    int (*wait)(void *, const int *fds, unsigned count, unsigned timeout_ms);
    uint64_t (*now_ms)(void *);
    int (*boot_flags)(void *, uint32_t *);
    int (*power)(void *, unsigned action);                 /* LGN_EVENT_SHUTDOWN/RESTART: orderly path */
    int (*pids)(void *, uint32_t *, unsigned capacity);    /* live pids */
    void (*log)(void *, const char *);
};

/* Accepted LOGON endpoint (service 5), one exchange at a time. */
struct lgn_link {
    int fd, occupied, retiring;
    unsigned refs;
    uint32_t serial, endpoint_generation, peer_role, peer_pid, peer_process_generation, session_id, service_generation;
    uint8_t header[64], body[LGN_BODY_MAX];
    uint32_t received, total, operation;
    uint64_t request_id, last_request, frame_generation, pending_since, attempt;
    int receiving, pending;
};
/* Queued LOGON -> ACCOUNT AUTH request. */
struct lgn_auth_request {
    uint32_t operation, bytes, purpose, link;
    uint64_t generation, attempt;
    uint8_t body[LGN_BODY_MAX];
};
enum lgn_purpose { LGN_PURPOSE_LIST = 1, LGN_PURPOSE_CHALLENGE, LGN_PURPOSE_VERIFY, LGN_PURPOSE_PLAN,
                   LGN_PURPOSE_SOURCE, LGN_PURPOSE_PROBE /* sec-user-04: ENROLL_SOURCE relay / enrollment probe */ };
struct lgn_auth {
    int fd;
    unsigned refs;
    uint32_t serial, endpoint_generation, service_generation, peer_pid, peer_process_generation;
    uint64_t next_id;
    int busy;
    struct lgn_auth_request active, queue[LGN_QUEUE];
    unsigned queued;
    uint64_t id, sent_at;
    uint8_t reply[LGN_REPLY_MAX];
    uint32_t got, total;
    int32_t status;
};

enum lgn_phase { LGN_NONE = 0, LGN_CREATE, LGN_LAUNCH_W32, LGN_LAUNCH_DESKTOP, LGN_POLL, LGN_ACTIVE, LGN_ENDING };
enum lgn_end { LGN_END_CRASH = 0, LGN_END_LOGOFF, LGN_END_SHUTDOWN, LGN_END_RESTART, LGN_END_FAILED, LGN_END_FORCED };
struct lgn_session {
    uint32_t phase, id, generation, uid, end, pending_action;
    int blank, recovery, adopted, end_begun;
    char name[32];
    uint8_t credential[64], profile[112];
    struct ciuki_security_launch launch[2];   /* index = purpose - 1: desktop, w32srv */
    struct ciuki_security_console console;
    int console_valid;
    uint64_t started, next_poll, create_until;
};
/* Pending verification from a UI link (or the internal autologon, link=~0). */
struct lgn_verify { uint64_t attempt; uint32_t link, action; int empty_secret, outcome_seen, occupied; uint64_t started; };

struct lgn {
    const struct lgn_sys *sys;
    struct acct_logon correlation;          /* apps/accounts/logon.c; .kernel is LOGON's binding (links.c) */
    struct lgn_link links[LGN_LINKS];
    struct lgn_auth auth;
    struct lgn_session session;
    struct lgn_verify verify[4];
    struct ciuki_security_console boot;     /* session-0 trusted gate */
    int boot_valid, safe, autologon_done, logged_off, accounts_ready, list_pending, power_issued, autologon_candidate;
    uint32_t own_generation, serial, mode, message, enter_reason, power_action;
    int32_t error;
    uint64_t state_generation, accounts_generation, started, next_list, next_auth_connect, next_enter, unavailable_since;
    char log_line[160];
    /* sec-user-04: the armed administrator log-off of the locked session
     * (UI_STATE OVERRIDE), the G32 enrollment workflow (LIST ENOENT or a
     * provisional catalogue, confirmed by ENROLL_SOURCE) and its probe. */
    int override, enrolling, probe_pending;
    uint64_t next_probe;
};

int lgn_init(struct lgn *, const struct lgn_sys *);
/* One pass: accept, receive, advance timers, then wait at most idle_ms. */
void lgn_step(struct lgn *, unsigned idle_ms);
/* Nonblocking part of a pass (the host harness interleaves other roles). */
void lgn_pass(struct lgn *);
/* Current screen for LOGON_UI (UI_STATE reply). */
void lgn_state(const struct lgn *, struct lgn_state *);
void lgn_close(struct lgn *);

/* links.c: endpoints, CSC1 frames and LOGON's acct_kernel binding. */
int lgn_links_bind(struct lgn *);
void lgn_links_accept(struct lgn *);
void lgn_links_receive(struct lgn *);
int lgn_reply(struct lgn *, unsigned link, int status, const void *body, size_t bytes);
int lgn_retain(struct lgn *, unsigned link, struct acct_subject *);
int lgn_retain_auth(struct lgn *, struct acct_subject *);
void lgn_link_release(struct lgn *, unsigned link);
int lgn_auth_connect(struct lgn *);
void lgn_auth_drop(struct lgn *);
int lgn_auth_queue(struct lgn *, uint32_t operation, uint64_t generation, const void *body, size_t bytes,
                   uint32_t purpose, uint32_t link, uint64_t attempt);
void lgn_auth_pump(struct lgn *);
void lgn_auth_poll(struct lgn *);   /* drain AUTH replies, then send the next request */
int lgn_link_live(struct lgn *, unsigned link);
unsigned lgn_wait_fds(const struct lgn *, int *fds, unsigned capacity);
int lgn_security(struct lgn *, uint32_t operation, uint32_t session, uint32_t flags,
                 const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes, struct ciuki_security_args *result);

/* coordinator.c: dispatch of complete requests and AUTH replies. */
void lgn_request(struct lgn *, unsigned link);
void lgn_auth_reply(struct lgn *, const struct lgn_auth_request *, int status, const void *body, size_t bytes);
void lgn_link_lost(struct lgn *, unsigned link, uint32_t role);
void lgn_auth_lost(struct lgn *);
void lgn_note(struct lgn *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
