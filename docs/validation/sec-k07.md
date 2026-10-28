# sec-k07 trusted gate and kernel teardown integration

Scope: sec-wave4 first round, contract row 5 (H09). Host evidence only; no
QEMU, image or hardware result is claimed. Binding specification:
[user-security-kernel.md](../design/user-security-kernel.md) sections
"Sessions and logoff", "Session launch and secure console (A1-2)", the A2-3
transition table and the service table, plus main contract sections 4.3 and
4.4 of [user-security-contract.md](../design/user-security-contract.md).
Written 2026-10-27 (after `date -Is`).

## Primary sources and resulting decisions

| Source | Finding | CiukiOS decision |
| --- | --- | --- |
| Microsoft [Initializing Winlogon](https://learn.microsoft.com/en-us/windows/win32/secauthn/initializing-winlogon) and [Responsibilities of Winlogon](https://learn.microsoft.com/en-us/windows/win32/secauthn/responsibilities-of-winlogon) | Winlogon registers Ctrl+Alt+Delete as the secure attention sequence before any other process can hook it, creates its own desktop in WinSta0 (the physical screen, keyboard and mouse) and switches to it on SAS, so logon/unlock UI is invisible to applications. | The kernel consumes the physical sequence in the merged input queue, before the DOS full-screen hook and any user grant, and switches the one input queue and the display to a trusted domain owned by LOGON_UI. |
| Microsoft [WlxLoggedOnSAS](https://learn.microsoft.com/en-us/windows/desktop/api/winwlx/nf-winwlx-wlxloggedonsas) and [WlxWkstaLockedSAS](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxwkstalockedsas) (GINA, historical XP interface) | SAS while logged on shows the security options; SAS while locked leads only to the unlock path. | Matches the frozen A2-3 table already implemented by k01: ATTENTION from USER leaves authentication_required=0; ATTENTION while locked keeps it at 1. k07 does not change those rules. |
| Microsoft [Desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops) | One input desktop at a time; applications on other desktops keep running. | Two exclusive kernel domains (trusted / session); concealed applications keep executing, their display calls answer EBUSY and input reads see no events. |
| Microsoft [Logging Off](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off) and [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex) (EWX_LOGOFF) | Logoff shuts down every process of the logon session before the user is logged off; the query/veto protocol precedes it. | END_BEGIN (after the desktop's own query/veto, sec-user-03) conceals at once, stops every session process; PID 1 also ends the session's DOS VMs and checks its Win32 objects; COLLECT reports CLOSING until all are gone. |
| IBM PS/2 Technical Reference / Linux atkbd (already cited by `drivers/i8042.c`) | Set-2 Delete is E0 71, keypad Del 71, Ctrl 14 (E0 14), Alt 11 (E0 11). | The filter works on the public set-1 positions after decoding (0x153/0x53, 0x1D/0x11D, 0x38/0x138), so PS/2, USB HID and the firmware adapter share one rule. |

The Windows 98 Resource Kit (owner's copy, `build/rskt`, not copied) describes
Ctrl+Alt+Del as the Close Program dialog; that behaviour is kept unchanged for
the interim desktop (below).

## Repository comparison

- k01 delivered the coordinator (`proc/security.c`): states, epochs, cookies,
  launch/readiness and a `security_grant_ops` adapter that failed closed with no
  backend; `security_console_attention` existed but nothing called it.
- Grants were a single owner (`grants.c` `grant_owner`); the only input
  consumer was the merged queue (`drivers/i8042.c`), with the DOS full-screen
  hook running in the producer before the queue. Ctrl+Alt+Del reached the
  desktop as ordinary keys (Task Manager, `apps/desktop/shell_g18.h`).
- k05 added `dosvm_session_revoke`; k06 scoped Win32 objects/reports by session
  but had no teardown entry; there was no session console object.

## Implementation

- **Physical secure attention** (`drivers/i8042.c`, sec-k07 block): in
  `key_emit` of the native controller, Delete or keypad Del pressed while a
  physical Ctrl and Alt are held is consumed when `security_attention_armed()`
  (a kernel LOGON role was registered this boot). Its repeats and release are
  swallowed; the queue switches to the trusted domain at once (pending user
  events discarded, RESYNC, then `CIUKI_INPUT_ATTENTION`), and PID 1 performs
  `security_console_attention(ATTENTION)`. No syscall writes this queue; the
  H09 census checks that only the queue and the grant backend call the domain
  switch. A trusted domain never feeds the DOS full-screen hook.
- **Domains** (`proc/grants.c`): LOGON_UI's trusted pair and one session's
  desktop pair. Session grants start suspended; only k01's activation (initial
  POLL or SECURE_LEAVE) enables them. `grants_display_pid/owner` report the
  active user owner only, so DOS full screen returns (existing owner-lost
  watchdog), new full-screen and present-lease requests refuse, and the
  present lease is revoked by its existing poll. Suspended display calls
  return EBUSY (as during a DOS handoff); suspended input reads return EAGAIN
  or block. In-flight presents/accelerator writes recheck the domain at every
  tile and stop; batches recheck through their authority callback; the trusted
  UI has no batch engine. Dequeue is atomic with the domain test.
- **Fences and neutral frame**: concealment of a visible session returns EBUSY
  until user presents, DOS full screen, the present lease and display
  suspension have drained and PID 1 has painted a black kernel frame; trusted
  presents wait for it. Activation discards keystrokes typed into the trusted UI.
- **Backend** (`proc/secure_console.c`, registered by `security_init`):
  `prepare` installs the private session console (fds 0..3, from
  `files_session_console` in `proc/fdtable.c`: one `/dev/console` description,
  writes to the session log, reads EOF, access mode fixed) plus the suspended
  desktop pair (4/5) or, for w32srv, a kernel notification channel (4) that
  receives one 16-byte `{1, ACCEPT_PENDING, session, 0}` message per pending
  W32SRV link (`proc/win32/service.c`). `trusted` gives LOGON_UI the same
  console and the trusted pair; PID 1 adds `CIUKI_CONSOLE_FD/DISPLAY_FD/INPUT_FD`
  labels. Activation requires a fresh grant generation.
- **Teardown**: COLLECT keeps CLOSING until a PID 1 pass found no DOS window
  VM (`dosvm_session_revoke`) and no Win32 object bound to the session
  (`w32_session_revoke`, which also purges that session's pending crash and
  refusal reports). Undrained or unfenced sessions block the next
  SESSION_CREATE (EBUSY), never panic. The release callback runs with the
  session record pinned (it may drop the last subject of that session).

## Interim desktop transition

Until sec-user-03 ships `/System/Services/logon` and `logon-ui`, PID 1 starts
the approved uid-1000 interim desktop, no LOGON role is ever registered and
`security_attention_armed()` is false: Ctrl+Alt+Del is not consumed, reaches
the desktop and the DOS full-screen system path exactly as before (Task
Manager, second press Shut Down), and the single legacy grant model is
unchanged (`grants_install`). The first boot that registers LOGON arms secure
attention for that boot; `grants_install` then refuses legacy grants. For
sec-user-03: LOGON_UI reads `CIUKI_INPUT_ATTENTION` (code
`CIUKI_SECURITY_SECURE_ATTENTION`) on its trusted input grant and relays it
to LOGON as LOCK_EVENT; LOGON then calls SECURE_ENTER(ATTENTION) (idempotent
for the kernel's own entry, or a fresh tuple when the physical entry had to
wait for a fence) and polls SECURE_ENTER while it returns EBUSY.

## Host evidence

See `build/codex/sec-k07-report.md` in the sec-k07 worktree for exact commands
and results (H09 three programs, the H07 and H06 additions, regressions).
