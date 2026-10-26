# fx-launchfb: start-up feedback cursor for every program launch (owner, T23 trial 5bb73dba)

- **Written:** 2026-10-26 16:31 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main be7852468.
- **Owner report:** "when I open any Win32 program there is no feedback from the cursor; it looks as if I am not opening anything." On the T23 Win32 programs take seconds to show their first window.

## Scope
1. **Research and cite.** Windows shell launch feedback: the IDC_APPSTARTING cursor (arrow plus hourglass) shown after a launch from Explorer, Start or Run until the new process shows a window or reaches its first input idle, with a timeout of about 5 s. Also STARTF_FORCEONFEEDBACK/STARTF_FORCEOFFFEEDBACK in STARTUPINFO, and the Windows 98 behaviour.
2. **Implement it in the desktop launcher for native, Win32 and DOS launches from Start, Run, desktop icons, Files and shortcuts.** The pointer becomes the app-starting cursor over the desktop and other windows until the launched process's first window is presented. A launch failure (error message), the process exit or the timeout also ends it. Several pending launches are counted. A busy hourglass cursor set by the application itself keeps precedence over the launch cursor. Use the existing cursor themes; if a theme has no app-starting cursor, add one in the theme's style (Tango-derived). Never alter the Ciuki portrait.
3. **Win32 programs.** Honour STARTF_FORCEONFEEDBACK and STARTF_FORCEOFFFEEDBACK when one program starts another.
4. **Tests.** Window-model rows for each case: native, Win32, DOS, failure, timeout, overlapping launches, application cursor precedence. Existing desktop suites stay green and the goldens are updated only for the intended change.

Report to build/codex/fx-launchfb-report.md. Leave changes uncommitted.
