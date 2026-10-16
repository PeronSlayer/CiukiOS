# f2-79 — AutoPlay probe and Desktop-folder poll: hundreds of single-sector reads on a slow stick, and the frame thread still does I/O

- **Phase:** F2+ (desktop). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-79` from main `85299550`.
- **Authority:** `apps/desktop/autoplay_content.c`, `apps/desktop/autoplay.c`, `apps/files/vfs.c` (listing), `apps/desktop/folder.c`, `apps/desktop/desktop.c` (stall observer), host tests `tests/host/desktop/` (`autoplay_test.c`, `autoplay_stall_test.h`, `desktop_folder_test.c`, `desktop_test.c`).

## Evidence (T23, image d598bf4c, `legacy/local/physical/d598bf4c/BOOT1.LOG` 1765–1955 and `BOOT2.LOG` 371–417, read-only)

- After `[usb-storage] volume drive=E … clusters=1953289 spc=16` the stall observer logs `wait=folder-service … autoplay_wait=media-scan` for 8.7–8.9 s; the controller worker of the stick ran one scan in 8704 ms (it was executing the scan's transfers). Per-command cost on this stick: about 28 ms per single-sector read (`mount_ms=252` for `reads=9 sectors=9 max_sectors=1`).
- `autoplay_content.c:109-118` `probe()` lists the root, then the music and pictures sub-directories, reads `autorun.inf`, and `ap_probe_workspace` runs it on the f2-78 worker. Every listing goes through `apps/files/vfs.c:34-56` `list()`, which calls `metadata_stat()` for every entry (a path lookup per entry) and `sniff()` (open + 52-byte read) for up to 128 extension-less files: hundreds of single-sector commands on a 28 ms/sector device.
- `folder.c:140-153`: the frame thread polls `C:\Documents\Desktop` every second with the same `fs.list` (stat per entry) and blocks for the duration of each syscall. The kernel half (namespace lock held across device I/O) is f1-78; the desktop must still never block its frame thread on storage.

## Required

1. **Cheap listing.** A listing mode that returns names and the FAT metadata the directory entry already carries (type, size, modified) without a per-entry `stat` and without sniffing. Check the libc/newlib port's `dirent` and the kernel directory syscall: if the stream already carries attributes/size/mtime, expose them; if it does not, add the smallest extension to the existing directory syscall result (no new syscall) and document it in the API design doc. `fm_vfs.list` gains a flags argument (`FM_LIST_NAMES_ONLY`, `FM_LIST_SNIFF`); Files keeps today's behaviour for its browser view.
2. **Bounded probe.** The AutoPlay probe uses the cheap listing with hard budgets: at most 256 root entries, 64 per sub-directory, 8 sniffed files, and 2 s of worker time; when a budget expires the popup appears with the generic choices (Open folder / Do nothing) and `[desktop] autoplay drive=E budget=… entries=… ms=…` is logged. `desk_autoplay_activity` reports the phase (`media-scan:root`, `media-scan:music`, `media-scan:pictures`, `media-scan:inf`). Host test: with a fake `fm_vfs` whose every call sleeps 25 ms and a root of 300 entries, the popup request is ready within 3 s of the volume update.
3. **Frame thread without I/O.** The one-second Desktop-folder comparison moves to a worker (the settings/wallpaper worker pattern) or an asynchronous listing job; the frame thread only consumes a ready result, and `desk_folder_service` performs no storage call on the idle path. Explicit folder commands (new, rename, delete, paste) keep their current synchronous semantics but are issued through the existing `fm_job` where one exists. Host test: with the 25 ms-per-call fake VFS, the stall observer never records `wait=folder-service` with `no_frame_ms` ≥ 100 during a 10 s run with a stick mounting at t=1 s.
4. **Host coverage** in `tests/host/desktop` (`autoplay_test.c`, `desktop_folder_test.c`, `desktop_test.c`, the gate), `make CAP= desktop`, `python3 -B -m unittest discover -s tests/host/desktop -t .`, `python3 -B -m unittest discover -s tests/host -p 'test_desktop_gate.py'`.
5. Report in `build/codex/f2-79-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Shell state changes only on the frame thread (f2-78 rule); the worker owns no shell state. UI copy in English; the popup text and choices stay as f2-76/f2-78 defined them.
