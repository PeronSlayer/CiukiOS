# f2-80 — Files: directory listing and navigation off the UI thread, cheap listing on removable volumes; desktop media polling off the frame thread

- **Phase:** F2+ (desktop/apps). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-80` from main (after `2fef3ffc`).
- **Authority:** `apps/files/app.c`, `apps/files/model.c`, `apps/files/vfs.c`, `apps/libdeskui` (a "Loading…" state if needed), `apps/desktop/desktop.c` (`media_read`), `apps/desktop/client.c` (ping/unresponsive), host tests `tests/host/desktop/` (files/desktop tests, `app_replay.c`).

## Evidence (T23, image 43d72a5c, owner report 2026-10-16 night; `legacy/local/physical/43d72a5c/BOOT2.LOG` black box, read-only)

- Owner: clicking inside the stick in Files makes Files "Not responding" (desktop ping without reply for 5 s, `client.c:774`); the system lags meanwhile.
- `files_navigate` (`app.c:565`) calls `fm_navigate` synchronously on the UI thread; the browser listing (`model.c:229 browser_listing`) uses `list()` with stat per entry and up to 128 ELF sniffs (`vfs.c:34-56`, `FM_LIST_SNIFF`), i.e. hundreds of commands on a 28 ms-per-sector stick; `[cpu] top=files 22–44 %` for seconds.
- The desktop frame thread calls `media_read` (26 `MEDIA_GET_VOLUME` devctls) when the media panel opens (`shell.c:2154 want_reload|=32`) and after a removal; each devctl waits for the VFS lock (f1-79 removes the wait in the kernel; the desktop must still not issue 26 blocking calls on the frame thread).

## Required

1. Files: listing, navigation and refresh run on a worker with a cancellable job; the UI thread shows the path, a "Loading…" line and answers pings; a new navigation cancels the previous listing; results are applied only if their revision matches. Keyboard/mouse remain live during listing. Explicit file operations keep the existing `fm_job` path.
2. Cheap listing by default: the browser uses `FM_LIST_NAMES_ONLY` (type/size/mtime from the directory entry, f2-79) for every volume; the ELF sniff of extension-less files runs lazily on the worker after the listing is shown and only for rows that are visible (or on open), never during the first paint; icons by extension/type.
3. Desktop: `media_read` moves off the frame thread (the folder/autoplay worker pattern of f2-79) or becomes a single non-blocking snapshot devctl once f1-79 lands; the frame thread only consumes the result; the stall observer never records `wait=media-read` ≥ 100 ms with a 25 ms-per-call fake VFS.
4. Host coverage: Files listing of a 300-entry fake volume with 25 ms per call keeps the UI responsive (ping answered within 1 s, first paint within 300 ms, listing completes in the background, cancellation on navigation); desktop media panel open with a slow volume: no frame-thread stall; `make CAP= desktop`, `make CAP= apps`, `python3 -B -m unittest discover -s tests/host/desktop -t .`, `python3 -B -m unittest discover -s tests/host -p 'test_desktop_gate.py'`.
5. Report `build/codex/f2-80-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Shell state changes only on the frame thread (f2-78 rule). UI copy in English.
