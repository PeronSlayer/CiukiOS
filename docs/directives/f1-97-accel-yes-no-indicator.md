# f1-97 — Plain "2D acceleration: YES/NO" indicator at start-up (owner, 2026-10-22 09:33)

- **Phase/model:** F1/F2, Claude agent (Sonnet). **Worktree:** `wt/f1-97` from `main`.
- **Owner request:** at system start, for debugging, something that says plainly whether the acceleration works: "YES, it works" or "NO, it does not", without frills and without wasting time.

## Required

1. **Splash:** once the Savage activation has decided (the `[accel] savage … result=` record, before the desktop handover), draw one small line under the progress bar: `2D acceleration: YES` or `2D acceleration: NO (<reason>)` where `<reason>` is the quarantine reason token (`bci_test_readback_failed`, `unsupported_device`, …). Same font and colour rules as the tagline; no extra disk read; drawn by the console worker like the bar (f0-55/f0-55b); stays until the handover.
2. **Desktop:** at the first desktop frame, a one-line notice in the top bar (the existing shell notice mechanism, `desk_shell_notify_*`): `2D acceleration: YES — Savage` or `2D acceleration: NO — software rendering (<reason>)`, visible for 10 s, dismissed by any click; never a dialog.
3. **Log:** one protected record `[accel] summary accelerated=1|0 driver=savage reason=<token>` at the decision point so the serial and BOOT.LOG answer the same question in one line.
4. Host tests: splash golden frames with YES and NO, the desktop notice text for both cases, the summary record format; the splash line never takes the framebuffer lock longer than the bar's budget.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f1-97-report.md`.
