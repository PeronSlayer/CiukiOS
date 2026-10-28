/* fx-uaccounts: Control Panel -> User Accounts end to end. The production
 * Control Panel (apps/control/main.c's native loop, cp_job worker,
 * completion channel and accounts glue; app.c's page: control_panel_test.c)
 * runs as /Programs/control, a session child of the desktop with the
 * desktop's credential and no service role, against the production ACCOUNT
 * server, LOGON and the k01/k07 kernel of the H10 harness. Its ACCOUNTS link
 * and QUERY_SELF reach the kernel through the same syscall boundary as every
 * other harness process (b_security/b_send/b_recv/b_close). The worker
 * thread drives the kernel model (c_wait -> world) while the Control Panel's
 * thread sleeps in its own poll(); the page must adopt each result on the
 * loop turn after the completion token, on a page without any heartbeat.
 * Included by account_controls_test.inc with -DCIUKI_CONTROL_PANEL.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "control_panel_test.h"

static struct bind ctl_bind;
static const struct lgn_client_sys ctl_sys = { &ctl_bind, b_security, b_send, b_recv, c_wait, b_close, b_now };
static struct process *ctl_p;

/* The desktop launches the Control Panel on `page` (--panel=NAME). */
static void control_start(const char *page)
{
    struct process *d = proc_find(logon.session.launch[0].pid); CHECK(d);
    d->actions[SIGCHLD].handler = SIG_IGN;
    g_current = &controller;
    ctl_bind.t = make_process(d, &ctl_p);
    const struct ciuki_credential *c = security_credential(ctl_p), *dc = security_credential(d);
    CHECK(ctl_p->ppid == d->pid && security_role(ctl_p) == CIUKI_SECURITY_ROLE_NONE);
    CHECK(c->uid == dc->uid && c->session_id == logon.session.id && c->group_count == dc->group_count);
    cpt_open(&ctl_sys, page);
    CHECK(cpt_busy());   /* opening the page started the READ worker */
}
static void control_settle(void)
{
    int r = cpt_settle();
    if (r || cpt_busy()) printf("  Control Panel settle=%d busy=%d note=\"%s\"\n", r, cpt_busy(), cpt_note());
    CHECK(!r && !cpt_busy());
}
static void control_stop(void)
{
    cpt_close();
    uint32_t pid = ctl_p->pid;
    stop_collect(ctl_p); CHECK(!proc_find(pid));
    ctl_p = NULL; ctl_bind.t = NULL;
}
static int cpt_find(const char *label)
{
    for (unsigned i = 0; i < cpt_rows(); i++) if (!strcmp(cpt_row(i), label)) return (int)i;
    return -1;
}
static int note_is(const char *text)
{
    if (strcmp(cpt_note(), text)) { printf("  note \"%s\", expected \"%s\"\n", cpt_note(), text); return 0; }
    return 1;
}
static void control_password(const char *old, const char *password)
{
    if (old) cpt_type(CPT_OLD, old);
    cpt_type(CPT_NEW, password); cpt_type(CPT_CONFIRM, password);
    cpt_activate(CPT_GO); CHECK(cpt_busy());
    control_settle();
}

