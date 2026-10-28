/* H10 (sec-user-03): LOGON_UI model and view (apps/logon/ui.c) against a
 * scripted LOGON. Every request body the UI sends is decoded with the
 * production codecs and checked field by field; frames of each screen are
 * written for review and hashed (goldens); masking is proven by rendering
 * two different passwords of equal length to identical pixels.
 * Usage: logon_ui <frames-dir>
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "../../../apps/desktop/theme.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

/* ---- scripted LOGON --------------------------------------------------- */
static uint64_t now;
static struct lgn_state script;
static unsigned calls[16];
static uint32_t last_command, last_event_op, last_event_action, last_session, last_console, event_session, event_console;
static uint8_t last_begin[64], last_submit[208];
static int begin_status, submit_disposition = LGN_AUTHENTICATED, submit_status, event_status, event_progress;
static uint32_t submit_retry;
static int accounts = 2;
static uint32_t challenge_uid = 1000;
/* sec-user-04: the scripted enrollment source, plan and provisional catalogue. */
static int enrolling, planned; static uint32_t staged; static uint8_t last_plan[32 + 40 * 16]; static size_t last_plan_bytes;

static uint64_t host_now(void *ctx) { (void)ctx; return now; }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int host_call(void *ctx, unsigned service, uint32_t op, const void *body, size_t n, void *reply, size_t cap,
                     size_t *used, int *status)
{
    (void)ctx; *used = 0; *status = 0;
    if (service == LGN_SERVICE_AUTH) {
        CHECK(op == LGN_AUTH_LIST && n == 0);
        calls[15]++;
        if (enrolling && planned) {   /* sec-user-04: provisional catalogue, nothing enabled */
            static const char *const draft[] = { "CiukiAdministrator", "Guest", "Dad", "Kid" };
            static const uint32_t uids[] = { 500, 501, 1000, 1001 };
            uint8_t *r = reply; size_t need = 16 + 144 * 4; CHECK(cap >= need);
            memset(r, 0, need); put32(r, 1); put32(r + 4, 4); put32(r + 8, 9);
            for (int i = 0; i < 4; i++) { put32(r + 16 + 144 * i, uids[i]); strcpy((char *)r + 16 + 144 * i + 16, draft[i]); }
            *used = need; return 0;
        }
        uint8_t *r = reply; size_t need = 16 + 144 * (size_t)accounts; CHECK(cap >= need);
        memset(r, 0, need); put32(r, 1); put32(r + 4, (uint32_t)accounts); put32(r + 8, 7);
        static const char *const names[] = { "CiukiUser", "Alice", "Bob" };
        for (int i = 0; i < accounts; i++) { put32(r + 16 + 144 * i, 1000 + (uint32_t)i); put32(r + 16 + 144 * i + 8, 1); strcpy((char *)r + 16 + 144 * i + 16, names[i]); }
        *used = need; return 0;
    }
    CHECK(service == LGN_SERVICE_LOGON && op >= 1 && op <= LGN_OP_ENROLL_SOURCE);   /* sec-user-04: 10 */
    if (op == LGN_OP_ENROLL_SOURCE) {
        calls[10]++; CHECK(!n);
        struct lgn_source src; memset(&src, 0, sizeof(src));
        src.count = 2; src.rows[0].old_id = 3; strcpy(src.rows[0].name, "Dad");
        src.rows[1].old_id = 5; src.rows[1].flags = LGN_SOURCE_REPLACE; strcpy(src.rows[1].name, "Guest");
        if (planned) { src.flags = LGN_SOURCE_PLAN; src.generation = 9; src.staged = staged; src.rows[0].uid = 1000; src.rows[1].uid = 1001; }
        CHECK(!lgn_source_encode(&src, reply, cap, used)); return 0;
    }
    if (op == LGN_OP_ENROLL_PLAN) {
        CHECK(n >= 72 && n <= sizeof(last_plan)); memcpy(last_plan, body, n); last_plan_bytes = n; planned = 1;
        uint8_t *r = reply; memset(r, 0, 16); put32(r, 1); *used = 16; return 0;
    }
    calls[op]++;
    const uint8_t *b = body;
    if (op == LGN_OP_UI_STATE) {
        struct lgn_ui_command c; CHECK(!lgn_ui_command_decode(&c, b, n));
        last_command = c.command; last_session = c.session; last_console = c.console;
        if (c.command == LGN_CMD_ACKNOWLEDGE) script.message = LGN_MSG_NONE;
        if (c.command == LGN_CMD_ATTENTION && script.mode == LGN_MODE_HIDDEN) script.mode = LGN_MODE_SECURITY;
        if (c.command == LGN_CMD_LOGOFF || c.command == LGN_CMD_TASKMGR || c.command == LGN_CMD_SHUTDOWN ||
            c.command == LGN_CMD_RESTART || c.command == LGN_CMD_SET_PASSWORD) script.mode = LGN_MODE_HIDDEN;
        if (c.command == LGN_CMD_FORCE_LOGOFF) script.mode = LGN_MODE_ENDING;
        script.generation++;
        CHECK(!lgn_state_encode(&script, reply, cap)); *used = LGN_UI_STATE_BYTES; return 0;
    }
    if (op == LGN_OP_BEGIN_AUTH) {
        CHECK(n == 64); memcpy(last_begin, b, 64);
        if (begin_status) { *status = begin_status; return 0; }
        uint8_t *c = reply; memset(c, 0, 64);
        put32(c, 1); put32(c + 4, r32(b + 4)); put32(c + 8, 77); put32(c + 12, 1); put32(c + 16, challenge_uid);
        put32(c + 20, r32(b + 8)); put32(c + 24, r32(b + 12) ? r32(b + 12) : 9); put32(c + 28, 3); put32(c + 32, 5);
        put32(c + 36, 1); put32(c + 40, 1); put32(c + 48, 7);
        *used = 64; return 0;
    }
    if (op == LGN_OP_SUBMIT_AUTH) {
        CHECK(n == 208); memcpy(last_submit, b, 208);
        struct lgn_progress p = { (uint32_t)submit_disposition, submit_status, submit_retry };
        CHECK(!lgn_progress_encode(&p, reply, cap)); *used = 16;
        if (submit_disposition == LGN_AUTHENTICATED)
            script.mode = script.mode == LGN_MODE_LOCKED ? (r32(last_begin + 4) == 2 ? LGN_MODE_ENDING : LGN_MODE_HIDDEN) : LGN_MODE_LOADING;
        return 0;
    }
    if (op == LGN_OP_LOCK_EVENT || op == LGN_OP_POWER_EVENT) {
        CHECK(n == 32 && r32(b) == 1);
        last_event_op = op; last_event_action = r32(b + 4); event_session = r32(b + 8); event_console = r32(b + 12);
        if (event_status) { *status = event_status; return 0; }
        struct lgn_progress p = { 0, event_progress, 0 };
        CHECK(!lgn_progress_encode(&p, reply, cap)); *used = 16;
        if (!event_progress && op == LGN_OP_LOCK_EVENT) script.mode = last_event_action == 1 ? LGN_MODE_LOCKED : LGN_MODE_HIDDEN;
        if (op == LGN_OP_POWER_EVENT) script.mode = LGN_MODE_POWER;
        return 0;
    }
    CHECK(0); return -EIO;
}
static const struct lui_host host = { NULL, host_now, host_call, NULL };

