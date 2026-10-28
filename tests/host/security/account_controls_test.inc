/* H12 (sec-user-04): User Accounts, saver/resume lock, administrator log-off
 * of a locked session and G32 upgrade enrollment, on the production LOGON +
 * ACCOUNT + kernel host harness of logon_service_test.c (included there with
 * -DCIUKI_H12_ACCOUNTS). The Control Panel's production client
 * (apps/control/accounts_client.c) talks to the production account service
 * over a call-101 ACCOUNTS link owned by a session process; a raw encoder
 * bypasses the client's own checks to prove the service decides when a change
 * is applied. SPDX-License-Identifier: GPL-2.0-only */
#include "accounts_client.h"
#include "firstboot.h"

/* ---- the Control Panel client, bound to a session process ---------------- */
static struct lgn_client cp_link;
static struct bind *cp_bind = &desk_bind;
static int cp_call(void *ctx, uint32_t op, uint64_t generation, const void *body, size_t n, void *reply, size_t cap,
                   size_t *used, int *status)
{ (void)ctx; return lgn_client_call(&cp_link, op, generation, body, n, reply, cap, used, status, 20000); }
static int cp_self(void *ctx, uint32_t *uid, uint32_t groups[8], unsigned *count)
{
    (void)ctx;
    struct ciuki_security_args a = { .version = 1, .size = 64, .operation = CIUKI_SECURITY_QUERY_SELF };
    struct ciuki_credential c; memset(&c, 0, sizeof(c));
    int e = b_security(cp_bind, &a, NULL, 0, &c, sizeof(c));
    if (e) return e;
    *uid = c.uid; *count = c.group_count > 8 ? 8 : c.group_count;
    for (unsigned i = 0; i < *count; i++) groups[i] = c.groups[i];
    return 0;
}
static const struct cpa_transport cp_transport = { NULL, cp_call, cp_self, NULL };
static void cp_connect(void)
{
    if (cp_link.fd >= 0 && cp_link.sys) lgn_client_close(&cp_link);
    lgn_client_init(&cp_link, &desk_sys, CIUKI_SECURITY_SERVICE_ACCOUNTS, CIUKI_SECURITY_ROLE_ACCOUNT);
}
static struct cpa_view view;
static int cp_do(unsigned action, uint32_t uid, const char *name, const char *old, const char *password, const char *confirm,
                 int admin, int enable)
{
    struct cpa_request q; memset(&q, 0, sizeof(q));
    q.action = action; q.uid = uid; q.admin = admin; q.enable = enable;
    if (name) snprintf(q.name, sizeof(q.name), "%s", name);
    if (old) snprintf(q.old_password, sizeof(q.old_password), "%s", old);
    if (password) snprintf(q.password, sizeof(q.password), "%s", password);
    snprintf(q.confirm, sizeof(q.confirm), "%s", confirm ? confirm : password ? password : "");
    int e = cpa_execute(&cp_transport, &q, &view);
    for (size_t i = 0; i < sizeof(q.password); i++) CHECK(!q.password[i] && !q.old_password[i] && !q.confirm[i]);
    return e;
}
static const struct cpa_record *named(const char *name)
{
    for (unsigned i = 0; i < view.count; i++) if (!strcmp(view.records[i].name, name)) return &view.records[i];
    return NULL;
}
/* RESERVE + management with the service's wire layout, no client checks. */
static int cp_raw(uint32_t wire, uint32_t flags, uint32_t uid, const char *name, const char *old, const char *password,
                  uint64_t generation)
{
    uint8_t body[312], digest[32], reserve[48], reply[16]; size_t used = 0; int status = 0;
    CHECK(!cpa_management_encode(wire, flags, uid, generation, name ? name : "", old ? old : "", password ? password : "", "",
                                 0, body, digest));
    memset(reserve, 0, sizeof(reserve)); w32(reserve, 1); w32(reserve + 4, wire); memcpy(reserve + 8, digest, 32);
    CHECK(!cp_call(NULL, 9, 0, reserve, sizeof(reserve), reply, sizeof(reply), &used, &status));
    if (status) return status;
    CHECK(used == 16); w64(body + 304, a64(reply));
    CHECK(!cp_call(NULL, wire, generation, body, sizeof(body), reply, sizeof(reply), &used, &status));
    return status;
}
static void logon_as(const char *name, const char *secret)
{
    welcome();
    struct lgn_progress p = authenticate(2, 0, name, secret);
    if (p.disposition != LGN_AUTHENTICATED) printf("  logon %s: disposition=%u status=%d\n", name, p.disposition, p.status);
    CHECK(p.disposition == LGN_AUTHENTICATED && !p.status);
    active();
    cp_connect();
}
static void log_off(void)
{
    CHECK(desk_event(LGN_NOTICE_LOGOFF).action == LGN_ACT_NONE);
    lgn_client_close(&cp_link);
    desktop_exit();
    welcome();
    g_ticks += 1000;
}

