/* SPDX-License-Identifier: MIT
 * fx-uaccounts: the production Control Panel (apps/control/main.c + app.c)
 * hosted as one session process of the kernel host harness
 * (control_panel_test.c), driven by control_accounts_test.inc. Every call
 * runs on the harness's thread while no Control Panel worker is running;
 * cpt_settle() runs the production native loop, whose workers drive the
 * kernel model through the supplied system seam until they complete. */
#ifndef CIUKI_CONTROL_PANEL_TEST_H
#define CIUKI_CONTROL_PANEL_TEST_H
#include <stdint.h>
struct lgn_client_sys;
struct cpa_view;

/* User Accounts widget ids (apps/control/accounts.inc). */
enum { CPT_LIST = 1620, CPT_LINE0 = 1622, CPT_TASK0 = 1630, CPT_NAME = 1642, CPT_ADMIN = 1643, CPT_OLD = 1645,
       CPT_NEW = 1647, CPT_CONFIRM = 1649, CPT_GO = 1650, CPT_CANCEL = 1651, CPT_NOTE = 1652, CPT_NOTE2 = 1654 };
enum { CPT_CREATE, CPT_RENAME, CPT_PASSWORD, CPT_PICTURE, CPT_TYPE, CPT_ENABLE, CPT_DELETE };   /* task buttons */

/* Starts the Control Panel on `page` (its --panel= name) with the accounts
 * link's system seam bound to `sys`, exactly as main() attaches it in a
 * profile session. Opening the page submits the READ worker. */
void cpt_open(const struct lgn_client_sys *sys, const char *page);
/* Runs the production loop (control_run) until it would sleep with no
 * worker left. 0, or -1 when a completion token woke the loop and the page
 * still held a finished worker's result after idle() (the "Reading the
 * accounts..." hang), or -2 when a worker did not complete in 120 s. */
int cpt_settle(void);
/* The Control Panel's main() shutdown: joins workers, closes the link. */
void cpt_close(void);

int cpt_busy(void);                         /* a User Accounts request is in flight */
const struct cpa_view *cpt_view(void);      /* the page's adopted view */
const char *cpt_note(void);                 /* the page's note (both lines) */
const char *cpt_text(int id);               /* a widget's text; NULL when absent */
int cpt_disabled(int id);                   /* -1 when absent */
unsigned cpt_rows(void);                    /* account list rows */
const char *cpt_row(unsigned index);
int cpt_task(void);                         /* open task form: 0 none, 1 + CPT_* */
unsigned cpt_password_notices(void);        /* DESK_SETTINGS_PASSWORD_SET queued to the desktop */
void cpt_activate(int id);
void cpt_select_row(unsigned index);
void cpt_edit(int id, const char *text);    /* DU_EDIT text */
void cpt_type(int id, const char *text);    /* keys into a masked secret field */
#endif