/* ---- helpers ------------------------------------------------------------ */
static uint32_t pixels[LUI_W * LUI_H];
static struct lui ui;
static const char *frames;
static void key(unsigned code) { struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = code, .value = 1 }; lui_input(&ui, &e);
                                 e.value = 0; lui_input(&ui, &e); }
static void type(const char *s)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm", digits[] = "1234567890";
    for (; *s; s++) {
        const char *p; unsigned code = 0; int shift = 0; char c = *s;
        if (c >= 'A' && c <= 'Z') { shift = 1; c = (char)(c + 32); }
        if ((p = strchr(row1, c))) code = 0x10 + (unsigned)(p - row1);
        else if ((p = strchr(row2, c))) code = 0x1e + (unsigned)(p - row2);
        else if ((p = strchr(row3, c))) code = 0x2c + (unsigned)(p - row3);
        else if ((p = strchr(digits, c))) code = 0x02 + (unsigned)(p - digits);
        else if (c == '-') code = 0x0c;
        else CHECK(0);
        if (shift) { struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = 0x2a, .value = 1 }; lui_input(&ui, &e); }
        key(code);
        if (shift) { struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = 0x2a, .value = 0 }; lui_input(&ui, &e); }
    }
}
static void click(int x, int y)
{
    struct ciuki_input_event m = { .type = CIUKI_INPUT_MOTION, .value = x, .value2 = y }; lui_input(&ui, &m);
    struct ciuki_input_event b = { .type = CIUKI_INPUT_BUTTON, .code = CIUKI_BUTTON_LEFT, .value = 1 }; lui_input(&ui, &b);
    b.value = 0; lui_input(&ui, &b);
}
static const struct lui_item *item(int action, int index)
{
    struct du_canvas c = { pixels, LUI_W, LUI_H, { 0, 0, LUI_W, LUI_H } }; lui_paint(&ui, &c); /* lays out */
    for (unsigned i = 0; i < ui.item_count; i++) if (ui.items[i].action == action && ui.items[i].index == index) return &ui.items[i];
    return NULL;
}
static void press(int action, int index) { const struct lui_item *it = item(action, index); CHECK(it); click(it->box.x + 2, it->box.y + 2); }
static void set_mode(uint32_t mode) { script.mode = mode; script.generation++; now += LUI_QUERY_MS; lui_wake(&ui); lui_tick(&ui); }
static uint64_t frame_hash(void)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < LUI_W * LUI_H; i++) { h ^= pixels[i] & 0xffffffu; h *= 1099511628211ull; }
    return h;
}
static char golden[4096]; static size_t golden_used;
static void frame(const char *name)
{
    struct du_canvas c = { pixels, LUI_W, LUI_H, { 0, 0, LUI_W, LUI_H } };
    lui_paint(&ui, &c);
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", frames, name);
    FILE *f = fopen(path, "wb"); CHECK(f);
    fprintf(f, "P6\n%d %d\n255\n", LUI_W, LUI_H);
    for (unsigned i = 0; i < LUI_W * LUI_H; i++) { uint8_t rgb[3] = { (uint8_t)(pixels[i] >> 16), (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i] }; fwrite(rgb, 1, 3, f); }
    fclose(f);
    golden_used += (size_t)snprintf(golden + golden_used, sizeof(golden) - golden_used, "%s %016llx\n", name,
                                    (unsigned long long)frame_hash());
}
static int zero(const void *p, size_t n) { const uint8_t *b = p; while (n--) if (*b++) return 0; return 1; }

