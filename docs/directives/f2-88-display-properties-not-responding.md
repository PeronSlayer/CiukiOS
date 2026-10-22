# f2-88 — Display properties window goes "not responding" on the T23

- **Phase/model:** F2, Claude agent (Sonnet; escalate to Opus if the cause is in the kernel). **Worktree:** `wt/f2-88` from `main`.
- **Evidence:** owner report (T23, 1.0.0 alpha, 2026-10-21 23:48): opening Display properties in the Control Panel shows the window immediately in the "not responding" state. Serial: `display-#39` task created, no record of its exit; `[desktop] stall stage=launch no_frame_ms=2149`. `src/kernel/core/display.c` answers the mode list from `g_boot` without blocking, so the wait is in the applet's host calls (`apps/control/display.c:118-150`: `host.engine` Savage query, `host.display`), in cp_host, or in the desktop's "not responding" heuristic while the applet is still starting.

## Required

1. Reproduce on the host with the Control Panel harness and the T23 display/accel fixture (Savage with the BCI quarantined, 1024×768); find the blocking call and its duration; the applet must render its first frame within 500 ms with every host call bounded (timeouts, no synchronous accel query on the UI thread).
2. The desktop's "not responding" marking must use the same rule for every application (first frame deadline, then heartbeat), and must not fire while the application is inside a desktop-owned wait (mailbox-wait on the desktop's own worker).
3. Host tests: the applet start with slow/absent accel info; the desktop heuristic with a 2 s first frame.
4. If the T23 disk logs (`legacy/local/physical/8f59e221/*.LOG`, to be copied by the owner) show a different cause, the lead will append it; design for the evidence above.

Rules as f1-92. Report `build/codex/f2-88-report.md`.
