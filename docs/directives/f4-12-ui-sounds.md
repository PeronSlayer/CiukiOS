# f4-12 — System event sounds

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f4-12` from `main`. Host-testable. Owner request 2026-10-13 after hearing the start-up sound on the T23: sound feedback for actions like Windows, including when the volume is changed from the mixer.

## Task

1. Event sounds through the existing system-sounds service of f4-06 (`apps/desktop/sounds.c`, `sound_runtime.c`): start-up (exists), shutdown, log-on/desktop ready, menu open, menu command, window open, window close, minimise, restore, error/invalid action, question dialog, information dialog, volume change (short tick on every step from the tray slider and keyboard), device connect, device disconnect, AutoPlay, empty recycle later. Each event bound to a WAV in the free sound pack of f4-06 (owner note: he will compose his own later; keep the mapping data-driven in `/System/sounds/scheme.cfg` → registry `Desktop\Sounds\<event>` when f2-47 is in).
2. Control Panel Sounds page: list of events with the assigned file, preview button, "No sounds" scheme and the default scheme, volume for UI sounds separate from the master (mixed in the service).
3. Rules: sounds never block the UI (the service already plays asynchronously; verify), coalesce bursts (volume tick at most every 60 ms), no sound when the master is muted, DOS/Win games' audio has priority (UI sounds skipped while a VM stream is active).
4. Host tests: event mapping, coalescing, muted master, Control Panel page through the harness.

## Acceptance

Desktop and audio host suites green; `make CAP= apps` builds; `docs/desktop.md` updated.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-12-report.md`.