int main(int argc, char **argv)
{
    CHECK(argc == 2); frames = argv[1];
    static uint32_t portrait[256 * 256];
    for (unsigned i = 0; i < 256 * 256; i++) portrait[i] = 0x00808080u | ((i * 7u) & 0xffu);
    script = (struct lgn_state){ .mode = LGN_MODE_STARTING, .console = 4, .generation = 1 };
    lui_init(&ui, &host, portrait, 0);
    lui_tick(&ui);
    CHECK(ui.state_valid && ui.visible && calls[LGN_OP_UI_STATE] == 1);
    frame("starting");

    /* Welcome: the account list, then a tile and its password. */
    set_mode(LGN_MODE_WELCOME);
    CHECK(ui.view == LUI_VIEW_LIST && ui.list_valid && ui.count == 2 && calls[15] == 1);
    frame("welcome");
    press(LUI_TILE, 0);
    CHECK(ui.view == LUI_VIEW_PASSWORD && ui.selected == 0);
    type("WrongPass1");
    CHECK(!strcmp(ui.password, "WrongPass1"));
    frame("welcome-password");
    uint32_t mask_a = (uint32_t)frame_hash();
    lgn_wipe(ui.password, 64); type("ZZZZZZZZZZ");
    struct du_canvas c = { pixels, LUI_W, LUI_H, { 0, 0, LUI_W, LUI_H } }; lui_paint(&ui, &c);
    CHECK((uint32_t)frame_hash() == mask_a);           /* masked: equal-length secrets paint identically */
    lgn_wipe(ui.password, 64); type("WrongPass1");
    submit_disposition = LGN_FAILED; submit_status = -EACCES; submit_retry = 1000;
    key(DU_KEY_ENTER);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == 1 && calls[LGN_OP_SUBMIT_AUTH] == 1);
    CHECK(r32(last_begin) == 1 && r32(last_begin + 4) == 2 && !r32(last_begin + 8) && !r32(last_begin + 12) &&
          r32(last_begin + 16) == 1 && r32(last_begin + 20) == 1000 && zero(last_begin + 24, 40));
    CHECK(r32(last_submit) == 1 && !r32(last_submit + 24) && r32(last_submit + 28) == 10 && !r32(last_submit + 32) &&
          r32(last_submit + 36) == 1000 && !memcmp(last_submit + 80, "WrongPass1", 10) && zero(last_submit + 90, 118) &&
          zero(last_submit + 48, 32));
    CHECK(zero(ui.password, sizeof(ui.password)) && strstr(ui.status, "incorrect"));
    frame("welcome-failed");
    type("x"); key(DU_KEY_ENTER);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == 1);              /* bounded retry: Enter ignored for 1 s */
    now += 1000;
    submit_disposition = LGN_AUTHENTICATED; submit_status = 0; submit_retry = 0;
    lgn_wipe(ui.password, 64); key(DU_KEY_ENTER);       /* empty password (ready CiukiUser) */
    CHECK(calls[LGN_OP_SUBMIT_AUTH] == 2 && !r32(last_submit + 28) && zero(last_submit + 80, 64));
    CHECK(ui.state.mode == LGN_MODE_LOADING);
    frame("loading");
    puts("H10 UI Welcome: list from AUTH LIST, tile -> password, BEGIN uid selector and SUBMIT bodies exact, masked secret, refusal text, bounded retry, empty password PASS");

    /* Esc never starts a session and wipes the attempt; typed names. */
    set_mode(LGN_MODE_WELCOME);
    press(LUI_TILE, 1); type("secret99"); key(DU_KEY_ESC);
    CHECK(ui.view == LUI_VIEW_LIST && zero(ui.password, 64) && calls[LGN_OP_BEGIN_AUTH] == 2);
    press(LUI_TYPED, 0);
    CHECK(ui.view == LUI_VIEW_TYPED);
    type("bad"); key(DU_KEY_BACKSPACE); key(DU_KEY_BACKSPACE); key(DU_KEY_BACKSPACE);
    key(DU_KEY_ENTER);                                   /* empty name: next field, no request */
    key(DU_KEY_ENTER);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == 2 && strstr(ui.status, "Type a user name"));
    ui.focus = 0; type("Guest"); ui.focus = 1; type("password1");
    begin_status = -EACCES; frame("welcome-typed");
    key(DU_KEY_ENTER);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == 3 && r32(last_begin + 16) == 2 && !r32(last_begin + 20) &&
          !strcmp((const char *)last_begin + 24, "Guest") && strstr(ui.status, "incorrect") && zero(ui.password, 64));
    begin_status = 0;
    press(LUI_BACK, 0); CHECK(ui.view == LUI_VIEW_LIST && zero(ui.name, 32));
    puts("H10 UI typed name: Esc/Cancel clears and returns without a request, name rule checked locally, refused Guest generic PASS");

    /* Locked: owner password only; blank owner can only end the session. */
    script.session = 3; script.console = 11; script.owner_uid = 1001; strcpy(script.owner_name, "Alice");
    set_mode(LGN_MODE_LOCKED);
    CHECK(ui.view == LUI_VIEW_PASSWORD);
    frame("locked");
    type("password"); key(DU_KEY_ENTER);
    CHECK(r32(last_begin + 4) == 3 && r32(last_begin + 8) == 3 && r32(last_begin + 12) == 11 && r32(last_begin + 16) == 1 &&
          r32(last_begin + 20) == 1001 && ui.state.mode == LGN_MODE_HIDDEN && !ui.visible);
    unsigned before = calls[LGN_OP_BEGIN_AUTH];
    type("ignored"); key(DU_KEY_ENTER);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == before && zero(ui.password, 64)); /* hidden: no field takes keys */
    script.flags = LGN_STATE_BLANK;
    set_mode(LGN_MODE_LOCKED);
    frame("locked-blank");
    CHECK(!item(LUI_FIELD_PASSWORD, 0));
    press(LUI_END_SESSION, 0);
    CHECK(last_command == LGN_CMD_FORCE_LOGOFF && last_session == 3 && last_console == 11 && ui.state.mode == LGN_MODE_ENDING);
    frame("ending");
    script.flags = 0;
    puts("H10 UI lock: unlock BEGIN binds session/console/owner uid, hidden screen takes no keys, passwordless owner gets End Session only PASS");

    /* Ctrl+Alt+Del: ATTENTION relay and the security menu. */
    set_mode(LGN_MODE_HIDDEN);
    struct ciuki_input_event att = { .type = CIUKI_INPUT_ATTENTION, .code = CIUKI_SECURITY_SECURE_ATTENTION };
    lui_input(&ui, &att);
    CHECK(last_command == LGN_CMD_ATTENTION && ui.state.mode == LGN_MODE_SECURITY && ui.view == LUI_VIEW_MENU && ui.visible);
    frame("security");
    event_progress = LGN_STATUS_NO_PASSWORD;
    press(LUI_LOCK, 0);
    CHECK(last_event_op == LGN_OP_LOCK_EVENT && last_event_action == LGN_EVENT_LOCK && event_session == 3 &&
          event_console == 11 && ui.view == LUI_VIEW_BLANK_LOCK);
    frame("security-blank-lock");
    press(LUI_SET_PASSWORD, 0);
    CHECK(last_command == LGN_CMD_SET_PASSWORD && ui.state.mode == LGN_MODE_HIDDEN);
    event_progress = 0;
    set_mode(LGN_MODE_SECURITY);
    press(LUI_LOGOFF, 0); CHECK(ui.view == LUI_VIEW_CONFIRM_LOGOFF);
    frame("security-logoff");
    key(DU_KEY_ESC); CHECK(ui.view == LUI_VIEW_MENU && ui.state.mode == LGN_MODE_SECURITY);
    press(LUI_LOGOFF, 0); press(LUI_LOGOFF_CONFIRM, 0);
    CHECK(last_command == LGN_CMD_LOGOFF && ui.state.mode == LGN_MODE_HIDDEN);
    set_mode(LGN_MODE_SECURITY);
    press(LUI_POWER_MENU, 0); CHECK(ui.view == LUI_VIEW_CONFIRM_POWER);
    frame("security-power");
    press(LUI_RESTART, 0); CHECK(last_command == LGN_CMD_RESTART);
    set_mode(LGN_MODE_SECURITY);
    press(LUI_TASKMGR, 0); CHECK(last_command == LGN_CMD_TASKMGR);
    set_mode(LGN_MODE_SECURITY);
    press(LUI_CANCEL, 0);
    CHECK(last_event_op == LGN_OP_LOCK_EVENT && last_event_action == LGN_EVENT_MENU_RETURN && ui.state.mode == LGN_MODE_HIDDEN);
    puts("H10 UI security menu: ATTENTION relayed, Lock guidance for a blank owner and Set Password, Log Off confirm/Esc, Shut Down choice, Task Manager, Cancel PASS");

    /* Logon screen power and the unavailable screen; one-shot messages. */
    script.session = 0; script.owner_uid = 0; memset(script.owner_name, 0, sizeof(script.owner_name)); script.console = 12;
    script.message = LGN_MSG_SESSION_ENDED;
    set_mode(LGN_MODE_WELCOME);
    CHECK(strstr(ui.status, "ended unexpectedly") && script.message == LGN_MSG_NONE && last_command == LGN_CMD_ACKNOWLEDGE);
    frame("welcome-message");
    press(LUI_POWER_MENU, 0); press(LUI_SHUTDOWN, 0);
    CHECK(last_event_op == LGN_OP_POWER_EVENT && last_event_action == LGN_EVENT_SHUTDOWN && !event_session && event_console == 12);
    frame("power");
    accounts = 0; set_mode(LGN_MODE_UNAVAILABLE);
    frame("unavailable");
    unsigned queries = calls[LGN_OP_UI_STATE];
    press(LUI_RETRY, 0); CHECK(calls[LGN_OP_UI_STATE] > queries);
    script.flags = LGN_STATE_SAFE; set_mode(LGN_MODE_WELCOME);
    CHECK(ui.list_valid && ui.count == 0);
    frame("welcome-safe-empty");
    puts("H10 UI power/unavailable: Turn Off Computer -> POWER_EVENT with the boot console, Retry re-queries, messages acknowledged once PASS");

    /* --- sec-user-04 block: an administrator's log-off of a locked session. --- */
    script.flags = 0; script.session = 3; script.console = 11; script.owner_uid = 1001; strcpy(script.owner_name, "Alice");
    accounts = 2; set_mode(LGN_MODE_LOCKED);
    press(LUI_OVERRIDE, 0);
    CHECK(ui.view == LUI_VIEW_OVERRIDE && item(LUI_FIELD_NAME, 0) && item(LUI_FIELD_PASSWORD, 0));
    frame("locked-override");
    key(DU_KEY_ESC);
    CHECK(ui.view == LUI_VIEW_PASSWORD && last_command == LGN_CMD_OVERRIDE_CANCEL);
    press(LUI_OVERRIDE, 0);
    ui.focus = 0; type("Bob"); key(DU_KEY_ENTER); type("bobpass12");
    unsigned begins = calls[LGN_OP_BEGIN_AUTH];
    key(DU_KEY_ENTER);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == begins + 1 && r32(last_begin + 4) == 2 && !r32(last_begin + 8) && !r32(last_begin + 12) &&
          r32(last_begin + 16) == 2 && !strcmp((const char *)last_begin + 24, "Bob"));
    CHECK(!memcmp(last_submit + 48, "Bob", 4) && !memcmp(last_submit + 80, "bobpass12", 9) && zero(ui.password, 64));
    CHECK(ui.state.mode == LGN_MODE_ENDING);
    puts("H10 UI administrator log-off (sec-user-04): locked-screen link, Esc cancels the armed request, OVERRIDE then a typed-name LOGON BEGIN, secrets wiped PASS");

    /* --- sec-user-04 block: G32 upgrade enrollment. --- */
    script.session = 0; script.owner_uid = 0; memset(script.owner_name, 0, sizeof(script.owner_name)); script.console = 13;
    enrolling = 1; set_mode(LGN_MODE_ENROLL);
    CHECK(calls[10] >= 1 && ui.source_valid && ui.view == LUI_VIEW_ENROLL_PLAN && ui.source.count == 2 && ui.first_admin < 0);
    CHECK(!strcmp(ui.labels[0], "Dad") && !ui.labels[1][0]);
    frame("enroll-plan");
    press(LUI_PLAN_OK, 0); CHECK(!planned && strstr(ui.status, "first administrator"));
    press(LUI_ROW, 1); CHECK(ui.first_admin == 1);
    press(LUI_PLAN_OK, 0); CHECK(!planned && strstr(ui.status, "needs a name"));
    press(LUI_FIELD_LABEL, 1); type("Kid");
    press(LUI_ROW, 0); CHECK(ui.first_admin == 0 && !strcmp(ui.labels[1], "Kid"));
    press(LUI_PLAN_OK, 0);
    CHECK(planned && last_plan_bytes == 32 + 80 && r32(last_plan) == 1 && r32(last_plan + 4) == 2 && r32(last_plan + 8) == 3 &&
          r32(last_plan + 32) == 3 && !strcmp((const char *)last_plan + 36, "Dad") && r32(last_plan + 72) == 5 &&
          !strcmp((const char *)last_plan + 76, "Kid") && zero(last_plan + 24, 8));
    CHECK(ui.view == LUI_VIEW_ENROLL_PASSWORD && ui.enroll_record == 0 && ui.accounts[0].uid == 500 && !item(LUI_FIELD_OLD, 0));
    frame("enroll-password");
    submit_disposition = LGN_ENROLL_STAGED; submit_status = 0;
    press(LUI_FIELD_PASSWORD, 0); type("recovery12"); press(LUI_FIELD_CONFIRM, 0); type("recovery13");
    begins = calls[LGN_OP_BEGIN_AUTH]; press(LUI_ENROLL_OK, 0);
    CHECK(calls[LGN_OP_BEGIN_AUTH] == begins && strstr(ui.status, "do not match") && zero(ui.password, 64) && zero(ui.confirm, 64));
    press(LUI_FIELD_PASSWORD, 0); type("recovery12"); press(LUI_FIELD_CONFIRM, 0); type("recovery12");
    challenge_uid = 500; staged = 1; press(LUI_ENROLL_OK, 0);
    CHECK(r32(last_begin + 4) == 4 && r32(last_begin + 16) == 1 && r32(last_begin + 20) == 500 && !r32(last_submit + 28) &&
          r32(last_submit + 32) == 10 && !memcmp(last_submit + 144, "recovery12", 10));
    CHECK(ui.enroll_record == 2 && item(LUI_FIELD_OLD, 0) && zero(ui.password, 64));
    frame("enroll-imported");
    press(LUI_FIELD_OLD, 0); type("dadpass"); press(LUI_FIELD_PASSWORD, 0); type("dadpass12"); press(LUI_FIELD_CONFIRM, 0); type("dadpass12");
    challenge_uid = 1000; staged = 5; press(LUI_ENROLL_OK, 0);
    CHECK(r32(last_begin + 20) == 1000 && r32(last_submit + 28) == 7 && !memcmp(last_submit + 80, "dadpass", 7) &&
          !memcmp(last_submit + 144, "dadpass12", 9) && ui.enroll_record == 3 && zero(ui.old_password, 64));
    submit_disposition = LGN_ENROLL_COMMITTED; challenge_uid = 1001;
    press(LUI_FIELD_OLD, 0); press(LUI_FIELD_PASSWORD, 0); type("kidpass12"); press(LUI_FIELD_CONFIRM, 0); type("kidpass12");
    press(LUI_ENROLL_OK, 0);
    CHECK(r32(last_begin + 20) == 1001 && !r32(last_submit + 28) && strstr(ui.status, "converted"));
    submit_disposition = LGN_AUTHENTICATED; challenge_uid = 1000; enrolling = 0; planned = 0;
    puts("H10 UI G32 enrollment (sec-user-04): ENROLL_SOURCE rows, first administrator and replacement label required, exact "
         "ENROLL_PLAN rows, recovery/imported/fresh password forms, local confirmation, ENROLL BEGIN/SUBMIT bodies, staged then committed PASS");

    lui_wipe(&ui);
    CHECK(zero(ui.password, 64) && zero(ui.name, 32));
    char path[512]; snprintf(path, sizeof(path), "%s/goldens.txt", frames);
    FILE *g = fopen(path, "w"); CHECK(g); fputs(golden, g); fclose(g);
    printf("H10 logon UI: PASS checks=%u\n", checks);
    return 0;
}
