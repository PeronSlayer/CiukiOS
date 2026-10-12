# f2-38 — Shut Down and Restart close every program and VM before the power request

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-38` (branch `wt/f2-38` from `main`). Host-testable (desktop harness, gate tests); the lead qualifies on QEMU and the T23.

## Defect (T23, 2026-10-12)

With a DOS window (DOOM) open, Ciuki > Shut Down... showed "Shut down failed (error 16)". `power_syscall` flushes storage first and `storage_shutdown()` (`src/kernel/fs/mount.c` ~307) refuses with EBUSY while any VFS node is referenced (open files of running programs, the games disk held by the DOS VM). The desktop (`apps/desktop/desktop.c` `want_power`) calls `ciuki_power` at once and reports the raw errno.

## Required behaviour

1. After the Shut Down dialog is confirmed, the desktop closes every client window the way Alt+F4 does (CLOSE, then the ordinary close protocol), including DOS windows (their VMs are torn down by the kernel when the client exits), waits up to 3 s for each to exit, and terminates survivors with the existing `terminate` path (call 71 rights). Its own descriptors (settings, mixer, log) are closed too.
2. Then it calls `ciuki_power`; on EBUSY it retries every 250 ms for up to 5 s (the boot-log drain and late exits), then shows "Some programs are still running: <names>" with Cancel/Force (Force = terminate and retry once). On any other error the current message stays, with the errno text.
3. A "Shutting down..." built-in window (same frame painter as About) stays on screen during the sequence; input is ignored except Cancel.
4. Gate scenarios and the crash-isolation probe keep their current semantics (no shutdown inside gates).

## Tests

Desktop host harness: shutdown with two clients and a DOS window model (all closed, order, timeouts, a survivor terminated, EBUSY retry then success, EBUSY persisting → message with names, Cancel restores the desktop); `test_desktop_gate.py` unchanged or extended. Document in `docs/desktop.md` (Shut Down section).

## Constraints

No kernel change expected; if `storage_shutdown` needs a bounded wait for in-flight writers instead of an immediate EBUSY, propose it in the report (do not implement). No QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f2-38-report.md`.
