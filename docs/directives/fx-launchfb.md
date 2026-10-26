# fx-launchfb: start-up feedback cursor for every program launch (owner, T23 trial 5bb73dba)

- **Written:** 2026-10-26 16:31 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main be7852468.
- **Owner report:** "when I open any Win32 program there is no feedback from the cursor; it looks as if I am not opening anything." On the T23 Win32 programs take seconds to show their first window.

## Scope
1. **Research and cite.** Windows shell launch feedback: the IDC_APPSTARTING cursor (arrow plus hourglass) shown after a launch from Explorer, Start or Run until the new process shows a window or reaches its first input idle, with a timeout of about 5 s. Also STARTF_FORCEONFEEDBACK/STARTF_FORCEOFFFEEDBACK in STARTUPINFO, and the Windows 98 behaviour.
2. **Implement it in the desktop launcher for native, Win32 and DOS launches from Start, Run, desktop icons, Files and shortcuts.** The pointer becomes the app-starting cursor over the desktop and other windows until the launched process's first window is presented. A launch failure (error message), the process exit or the timeout also ends it. Several pending launches are counted. A busy hourglass cursor set by the application itself keeps precedence over the launch cursor. Use the existing cursor themes; if a theme has no app-starting cursor, add one in the theme's style (Tango-derived). Never alter the Ciuki portrait.
3. **Win32 programs.** Honour STARTF_FORCEONFEEDBACK and STARTF_FORCEOFFFEEDBACK when one program starts another.
4. **Tests.** Window-model rows for each case: native, Win32, DOS, failure, timeout, overlapping launches, application cursor precedence. Existing desktop suites stay green and the goldens are updated only for the intended change.

Report to build/codex/fx-launchfb-report.md. Leave changes uncommitted.

## Lead amendment 1 (2026-10-26 17:49): review fixes

Your work is committed (4b113cbcd). The review found no blocker; full text in /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/launchfb-duke3d-review.md. Fix these two P2s:
1. **Waiting for the first frame must cost nothing.** Marking each new Win32 client "awaiting first frame" switches the desktop to a 1 ms poll and holds gated input for up to 5 s. Input must never wait for launch feedback, and the wait must not busy-poll: use the existing event or wakeup paths.
2. **The launch cursor must not cover an application's own cursor.** It overrides hidden and custom client cursors and can flash over a full-screen game. Show it only where the system cursor would be shown: over desktop-owned surfaces and windows that use the default arrow. Never show it over a client that set its own or a hidden cursor, and never in full screen.

Also look at P3 "a completing Link launch can clear an overlapping one" and fix it if it is cheap. Add regressions. Report as "Review round 1". Leave changes uncommitted.