static void scenario_control_panel(void)
{
    boot(0);

    /* Administrator: the whole list; create a Standard user; give Guest a
     * password and turn it on (another account: no LOGON password notice). */
    logon_as("Alice", "password");
    control_start("users");
    control_settle();
    const struct cpa_view *v = cpt_view();
    CHECK(v->valid && v->self_uid == 1000 && v->self_admin && !v->self_guest && v->count == 3);
    CHECK(cpt_rows() == 3 && cpt_find("Alice (Admin)") >= 0 && cpt_find("CiukiAdministrator (Admin, off)") >= 0 &&
          cpt_find("Guest (Guest, off)") >= 0);
    CHECK(!strcmp(cpt_text(CPT_LINE0), "Alice") && !cpt_disabled(CPT_TASK0 + CPT_CREATE) &&
          cpt_disabled(CPT_TASK0 + CPT_DELETE) == 1);
    cpt_activate(CPT_TASK0 + CPT_CREATE); CHECK(cpt_task() == 1 + CPT_CREATE);
    cpt_edit(CPT_NAME, "Bob");
    control_password(NULL, "bobpass12");
    CHECK(note_is("The account was created.") && cpt_task() == 0 && cpt_find("Bob (Standard)") >= 0);
    CHECK(cpa_find(cpt_view(), 1001) && (cpa_find(cpt_view(), 1001)->flags & CPA_ENABLED));
    cpt_select_row((unsigned)cpt_find("Guest (Guest, off)"));
    CHECK(!strcmp(cpt_text(CPT_LINE0), "Guest") && !strcmp(cpt_text(CPT_TASK0 + CPT_PASSWORD), "Reset the password"));
    cpt_activate(CPT_TASK0 + CPT_PASSWORD); CHECK(cpt_task() == 1 + CPT_PASSWORD && !cpt_text(CPT_OLD));
    control_password(NULL, "guestpass1");
    CHECK(note_is("The password was changed.") && cpt_task() == 0);
    CHECK(!strcmp(cpt_text(CPT_TASK0 + CPT_ENABLE), "Turn on the account"));
    cpt_activate(CPT_TASK0 + CPT_ENABLE); CHECK(cpt_task() == 1 + CPT_ENABLE);
    cpt_activate(CPT_GO); CHECK(cpt_busy());
    control_settle();
    CHECK(note_is("The account was turned on or off.") && cpt_find("Guest (Guest)") >= 0 && !cpt_password_notices());
    control_stop();
    puts("H12 Control Panel administrator: User Accounts READ adopted from the worker, Create Account, Guest password "
         "reset and turn-on through the page, each adopted on the turn after its completion PASS");
    log_off();

    /* Standard user: his own account only; a wrong old password, then his
     * own password (the LOGON password notice is queued). */
    logon_as("Bob", "bobpass12");
    control_start("users");
    control_settle();
    v = cpt_view();
    CHECK(v->valid && v->self_uid == 1001 && !v->self_admin && !v->self_guest);
    CHECK(cpt_rows() == 1 && cpt_find("Bob (Standard)") == 0 && cpt_disabled(CPT_TASK0 + CPT_CREATE) == 1 &&
          !cpt_disabled(CPT_TASK0 + CPT_PASSWORD));
    CHECK(note_is("You must be an administrator to change this setting."));
    cpt_activate(CPT_TASK0 + CPT_PASSWORD); CHECK(cpt_task() == 1 + CPT_PASSWORD && cpt_text(CPT_OLD));
    control_password("wrongpass", "bobpass34");
    CHECK(note_is("The current password is incorrect.") && cpt_task() == 1 + CPT_PASSWORD && !cpt_password_notices());
    g_ticks += 1000;   /* the failed-password delay */
    control_password("bobpass12", "bobpass34");
    CHECK(note_is("The password was changed.") && cpt_task() == 0 && cpt_password_notices() == 1);
    control_stop();
    puts("H12 Control Panel Standard user: own account only, administrator tasks disabled, wrong old password "
         "refused by the service, own password changed, every result adopted PASS");
    log_off();

    /* Guest through the Passwords launch name: the own-password task opens
     * after READ; the old password is required. */
    logon_as("Guest", "guestpass1");
    control_start("passwords");
    control_settle();
    v = cpt_view();
    CHECK(v->valid && v->self_uid == 501 && v->self_guest && !v->self_admin);
    CHECK(cpt_rows() == 1 && cpt_find("Guest (Guest)") == 0 && cpt_task() == 1 + CPT_PASSWORD && cpt_text(CPT_OLD));
    control_password("guestpass1", "guestpass2");
    CHECK(note_is("The password was changed.") && cpt_password_notices() == 1);
    control_stop();
    puts("H12 Control Panel Guest: Passwords opens the own-password task after READ, password changed with the old "
         "one PASS");
}
