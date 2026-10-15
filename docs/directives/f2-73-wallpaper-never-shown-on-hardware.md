# f2-73 — The wallpaper is decoded and scaled but never shown on the T23 after f2-70 (image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-73` from `main`.

## Evidence (`disk-BOOT.LOG`)

- `[console] [desktop] stage=wallpaper_file_read us=669000`, `stage=wallpaper_decoded us=1426000`, `stage=wallpaper_scaled us=1777000`: the worker reads, decodes and scales the photograph in 1.8 s, yet the desktop stays without a wallpaper for the whole session (owner).
- `[blackbox] [user 23] [desktop] stall stage=shell worker=wallpaper/settings no_frame_ms=2081…4154 input_pending=1` and `worker=wallpaper/idle no_frame_ms=…6186` repeated through the boot: the shell stage never presents a frame while the wallpaper worker is in `settings` or `idle`.
- `SYSTEM/cache/wallpaper` exists on the disk (f2-70's disk cache). The volume was writable after repair (`[storage] repaired logs=1 lost=334 … dirty=cleared`, `mode=rw`).
- f2-70 (`docs/directives/f2-70-*.md`, `docs/validation/f2-70-theme-wallpaper-performance.md`) moved every wallpaper through the worker, keeps the old scene until "one full replacement upload", caches frames and layers, and skips "an identical visible photograph after chrome has repainted".

## Task

1. Reproduce on the host replay with the production boot sequence on a 1024×768 desktop whose first wallpaper comes from disk with the cache directory present and absent, writable and read-only: find why the replacement upload never happens (candidates: the "identical photograph" skip comparing against an empty scene; the publication waiting for a chrome repaint that never comes because the desktop presents only on damage since f2-65; the cache write failing on hardware and the fallback path not presenting; the worker stuck in `settings`).
2. Fix so that the first wallpaper is presented within one second of `wallpaper_scaled` and a change of wallpaper or theme is presented within one frame of its completion; the stall record must name the exact wait when a frame is not presented for more than 500 ms.
3. Replay tests for the four combinations above and for the theme change; goldens; PNGs under `build/host/f2-73/`.

## Acceptance

Desktop suite and gate green; `desktop-shell-qemu-t23` green; on the T23 the wallpaper appears at boot.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Build the kernel (`python3 scripts/build_kernel.py`) and the DOS image (`python3 scripts/build_dos_image.py`) before the report. Evidence (read-only): `legacy/local/physical/a395ffa8/` (`disk-BOOT.LOG` latest normal boot with the `[blackbox]` head of the previous boot, `disk-BOOT1.LOG` first boot, `disk-BOOT2.LOG` the Hardware check boot, `serial.log` corrupted). Owner's standing rule: every hardware decision is grounded in the supplied driver package (`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/` with the INF/registry tables of the vendor drivers, `third_party/drivers/` DOS drivers) and in upstream open sources found on the internet (X.Org, Linux, FreeBSD, SeaBIOS, vendor datasheets); cite them in the report; never program registers from reasoning alone. Report `build/codex/f2-73-report.md`: root causes (file:line), fixes, verbatim test output, what the lead must verify on the H page and on the T23.
