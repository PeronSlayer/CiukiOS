# sec-user-03 authenticated logon and session UX

Scope: sec-wave4 second round, contract row 6A (H10, RUS, RWM). Host
evidence only; no QEMU, image or hardware result is claimed. Binding
specification: [user-security-contract.md](../design/user-security-contract.md)
sections 4.1-4.4 (with 3.1/3.2 for the account rules they cite) and
[user-security-kernel.md](../design/user-security-kernel.md) "Sessions and
logoff", "Session launch and secure console (A1-2)", the A2-3 table and the
service table; [sec-k07](sec-k07.md) for the trusted gate.

## Primary sources and resulting decisions

| Source | Finding | CiukiOS decision |
| --- | --- | --- |
| Microsoft [WlxActivateUserShell](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxactivateusershell) | Winlogon starts the user's shell only after a successful logon, with its desktop still locked; if the shell cannot start, the logon is cancelled. | LOGON creates the session, launches w32srv then the desktop (SESSION_LAUNCH) and releases the console only when both report ready; a failed or timed-out launch ends the session and the trusted screen shows "Your session could not be started". |
| Microsoft [WlxLoggedOnSAS](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxloggedonsas) | Ctrl+Alt+Del while logged on (Winlogon desktop current, workstation locked meanwhile) returns NONE (back to the desktop), LOCK_WKSTA, LOGOFF, SHUTDOWN/REBOOT/POWER_OFF ("logs the user off and shuts down"), TASKLIST or FORCE_LOGOFF. | The security screen offers Lock Computer, Log Off..., Shut Down... (Shut Down/Restart), Task Manager and Cancel. Log Off, Shut Down/Restart and Task Manager are performed by the user's own desktop after the console returns to it (the programs are asked first); Cancel is the menu return (SECURE_LEAVE). FORCE_LOGOFF is a separate, confirmed command. |
| Microsoft [WlxWkstaLockedSAS](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxwkstalockedsas) | Ctrl+Alt+Del while locked can only keep the lock, unlock, or log the user off (forced or normal) with the workstation staying locked until the logoff completes. | The locked screen asks only the owner's password; attention while locked keeps the lock (A2-3). A password-less owner whose session got locked (service restart) can only end it (FORCE_LOGOFF), the screen staying trusted until collection. |
| Microsoft [Logging Off](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off) | Logoff sends WM_QUERYENDSESSION to every window; any FALSE cancels it; agreeing programs get WM_ENDSESSION and every process of the logon session is terminated. | The desktop keeps its G3 query/veto (native END_SESSION, Win32 WM_QUERYENDSESSION through w32srv). A veto keeps the same session and identity; after consent the desktop saves settings, tells LOGON (SESSION_EVENT notice), asks the session's w32srv to flush (SIGTERM) and exits; the kernel stops the remaining session processes and LOGON collects the session (END_BEGIN/END_COLLECT) before Welcome. |
| Microsoft [Initializing Winlogon](https://learn.microsoft.com/en-us/windows/win32/secauthn/initializing-winlogon) (via sec-k07) | Winlogon owns the secure desktop; Ctrl+Alt+Del is registered before any other process. | Unchanged k07 behaviour: the kernel consumes the sequence once LOGON is registered; LOGON_UI relays the ATTENTION input event to LOGON. |

## Repository comparison

- sec-k01/k07: SESSION_CREATE/LAUNCH/LAUNCH_POLL/SECURE_ENTER/LEAVE/END and the
  real backend existed; nothing called them (no LOGON image, PID 1 fell back to
  the interim uid-1000 desktop).
- sec-user-01/02: ACCOUNT's AUTH relay, the LOGON correlation core
  (`apps/accounts/logon.c`), the ready/Setup seeds and first-boot promotion
  existed; ACCOUNT sends AUTH_OUTCOME to a LOGON endpoint nobody served.
- The desktop had the G32 desktop-owned gate (`apps/desktop/logon.inc`), a
  Start-menu Lock on that gate, G3 query/veto Log Off back to that gate, and
  spawned/supervised its own w32srv.

## Implementation

- **LOGON** (`/System/Services/logon`, `apps/logon/coordinator.c`, `links.c`):
  one nonblocking event loop. Boot gate SECURE_ENTER(0, ATTENTION); AUTH LIST
  until the account service is available; automatic logon only when the list
  is exactly the ready image's CiukiUser (uid 1000) and the boot is not Safe
  Mode (ACCOUNT still validates the whole tuple, both role generations 1 and
  its latch; LOGON consumes its own latch either way, never again this boot);
  BEGIN_AUTH/SUBMIT_AUTH relays through `acct_logon_*` with the challenge,
  verification and AUTH_OUTCOME correlation of A2-1; session start; lock and
  unlock; secure attention; the security-menu commands; Log Off collection;
  the logon screen's power requests (once, orderly path). A restarted LOGON
  adopts a live session (locked by the kernel at the old instance's death) and
  never logs on automatically.
- **Unlock proof**: before correlating an outcome LOGON refreshes its console
  binding idempotently; UNLOCK releases the console only with that exact epoch
  (a stale LEAVE is not retried with a newer tuple). An empty-secret UNLOCK
  that ACCOUNT authenticates (password-less owner) never unlocks; the owner is
  offered End Session only.
- **LOGON_UI** (`/System/Services/logon-ui`, `apps/logon/ui.c`, `ui_main.c`):
  Welcome with the accounts AUTH LIST returns (disabled Guest/500 never
  appear; Safe Mode lists a configured CiukiAdministrator), typed names, masked
  password fields (printable ASCII, 8-63 rule enforced by ACCOUNT), the
  account service's 1 s retry delay respected, Esc/Cancel never starting a
  session, no Close button; the locked screen; the security menu; waiting,
  logging-off, power and "Logon is not available" screens; Safe Mode corner
  labels; the approved Ciuki portrait (`/system/assets/ciuki-portrait.xrgb`,
  the desktop's deterministic conversion) and the tagline. It paints only when
  the trusted domain owns the console and retries presents refused while PID 1
  still owes the neutral frame.
- **Desktop** (`apps/desktop/logon_session.c`, `logon.inc`, `desktop.c`,
  `win32_native.c`, `input.c`): detects SESSION_LAUNCH labels; uses the
  installed grant descriptors; profile from the kernel environment (settings
  in `CIUKI_PROFILE_SETTINGS`, documents in `HOME`; the image's common Desktop
  and Start menu stay shared); no G32 gate; readiness after the first composed
  frame; Start > Lock through SESSION_LOCK with set-password guidance for a
  password-less owner; secure-menu actions fetched after the activation's
  RESYNC; Log Off / Shut Down / Restart end the session as above; does not
  spawn or supervise w32srv (the session's W32SRV is LOGON's).
- **w32srv** sends its readiness after its hive is loaded and it registered.
- **Kernel** (marked hunks): LOGON's frame limit 7 -> 9 (`channel.c`), the
  route table of `security_subject_check` (`security.c`), and the session
  desktop's mixer handle and termination authority with their labels at
  SESSION_LAUNCH (`spawn.c`), as PID 1 gives the interim desktop.

## Private protocol extension (for lead review)

Two LOGON (service 5) operations, private schema 1, bodies in
`apps/logon/logon_proto.h`. Neither authenticates, unlocks nor creates a
session; LOGON authorizes each by the kernel-retained peer role:

| Operation | Peer | Body |
| --- | --- | --- |
| UI_STATE=8 | LOGON_UI | 32-byte command (query, ATTENTION relay, security-menu Log Off / Task Manager / Shut Down / Restart / Set Password, confirmed force logoff, message acknowledgment) bound to the current session and console generation; 96-byte reply: screen mode, flags, session, console generation, owner uid/name, one-shot message. |
| SESSION_EVENT=9 | same-session DESKTOP | 32-byte notice (query, Log Off / Shut Down / Restart after the desktop's own query/veto, password set); 64-byte reply: pending secure-menu action (consumed), owner uid/name, password-less/recovery flags. |

SESSION_LOCK=5 keeps its body; its progress status is -EPERM for a
password-less owner (guidance, no lock). k07's note said LOGON_UI relays
attention as LOCK_EVENT: LOCK_EVENT's actions are frozen to lock/menu-return,
so the relay is UI_STATE command 1 and LOGON's idempotent SECURE_ENTER
(ATTENTION); LOGON also detects any kernel-side console change itself
(session generation watch, 100 ms).

## Open items

- G32 upgrade enrollment UI: LOGON relays ENROLL_PLAN unchanged, but no
  interface gives the trusted UI the protected G32 source records the plan
  must list exactly (A2-1), so BEGIN_AUTH ENROLL is refused and an upgraded
  installation shows "Logon is not available" with an explanation. Needs a
  contract amendment (an AUTH read of the enrollment source) before the UI.
- An administrator ending another user's locked session (contract 4.3) is not
  offered; the owner unlocks, or a password-less owner ends the session.
- Password-on-resume and saver dismissal stay sec-user-04 (a LOGON-session
  desktop uses SESSION_LOCK on resume meanwhile); sec-user-04's password page
  should send SESSION_EVENT notice 4 after a self password change so Lock
  stops showing the guidance in that session.
- The 5000 ms launch deadline (CIUKI_SECURITY_LAUNCH_TIMEOUT_MS) covers the
  desktop's start to its first composed frame; the desktop records
  `session_ready` in its startup stages and LOGON logs `start_ms`. Hardware
  timing on the Pentium III is lead evidence.