/* ---- User Accounts (contract 7.1) ---------------------------------------- */
static void scenario_accounts(void)
{
    boot(0);
    logon_as("Alice", "password");
    CHECK(!cp_do(CPA_READ, 0, NULL, NULL, NULL, NULL, 0, 0));
    CHECK(view.valid && view.self_uid == 1000 && view.self_admin && !view.self_guest && view.count == 3);
    CHECK(named("CiukiAdministrator") && !(named("CiukiAdministrator")->flags & CPA_ENABLED) && named("Guest"));
    CHECK(cpa_allowed(&view, CPA_CREATE, NULL) && !cpa_allowed(&view, CPA_DELETE, cpa_find(&view, 1000)) &&
          !cpa_allowed(&view, CPA_RENAME, cpa_find(&view, 501)) && cpa_allowed(&view, CPA_PASSWORD, cpa_find(&view, 501)));
    uint64_t g0 = view.generation;
    CHECK(!cp_do(CPA_CREATE, 0, "Bob", NULL, "bobpass12", NULL, 0, 1));
    const struct cpa_record *bob = named("Bob");
    CHECK(bob && bob->uid == 1001 && bob->groups == ACCT_STANDARD && (bob->flags & CPA_ENABLED) && view.generation > g0);
    CHECK(cp_do(CPA_CREATE, 0, "bob", NULL, "bobpass12", NULL, 0, 1) == -EINVAL);          /* duplicate (case-insensitive) */
    CHECK(cp_do(CPA_CREATE, 0, "Short", NULL, "short", NULL, 0, 1) == -EINVAL);            /* 8..63 */
    CHECK(cp_do(CPA_CREATE, 0, "Typo", NULL, "typopass1", "typopass2", 0, 1) == -EINVAL);  /* confirmation */
    CHECK(cp_do(CPA_CREATE, 0, "Guest", NULL, "guestpass1", NULL, 0, 1) == -EINVAL);       /* reserved */
    CHECK(!cp_do(CPA_RENAME, 1001, "Robert", NULL, NULL, NULL, 0, 0) && named("Robert") && !named("Bob"));
    CHECK(!cp_do(CPA_TYPE, 1001, NULL, NULL, NULL, NULL, 1, 0) && (named("Robert")->flags & CPA_ADMIN));
    CHECK(!cp_do(CPA_TYPE, 1001, NULL, NULL, NULL, NULL, 0, 0) && !(named("Robert")->flags & CPA_ADMIN));
    /* The logged-on account: the client offers nothing destructive and the
     * service refuses it anyway when applied (active account, EBUSY). */
    CHECK(cp_do(CPA_TYPE, 1000, NULL, NULL, NULL, NULL, 0, 0) == -EPERM);
    CHECK(cp_do(CPA_DELETE, 1000, NULL, NULL, NULL, NULL, 0, 0) == -EPERM);
    CHECK(cp_raw(4, 512, 1000, NULL, NULL, NULL, view.generation) == -EBUSY);
    CHECK(cp_raw(5, 0, 1000, NULL, NULL, NULL, view.generation) == -EBUSY);
    CHECK(cp_raw(4, 256, 1000, NULL, NULL, NULL, view.generation) == -EBUSY);
    /* A stale snapshot is refused when applied (A2-4): the list changed. */
    CHECK(cp_raw(4, 1024, 1001, "Rob", NULL, NULL, g0) == -EAGAIN && named("Robert"));
    /* Built-ins keep name and type; Guest needs a password before it is on. */
    CHECK(cp_raw(4, 1024, 501, "Visitor", NULL, NULL, view.generation) == -EINVAL);
    CHECK(cp_do(CPA_ENABLE, 501, NULL, NULL, NULL, NULL, 0, 1) == -EINVAL);
    CHECK(!cp_do(CPA_PASSWORD, 501, NULL, NULL, "guestpass1", NULL, 0, 0));
    CHECK(!cp_do(CPA_ENABLE, 501, NULL, NULL, NULL, NULL, 0, 1) && (named("Guest")->flags & CPA_ENABLED));
    /* Own password: the old one first (Windows XP), then the new one. */
    CHECK(cp_do(CPA_PASSWORD, 1000, NULL, "wrongpass", "alicepass2", NULL, 0, 0) == -EACCES);
    g_ticks += 1000;
    CHECK(!cp_do(CPA_PASSWORD, 1000, NULL, "password", "alicepass2", NULL, 0, 0));
    CHECK(!(desk_event(LGN_NOTICE_PASSWORD).flags & LGN_EVENT_BLANK));
    CHECK(!cp_do(CPA_CREATE, 0, "Carol", NULL, "carolpass1", NULL, 0, 1));
    CHECK(!cp_do(CPA_DELETE, 1001, NULL, NULL, NULL, NULL, 0, 0) && !named("Robert"));
    puts("H12 administrator: create Standard/administrator, duplicate/reserved/short/mismatched refused, rename, type both ways, "
         "own account protected by the client and EBUSY from the service, stale generation EAGAIN, built-in name kept, "
         "Guest password-first enable, own password needs the old one, delete inactive PASS");
    log_off();

    /* A Standard user: own account only, own password and picture. */
    logon_as("Carol", "carolpass1");
    CHECK(!cp_do(CPA_READ, 0, NULL, NULL, NULL, NULL, 0, 0));
    CHECK(!view.self_admin && view.self_uid == 1002 && !named("CiukiAdministrator"));   /* disabled accounts hidden */
    CHECK(!cpa_allowed(&view, CPA_CREATE, NULL) && !cpa_allowed(&view, CPA_TYPE, cpa_find(&view, 1002)) &&
          !cpa_allowed(&view, CPA_ENABLE, cpa_find(&view, 1000)) && cpa_allowed(&view, CPA_PASSWORD, cpa_find(&view, 1002)) &&
          !cpa_allowed(&view, CPA_PASSWORD, cpa_find(&view, 1000)));
    CHECK(cp_do(CPA_CREATE, 0, "Mallory", NULL, "mallory12", NULL, 1, 1) == -EPERM);
    /* Bypassing the page: the service decides at apply time. */
    CHECK(cp_raw(3, 3, 0, "Mallory", NULL, "mallory12", view.generation) == -EPERM);   /* RESERVE: administrators only */
    CHECK(cp_raw(4, 2048, 1000, NULL, NULL, "takeover12", view.generation) == -EPERM);
    CHECK(cp_raw(4, 256 | 1, 501, NULL, NULL, NULL, view.generation) == -EPERM);
    CHECK(cp_raw(4, 512 | 2, 1002, NULL, NULL, NULL, view.generation) == -EPERM);
    CHECK(cp_do(CPA_PASSWORD, 1002, NULL, "wrongpass", "carolpass2", NULL, 0, 0) == -EACCES);
    g_ticks += 1000;
    CHECK(!cp_do(CPA_PASSWORD, 1002, NULL, "carolpass1", "carolpass2", NULL, 0, 0));
    CHECK(!cp_do(CPA_PICTURE, 1002, NULL, NULL, NULL, NULL, 0, 0));
    puts("H12 Standard user: own account only, administrator tasks disabled, direct create/reset/enable/promote refused "
         "by the service, own password with the old one, own picture PASS");
    log_off();

    /* Guest: its own password with the old one, nothing else. */
    logon_as("Guest", "guestpass1");
    CHECK(!cp_do(CPA_READ, 0, NULL, NULL, NULL, NULL, 0, 0));
    CHECK(view.self_guest && !view.self_admin && view.self_uid == 501);
    CHECK(cpa_allowed(&view, CPA_PASSWORD, cpa_find(&view, 501)) && !cpa_allowed(&view, CPA_RENAME, cpa_find(&view, 501)) &&
          !cpa_allowed(&view, CPA_ENABLE, cpa_find(&view, 501)));
    CHECK(cp_do(CPA_PASSWORD, 501, NULL, "", "guestpass2", NULL, 0, 0) == -EACCES);
    g_ticks += 1000;
    CHECK(!cp_do(CPA_PASSWORD, 501, NULL, "guestpass1", "guestpass2", NULL, 0, 0));
    CHECK(cp_raw(4, 1024, 501, "Visitor", NULL, NULL, view.generation) == -EPERM);    /* sec-k08 (U4-5): no own rename */
    CHECK(cp_raw(5, 0, 1002, NULL, NULL, NULL, view.generation) == -EPERM);
    CHECK(cp_raw(4, 512 | 2, 501, NULL, NULL, NULL, view.generation) == -EPERM);
    puts("H12 Guest: own password only with the old password, rename/delete/type refused PASS");
}

