# f2-88c — Display Properties still freezes on the T23 (third report): no polling, no decode

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f2-88c` from `main`.
- **Evidence (T23, image 3423912f…, serial `build/lead-scratch/t23-3423912f-serial.txt`):** `[user 59] [control] display open ms=0 decode=pending first_frame=0 ping_answered=0` and then no decode-completion record for the rest of the session; `[cpu] top=control#59:401–434k399–432s…, control#61:251–268k247–265s…` and later `control#63:756k749s`: the UI thread and the decode worker each spend 13–38 % of the CPU **in the kernel** for minutes. The owner cannot do anything in the dialog (Background, Appearance, Settings). On QEMU the dialog opened in 2 s (f2-88b), so the defect needs the T23's single 1.1 GHz CPU to show: a polling loop (UI turn with a zero timeout while a decode is pending, trylock retries, 1 ms sleeps) that starves the worker so the decode never completes.

## Required

1. Remove polling entirely: the worker signals completion through a file descriptor in the UI's wait set (pipe/eventfd-equivalent of the SDK) and the UI blocks in its normal event wait; no zero-timeout loops, no trylock retry loops; prove it with a host test that runs UI and worker on one emulated CPU with the kernel's scheduling rules (no busy loop: the UI thread's kernel time while waiting is ~0).
2. Do not decode the 2.5 MB wallpaper at all for built-in wallpapers: the image builder produces 192×144 thumbnails of every system wallpaper (deterministic, next to the wallpaper cache), the Background tab shows them directly; only user-chosen files are decoded, on the worker, at low priority, and cancelled when the tab changes.
3. Every tab responds to input within 100 ms on the T23 model during any background work; the applet idle ≤ 1 % CPU (aligned with f2-98a, which owns the other applets — touch only `apps/control/display.c`, `decoder.c`, `main.c` and the image builder's thumbnails).
4. Log one record per state change `[control] display state=… decode=… ui_wait=…` so the next T23 serial shows it.
5. Host tests: the no-polling test, thumbnails in the image, input latency on the model, golden frames.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.

## Amendment 1 (lead, 2026-10-22 23:41): decisions from the first report

1. **Native wait: authorized.** Fix the native `net_poll()` wait, and any signal or wakeup integration it needs, outside the applet files.
   - Wait to the real deadline or event, keep the registration/recheck race protection, and wake promptly for input, channel completion, fd close and caught signals.
   - Host tests prove each wakeup source and that an idle UI and worker consume no periodic kernel time; extend the model to execute the native wait.
   - This is the Windows 9x model: a blocked thread is not scheduled until its event fires.
2. **Worker priority: the existing policy is accepted.** The background worker stays at base 7 against base 9 for the focused UI (input boost 11), with no new priority API in this directive.
3. **Decoder cancellation** stays cooperative at decode boundaries; the UI state cancels immediately.
