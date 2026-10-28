/* SPDX-License-Identifier: MIT
 * LOGON_UI model and view (/System/Services/logon-ui), sec-user-03.
 *
 * The trusted logon screen: Welcome with the enabled accounts the account
 * service lists, a typed-name alternative, password entry, the locked screen
 * (owner password only), the Ctrl+Alt+Del security menu, waiting screens and
 * the "logon is not available" screen. It owns no authority: every request
 * goes to LOGON (service 5) or AUTH LIST (service 1) through the host seam,
 * which the native adapter binds to the kernel-installed links and the host
 * tests to a scripted LOGON. Secrets live only in struct lui and are wiped
 * after every submission, cancel and screen change; they are never painted
 * (masked), logged or copied elsewhere. English copy (owner rule).
 */
#ifndef CIUKI_LOGON_UI_H
#define CIUKI_LOGON_UI_H
#include "logon_proto.h"
#include "../libdeskui/deskui.h"
#include <ciuki/abi.h>

#define LUI_W 640
#define LUI_H 480
#define LUI_BACKGROUND 0x0a3d62u   /* Setup's identity colour */
#define LUI_BAND 0x1b2a5au
#define LUI_ACCOUNTS 18u
#define LUI_ITEMS 28u   /* sec-user-04: enrollment rows (16 -> 28) */
#define LUI_QUERY_MS 250u
#define LUI_HIDDEN_QUERY_MS 2000u

struct lui_host {
    void *ctx;
    uint64_t (*now_ms)(void *);
    /* One CSC1 request on the UI's link to service (LGN_SERVICE_LOGON or
     * LGN_SERVICE_AUTH): 0 with *status the frame status, or -errno. */
    int (*call)(void *, unsigned service, uint32_t operation, const void *body, size_t bytes,
                void *reply, size_t capacity, size_t *used, int *status);
    void (*log)(void *, const char *);
};
enum lui_view { LUI_VIEW_LIST, LUI_VIEW_PASSWORD, LUI_VIEW_TYPED, LUI_VIEW_CONFIRM_LOGOFF, LUI_VIEW_CONFIRM_POWER,
                LUI_VIEW_BLANK_LOCK, LUI_VIEW_MENU,
                /* sec-user-04 */ LUI_VIEW_OVERRIDE, LUI_VIEW_ENROLL_PLAN, LUI_VIEW_ENROLL_PASSWORD };
enum lui_action {
    LUI_NONE, LUI_TILE, LUI_FIELD_NAME, LUI_FIELD_PASSWORD, LUI_OK, LUI_BACK, LUI_TYPED, LUI_SHUTDOWN, LUI_RESTART,
    LUI_RETRY, LUI_LOCK, LUI_LOGOFF, LUI_POWER_MENU, LUI_TASKMGR, LUI_CANCEL, LUI_LOGOFF_CONFIRM, LUI_END_SESSION,
    LUI_SET_PASSWORD, LUI_DISMISS,
    /* sec-user-04: administrator log-off of a locked session, G32 enrollment */
    LUI_OVERRIDE, LUI_OVERRIDE_OK, LUI_FIELD_OLD, LUI_FIELD_CONFIRM, LUI_FIELD_LABEL, LUI_ROW, LUI_PLAN_OK, LUI_ENROLL_OK
};
struct lui_item { int action, index; struct du_rect box; const char *label; };
struct lui {
    const struct lui_host *host;
    struct lgn_state state;
    int state_valid, visible, dirty, view, focus, selected, busy;
    uint64_t next_query, retry_until, last_generation;
    struct { uint32_t uid; int blank_logon; char name[32]; } accounts[LUI_ACCOUNTS];
    unsigned count;
    int list_valid;
    char name[32], password[64], status[160];
    struct lui_item items[LUI_ITEMS];
    unsigned item_count;
    int mouse_x, mouse_y, layout;
    unsigned modifiers;
    const uint32_t *portrait;             /* 256x256 XRGB approved Ciuki portrait, or NULL */
    /* sec-user-04: second secrets of the enrollment form, the protected G32
     * source (names and ids only), the operator's labels and choice, and the
     * catalogue record being enrolled. Secrets are wiped like password. */
    char old_password[64], confirm[64];
    struct lgn_source source;
    int source_valid, first_admin, enroll_record;
    char labels[LGN_SOURCE_MAX][32];
};

void lui_init(struct lui *, const struct lui_host *, const uint32_t *portrait, int layout);
/* Periodic work: state queries and account list. now in ms. */
void lui_tick(struct lui *);
/* One input event from the trusted grant (MOTION already in design pixels). */
void lui_input(struct lui *, const struct ciuki_input_event *);
/* Paints the design canvas (LUI_W x LUI_H) when visible. */
void lui_paint(struct lui *, struct du_canvas *);
/* Forces a state query now (domain switch seen by the native loop). */
void lui_wake(struct lui *);
void lui_wipe(struct lui *);
#endif