/* ---- last administrator (contract 3.1/7.1), from the Safe Mode recovery
 * account: the only enabled administrator with a password cannot be turned
 * off or demoted; once another exists, it can. ---------------------------- */
static void scenario_last_admin(void)
{
    boot_flags_value = CIUKI_BOOT_SAFE_MODE; g_boot.flags |= CBI_F_SAFE_MODE;
    boot(0);
    logon_as("CiukiAdministrator", "password");
    CHECK(desktop_credential()->uid == 500 && (desktop_credential()->flags & CIUKI_CRED_SAFE_RECOVERY));
    CHECK(!cp_do(CPA_READ, 0, NULL, NULL, NULL, NULL, 0, 0) && view.self_admin && view.self_uid == 500);
    CHECK(cp_do(CPA_ENABLE, 1000, NULL, NULL, NULL, NULL, 0, 0) == -EPERM && (named("Alice")->flags & CPA_ENABLED));
    CHECK(cp_do(CPA_TYPE, 1000, NULL, NULL, NULL, NULL, 0, 0) == -EPERM && (named("Alice")->flags & CPA_ADMIN));
    CHECK(cp_do(CPA_DELETE, 1000, NULL, NULL, NULL, NULL, 0, 0) == -EPERM && named("Alice"));
    CHECK(!cp_do(CPA_CREATE, 0, "Bob", NULL, "bobpass12", NULL, 1, 1));
    CHECK(!cp_do(CPA_TYPE, 1000, NULL, NULL, NULL, NULL, 0, 0) && !(named("Alice")->flags & CPA_ADMIN));
    CHECK(cp_do(CPA_ENABLE, 1001, NULL, NULL, NULL, NULL, 0, 0) == -EPERM);   /* Bob is now the last one */
    puts("H12 last administrator: the only enabled administrator cannot be turned off, demoted or deleted (EPERM, "
         "applied by the service); a second administrator lifts the rule PASS");
}

