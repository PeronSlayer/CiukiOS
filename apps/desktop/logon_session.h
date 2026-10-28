/* SPDX-License-Identifier: MIT
 * sec-user-03: native binding of a LOGON-launched session desktop
 * (logon_session.c). Not used by host models of the shell. */
#ifndef DESK_LOGON_SESSION_H
#define DESK_LOGON_SESSION_H
struct desktop;
/* 1 when the kernel's SESSION_LAUNCH labels name this desktop; fills the
 * installed display/input descriptors. 0: interim/probe desktop. */
int desk_logon_detect(int *display, int *input);
void desk_logon_profile(struct desktop *);   /* profile paths and account name */
void desk_logon_ready(struct desktop *);     /* readiness handshake, once */
int desk_logon_ready_sent(void);
void desk_logon_poll(struct desktop *);      /* after RESYNC: secure-menu action */
int desk_logon_lock(struct desktop *);       /* SESSION_LOCK */
/* SESSION_EVENT notice (LGN_NOTICE_LOGOFF/SHUTDOWN/RESTART), w32srv flush,
 * exit. Never returns. */
void desk_logon_end(struct desktop *, unsigned notice) __attribute__((noreturn));
#endif
