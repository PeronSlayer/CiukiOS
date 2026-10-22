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

## Lead note (2026-10-22 23:01): QEMU evidence

`build/lead-scratch/qemu-control-probe-serial.txt` (current image, QEMU
qemu-t23 profile, `scripts/test/control_display_probe.py`): `[control]
display open ms=0 decode=pending`, then `[cpu] top=control#34:462k460s3,
control#32:152k124s3` and `[control] display open ms=3300 decode=3220
render=16 … ping_answered=23`. The decode worker spends almost all of its
3.2 s **in the kernel** (460 of 462 ms per 2 s window are system time), not
in the PNG decoder, and the UI thread too (124 of 152 ms). Find which kernel
paths those are (the chunked file read with 1 ms sleeps, demand-zero faults
of the multi-MB decode buffers, the trylock/futex path of libpthread, the
SDK's malloc growing the heap page by page) with the host kernel models and
remove them; on the T23 the same work never finishes.