/* ---- saver and resume lock (contract 4.3) -------------------------------- */
static void scenario_saver(void)
{
    boot(0);
    logon_as("Alice", "password");
    uint32_t session = logon.session.id;
    struct lgn_session_event ev = desk_event(LGN_NOTICE_SAVER);
    CHECK(gate_trusted() && logon.mode == LGN_MODE_LOCKED && !(ev.flags & LGN_EVENT_BLANK));
    RUN_UNTIL(desk_event(LGN_NOTICE_QUERY).flags & LGN_EVENT_LOCKED, 400);
    CHECK(logon.session.console.reason == CIUKI_SECURITY_SECURE_SAVER && logon.session.console.authentication_required);
    struct lgn_state st = attention(); CHECK(st.mode == LGN_MODE_LOCKED);
    struct lgn_progress p = authenticate(3, 1000, NULL, "wrongpassword");
    CHECK(p.disposition == LGN_FAILED && gate_trusted());
    g_ticks += 1000;
    p = authenticate(3, 1000, NULL, "password");
    CHECK(p.disposition == LGN_AUTHENTICATED && !gate_trusted() && logon.session.id == session);
    puts("H12 saver: a protected saver's dismissal enters the trusted gate (SECURE_ENTER SAVER) before the desktop "
         "returns; attention and a wrong password keep it; the owner's password restores the same session PASS");
    ev = desk_event(LGN_NOTICE_STANDBY);
    CHECK(gate_trusted() && logon.mode == LGN_MODE_LOCKED);
    RUN_UNTIL(desk_event(LGN_NOTICE_QUERY).flags & LGN_EVENT_LOCKED, 400);
    CHECK(logon.session.console.reason == CIUKI_SECURITY_SECURE_RESUME);
    CHECK(desk_event(LGN_NOTICE_STANDBY).flags & LGN_EVENT_LOCKED);   /* the post-resume notice is idempotent */
    p = authenticate(3, 1000, NULL, "password");
    CHECK(p.disposition == LGN_AUTHENTICATED && !gate_trusted());
    puts("H12 resume: Stand by arms SECURE_ENTER(RESUME) and reports LOCKED before sleeping; the resumed session stays "
         "locked until the owner's password PASS");
    /* sec-k08 (U4-3): a firmware-initiated suspend (APM Critical Suspend)
     * the desktop never armed: the kernel conceals the session through PID 1
     * and LOGON locks it; a plain Ctrl+Alt+Delete still shows the menu. */
    fw_resumes++; security_attention_resume(); pid1();
    RUN_UNTIL(logon.mode == LGN_MODE_LOCKED && !logon.enter_reason, 400);
    CHECK(gate_trusted() && logon.session.console.reason == CIUKI_SECURITY_SECURE_RESUME &&
          logon.session.console.authentication_required && logon.session.id == session);
    p = authenticate(3, 1000, NULL, "password");
    CHECK(p.disposition == LGN_AUTHENTICATED && !gate_trusted() && logon.mode == LGN_MODE_HIDDEN);
    g_ticks += 5000; st = attention(); CHECK(st.mode == LGN_MODE_SECURITY);
    CHECK(!ui_event(LGN_OP_LOCK_EVENT, LGN_EVENT_MENU_RETURN, st.session, st.console, &p) && !gate_trusted());
    puts("H12 firmware resume (sec-k08): a suspend the desktop did not arm is concealed by the kernel and locked by LOGON "
         "until the owner's password; a later Ctrl+Alt+Delete shows the security menu PASS");
}
static void scenario_saver_blank(void)
{
    boot(1);
    active(); cp_connect();
    struct lgn_session_event ev = desk_event(LGN_NOTICE_SAVER);
    CHECK((ev.flags & LGN_EVENT_BLANK) && !(ev.flags & LGN_EVENT_LOCKED) && !gate_trusted() && logon.mode == LGN_MODE_HIDDEN);
    ev = desk_event(LGN_NOTICE_STANDBY);
    CHECK((ev.flags & LGN_EVENT_BLANK) && !gate_trusted());
    struct lgn_progress p;
    CHECK(!desk_lock(&p) && p.status == LGN_STATUS_NO_PASSWORD && !gate_trusted());
    puts("H12 blank owner: saver dismissal and Stand by keep no false lock (BLANK), Lock gives the set-password guidance PASS");
    /* sec-k08 (U4-3): a firmware resume conceals, then restores a passwordless owner. */
    fw_resumes++; security_attention_resume(); pid1();
    RUN_UNTIL(logon.mode == LGN_MODE_HIDDEN && !gate_trusted() && !logon.resume_until, 400);
    CHECK(logon.session.blank && grants_display_pid() == logon.session.launch[0].pid);
    puts("H12 firmware resume, blank owner (sec-k08): concealed by the kernel, restored by LOGON without a false lock PASS");
    /* User Accounts (Passwords alias): CiukiUser sets a password with the
     * empty old one; the desktop tells LOGON (notice PASSWORD). */
    CHECK(!cp_do(CPA_READ, 0, NULL, NULL, NULL, NULL, 0, 0) && view.self_admin && view.self_uid == 1000);
    CHECK(!cp_do(CPA_PASSWORD, 1000, NULL, "", "ciukipass1", NULL, 0, 0));
    CHECK(logon.session.blank);
    ev = desk_event(LGN_NOTICE_PASSWORD);
    CHECK(!(ev.flags & LGN_EVENT_BLANK) && !logon.session.blank);
    CHECK(!desk_lock(&p) && !p.status && gate_trusted() && logon.mode == LGN_MODE_LOCKED);
    RUN_UNTIL(!logon.enter_reason, 100);
    p = authenticate(3, 1000, NULL, "ciukipass1");
    CHECK(p.disposition == LGN_AUTHENTICATED && !gate_trusted());
    ev = desk_event(LGN_NOTICE_SAVER);
    CHECK(gate_trusted() && !(ev.flags & LGN_EVENT_BLANK));
    puts("H12 password set: User Accounts sets CiukiUser's first password, the PASSWORD notice clears the guidance, "
         "Lock and the saver now lock and the new password unlocks PASS");
}

