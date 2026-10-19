# f2-84 — Files: copy, move and delete on a worker with real progress dialogs; a copy from the USB stick must not make the system crawl

- **Phase:** F2 (applications). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-84` from main (after `0aad7c2a`).
- **Authority:** `apps/files/` (`app.c`, `model.c`, `model.h`, `vfs.c`, `transfer.c`), `sdk/deskui` (`du_progress` and a new progress dialog with bytes/rate/cancel only if the toolkit lacks it), `apps/music` only if it shares the copy path, `tests/host/desktop/` (Files tests, a slow-VFS model), `tests/suites/f2-desktop.json` (one new QEMU case with a throttled USB stick, written but not run), `docs/design/desktop-shell.md` or the Files design note.
- **Not yours:** the kernel VFS, USB and storage drivers, the desktop compositor.

## Owner report (T23, image c2d2a237, 2026-10-19)

"Provando a copiare un file dalla chiavetta (un MP3) il sistema ha iniziato a essere lentissimo al punto che ho dovuto spegnerlo forzatamente. Servono finestre di dialogo che mostrino l'avanzamento della copia, del taglio e dell'eliminazione."

## Evidence (`legacy/local/physical/c2d2a237/serial.log`, read-only; Files = pid 8, from 356 s to the forced power-off at 394 s)

- `[cpu] top=files#36:592k0s0,desktop#17:167k…` … `files#36:304k18s3,desktop#17:474k64s2`: the Files UI task takes 30–59 % of the CPU and the desktop 17–51 %, `idle_pm=0–2` for 38 s; no `[files]` records exist, the transfer prints nothing.
- `[storage] vfs_wait drive=E ms=106 holder=E reason=volume` once; the USB stick itself is healthy (f1-81: 58 ms mount, ~1 MB/s).
- Code: `start_job` arms a **2 ms** UI timer (`du_timer_start(a->ui, T_JOB, 2000000u, 1)`); `job_tick` runs **24** synchronous `fm_job_step` calls per tick on the UI thread; each step does one blocking 16 KiB read or write (`FM_CHUNK 16384`) on the VFS; `-EINTR` returns 1 and is retried at once (busy retry); `progress()` rebuilds and redraws the dialog **on every tick** (`du_progress` with `j->completed / j->count`: items, not bytes, so a single large file shows 0 of 1 until the end). The UI thread is therefore blocked in I/O for most of the time, the dialog repaints up to 500 times per second, and the desktop composes it each time.

## Required

1. **Worker.** Run `fm_job_step` on a dedicated worker thread (the pattern Files already uses for directory listings and Music for decoding): the UI thread never calls the VFS during a transfer. The worker reports progress through a mailbox/atomic snapshot (bytes done, total bytes, items done/total, current name, rate) that the UI reads at 10 Hz. Replace prompts (`FM_REPLACE`) and errors are marshalled to the UI thread; the worker blocks on the answer. Cancel is immediate at the next step boundary and leaves no temporary file (`j->temporary` is removed).
2. **I/O.** Chunk 64 KiB (measure 32/64/128 on the host slow-VFS model and on the f1-81 USB numbers: 128 sectors per bulk command), no busy retry on `-EINTR` (block or back off ≥ 1 ms), one `sync` per file as now. Total bytes are counted before copying (a cheap listing pass) so that progress is by bytes; the per-file position is also reported.
3. **Dialogs.** One progress dialog per job kind — "Copying…", "Moving…", "Deleting…" — with: current item name and from/to, a bytes bar (percent), items done/total, transfer rate and remaining time (after 2 s of samples), elapsed, a Cancel button, and a pause of the window-close while running (close = cancel after confirmation). Delete shows items only. Properties keeps its counting dialog. Visual style from the existing `du_message`/`du_progress` (Ciuki Blue and Platinum); update the goldens that change and record the frames in `build/codex/f2-84-frames/`.
4. **Responsiveness budget.** During a copy of a 10 MB file from a 1 MB/s device: the Files window and the rest of the desktop answer input within 50 ms; Files UI task CPU ≤ 5 %, desktop CPU ≤ 10 % attributable to the dialog (10 Hz repaints of the dialog region only, not the whole window). Prove it on the host with a slow-VFS model (each read sleeps per byte to simulate 1 MB/s) and the existing desktop test harness timing; write one QEMU case in `tests/suites/f2-desktop.json` (throttled USB stick via the runner's `throttle_iops`, a copy from E: to C:, observation: input consumed during the copy, desktop frame time bounded, the dialog visible in a capture) and leave it unrun.
5. **Records.** Protected `[files] job=copy|move|delete start items=%u bytes=%llu` / `done ms=%u bytes=%llu rate_kbs=%u errors=%u` / `cancel` records, and one `progress` record every 5 s while a job runs.
6. **Verification.** `make CAP= apps`, `python3 -B -m unittest discover -s tests/host/desktop -t .`, `python3 -B -m unittest discover -s tests/host -p 'test_desktop_gate.py'`, `make CAP= desktop` if the toolkit changes; if a public header changed `python3 scripts/gen_api_reference.py`. Report `build/codex/f2-84-report.md` with the before/after CPU and latency numbers from the host model.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. English UI copy. No new repository-root files.