/* ---- administrator log-off of a locked session (contract 4.3) ----------- */
static void scenario_override(void)
{
    boot(0);
    logon_as("Alice", "password");
    CHECK(!cp_do(CPA_CREATE, 0, "Bob", NULL, "bobpass12", NULL, 1, 1));
    CHECK(!cp_do(CPA_CREATE, 0, "Carol", NULL, "carolpass1", NULL, 0, 1));
    struct lgn_progress p;
    CHECK(!desk_lock(&p) && !p.status);
    RUN_UNTIL(logon.mode == LGN_MODE_LOCKED && !logon.enter_reason, 400);
    uint32_t session = logon.session.id;
    uint8_t challenge[64];
    CHECK(ui_begin(2, 0, "Bob", 0, 0, challenge) == -EBUSY);   /* not armed: no logon while a session is locked */
    struct lgn_state st = state();
    int status = 0;
    (void)ui_state(LGN_CMD_OVERRIDE, st.session, st.console + 1, &status); CHECK(status == -EAGAIN);
    st = ui_state(LGN_CMD_OVERRIDE, st.session, st.console, NULL);
    CHECK(st.mode == LGN_MODE_LOCKED && (st.flags & LGN_STATE_OVERRIDE));
    p = authenticate(2, 0, "Carol", "carolpass1");
    CHECK(p.disposition == LGN_FAILED && p.status == -EACCES && logon.session.id == session && gate_trusted());
    st = state(); CHECK(st.message == LGN_MSG_NOT_ADMIN && !(st.flags & LGN_STATE_OVERRIDE));
    (void)ui_state(LGN_CMD_ACKNOWLEDGE, 0, 0, NULL);
    st = ui_state(LGN_CMD_OVERRIDE, st.session, st.console, NULL);
    p = authenticate(2, 0, "Bob", "wrongpass1");
    CHECK(p.disposition == LGN_FAILED && p.status == -EACCES && logon.session.id == session && logon.mode == LGN_MODE_LOCKED);
    g_ticks += 1000;
    st = attention();   /* attention disarms it */
    CHECK(st.mode == LGN_MODE_LOCKED && !(st.flags & LGN_STATE_OVERRIDE));
    CHECK(ui_begin(2, 0, "Bob", 0, 0, challenge) == -EBUSY);
    st = ui_state(LGN_CMD_OVERRIDE, st.session, st.console, NULL);
    st = ui_state(LGN_CMD_OVERRIDE_CANCEL, 0, 0, NULL);
    CHECK(!(st.flags & LGN_STATE_OVERRIDE) && ui_begin(2, 0, "Bob", 0, 0, challenge) == -EBUSY);
    st = ui_state(LGN_CMD_OVERRIDE, st.session, st.console, NULL);
    lgn_client_close(&cp_link);
    p = authenticate(2, 0, "Bob", "bobpass12");
    CHECK(p.disposition == LGN_AUTHENTICATED && !p.status);
    CHECK(logon.session.phase == LGN_ENDING && logon.session.end == LGN_END_FORCED);
    desk_logon.fd = -1; lgn_client_close(&desk_logon); desk_bind.t = NULL;   /* the kernel ends the desktop */
    welcome();
    st = state(); CHECK(st.message == LGN_MSG_ADMIN_ENDED && !(st.flags & LGN_STATE_SESSION));
    (void)ui_state(LGN_CMD_ACKNOWLEDGE, 0, 0, NULL);
    for (unsigned i = 0; i < 100; i++) world();
    CHECK(!logon.session.phase);   /* no session was created by the override */
    g_ticks += 1000;
    logon_as("Bob", "bobpass12");
    CHECK(desktop_credential()->uid == 1001 && desktop_credential()->groups[0] == 544 && logon.session.id != session);
    puts("H12 administrator log-off: only while armed on the locked console, a Standard user and a wrong password "
         "leave the session locked, attention and cancel disarm it, an administrator's password ends the locked "
         "session (forced) without creating one, then logs on afresh PASS");
}

/* ---- G32 upgrade enrollment (contract amendment, sec-user-04) ----------- */
static uint8_t g32_bytes[1400];
static size_t g32_size;
static int g32_present = 1;
static int legacy_read(void *ctx, const char *name, void *out, size_t cap, size_t *n)
{
    (void)ctx;
    if (strcmp(name, "users.dat") || !g32_present) return -ENOENT;
    if (cap < g32_size) return -ENOSPC;
    memcpy(out, g32_bytes, g32_size); *n = g32_size; return 0;
}
static int legacy_remove(void *ctx, const char *name) { (void)ctx; if (!strcmp(name, "users.dat")) g32_present = 0; return 0; }
static int legacy_barrier(void *ctx) { (void)ctx; return 0; }
static const struct acct_legacy_io legacy_io = { NULL, legacy_read, legacy_remove, legacy_barrier };
static int source_query(struct lgn_source *src)
{
    uint8_t reply[LGN_SOURCE_BYTES(LGN_SOURCE_MAX)]; size_t used = 0;
    int s = call(&ui_logon, LGN_OP_ENROLL_SOURCE, 0, NULL, 0, reply, sizeof(reply), &used);
    if (!s) CHECK(!lgn_source_decode(src, reply, used));
    return s;
}
static struct lgn_progress enroll(uint32_t uid, const char *old, const char *secret)
{
    uint8_t challenge[64], body[208], reply[16]; size_t used = 0; struct lgn_progress p = { 0, 0, 0 };
    int s = ui_begin(4, uid, NULL, 0, 0, challenge);
    if (s) { p.disposition = LGN_FAILED; p.status = s; return p; }
    CHECK(a32(challenge + 16) == uid);
    CHECK(!lgn_submit_encode(challenge, "", old, secret, body));
    s = call(&ui_logon, LGN_OP_SUBMIT_AUTH, 0, body, 208, reply, 16, &used);
    lgn_wipe(body, sizeof(body));
    if (s) { p.disposition = LGN_FAILED; p.status = s; return p; }
    CHECK(used == 16 && !lgn_progress_decode(&p, reply, 16));
    return p;
}
static void scenario_upgrade(void)
{
    struct users_db old; memset(&old, 0, sizeof(old));
    old.count = 2; old.next_id = 9;
    old.records[0].id = 5; strcpy(old.records[0].name, "Guest");   /* a reserved name: replacement label required */
    old.records[1].id = 3; strcpy(old.records[1].name, "Dad");
    users_derive("", old.records[0].salt, 10000, old.records[0].verifier);
    users_derive("dadpass", old.records[1].salt, 10000, old.records[1].verifier);
    g32_size = users_encode(&old, g32_bytes, sizeof(g32_bytes)); CHECK(g32_size);
    struct acct_store store = fake_store(&fake);
    CHECK(!acct_server_init(&acct_srv, &store, 1));
    acct_srv.legacy = &legacy_io;
    account_thread = role_fixture(CIUKI_SECURITY_ROLE_ACCOUNT, &account_p);
    logon_start();
    ui_start();
    RUN_UNTIL(logon.mode == LGN_MODE_ENROLL, 4000);
    CHECK(!g32_present && logon.enrolling && !logon.session.phase && gate_trusted());
    struct lgn_state st = state(); CHECK(st.mode == LGN_MODE_ENROLL);
    for (unsigned i = 0; i < 200; i++) world();
    CHECK(logon.mode == LGN_MODE_ENROLL && !logon.session.phase);   /* never an automatic logon */
    struct lgn_source src;
    CHECK(!source_query(&src));
    CHECK(src.count == 2 && !(src.flags & LGN_SOURCE_PLAN) && src.rows[0].old_id == 3 && !strcmp(src.rows[0].name, "Dad") &&
          !src.rows[0].flags && src.rows[1].old_id == 5 && (src.rows[1].flags & LGN_SOURCE_REPLACE) && !src.rows[1].uid);
    uint8_t challenge[64];
    CHECK(ui_begin(4, 500, NULL, 0, 0, challenge) == -EACCES);   /* no plan: no ENROLL challenge */
    g_ticks += 1000;
    /* The plan: every protected row exactly, Dad first administrator, Guest relabelled. */
    uint8_t plan[32 + 40 * 2]; memset(plan, 0, sizeof(plan));
    w32(plan, 1); w32(plan + 4, 2); w32(plan + 8, 3);
    w32(plan + 32, 3); strcpy((char *)plan + 36, "Dad");
    w32(plan + 72, 5); strcpy((char *)plan + 76, "Guest");
    uint8_t reply[16 + 144 * ACCT_MAX]; size_t used = 0;
    CHECK(call(&ui_logon, LGN_OP_ENROLL_PLAN, 0, plan, sizeof(plan), reply, sizeof(reply), &used) == -EINVAL);
    strcpy((char *)plan + 76, "Kid"); memset(plan + 79, 0, 2);
    CHECK(!call(&ui_logon, LGN_OP_ENROLL_PLAN, 0, plan, sizeof(plan), reply, sizeof(reply), &used));
    CHECK(used == 16 + 144 * 4 && a32(reply + 4) == 4);
    const struct acct_enrollment *en = &acct_srv.service.enrollment;
    fake.enrollment_count = en->map_count;
    for (unsigned i = 0; i < en->map_count; i++) {
        fake.enrollment[i].legacy_id = en->map[i].old_id; fake.enrollment[i].uid = en->map[i].uid;
        memcpy(fake.enrollment[i].root, en->map[i].root, 64);
    }
    CHECK(!source_query(&src) && (src.flags & LGN_SOURCE_PLAN) && src.generation == en->generation && !src.staged);
    uint32_t dad = src.rows[0].uid, kid = src.rows[1].uid;
    CHECK(dad == 1000 && kid == 1001);
    char names[ACCT_MAX][32]; CHECK(ui_list(names) == 4);   /* LOGON_UI reads the provisional catalogue */
    for (unsigned i = 0; i < 200; i++) world();
    CHECK(logon.mode == LGN_MODE_ENROLL && logon.enrolling);   /* a provisional catalogue is not a logon list */
    struct lgn_progress p = enroll(dad, "wrongold", "dadpass12");
    CHECK(p.disposition == LGN_FAILED && p.status == -EACCES);
    g_ticks += 1000;
    p = enroll(dad, "dadpass", "short");
    CHECK(p.status);
    g_ticks += 1000;
    p = enroll(500, "", "recovery12");
    CHECK(p.disposition == LGN_ENROLL_STAGED && !p.status);
    p = enroll(dad, "dadpass", "dadpass12");
    CHECK(p.disposition == LGN_ENROLL_STAGED && !p.status);
    CHECK(!source_query(&src) && src.staged == ((1u << 0) | (1u << 2)));
    p = enroll(kid, "", "kidpass12");
    CHECK(p.disposition == LGN_ENROLL_COMMITTED && !p.status);
    RUN_UNTIL(logon.mode == LGN_MODE_WELCOME && logon.accounts_ready, 4000);
    st = state(); CHECK(st.message == LGN_MSG_ENROLLED && !logon.enrolling);
    (void)ui_state(LGN_CMD_ACKNOWLEDGE, 0, 0, NULL);
    unsigned n = ui_list(names); int found_dad = 0, found_kid = 0;
    for (unsigned i = 0; i < n; i++) { found_dad |= !strcmp(names[i], "Dad"); found_kid |= !strcmp(names[i], "Kid"); }
    CHECK(n == 2 && found_dad && found_kid);
    CHECK(source_query(&src) == -ENOENT);   /* enrolled: nothing left to convert */
    logon_as("Dad", "dadpass12");
    const struct ciuki_credential *c = desktop_credential();
    CHECK(c->uid == 1000 && c->groups[0] == 544);
    CHECK(!strcmp(last_profile, "/Windows/Profiles/u00000003"));   /* the G32 profile keeps its root */
    puts("H12 G32 upgrade: LOGON shows the enrollment screen (no autologon), ENROLL_SOURCE lists the protected old "
         "accounts with the reserved name flagged, the plan must relabel it, ENROLL needs a plan, old passwords are "
         "verified, 8-63 new passwords, staged then committed, Welcome lists the converted accounts and the first "
         "administrator logs on with its retained profile PASS");
}

#ifdef CIUKI_CONTROL_PANEL   /* fx-uaccounts: the production Control Panel process */
#include "control_accounts_test.inc"
#endif
static int h12_scenario(const char *name)
{
    if (!strcmp(name, "accounts")) scenario_accounts();
#ifdef CIUKI_CONTROL_PANEL
    else if (!strcmp(name, "control-panel")) scenario_control_panel();
    else if (!strncmp(name,"control-panel-",14)) scenario_control_completion(name+14);
#endif
    else if (!strcmp(name, "saver")) scenario_saver();
    else if (!strcmp(name, "saver-blank")) scenario_saver_blank();
    else if (!strcmp(name, "override")) scenario_override();
    else if (!strcmp(name, "upgrade")) scenario_upgrade();
    else if (!strcmp(name, "last-admin")) scenario_last_admin();
    else return 0;
    lgn_client_close(&cp_link);
    return 1;
}
