# f4-30 — the music player does not load an MP3 from the stick; the file picker must show Places

- **Phase:** F4 (media). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f4-30` from main (after `7ef68c1d`).
- **Authority:** `apps/music/` (`player.c`, `decoder.c`, `playlist.c`, `view.c`, `main.c`), `apps/libdeskui/` (`du_file_dialog`, listers, icons), `apps/desktop` media state for the Places list, host tests `tests/host/desktop/` (music/libdeskui tests).

## Evidence (T23, image eca4d773, owner report 2026-10-18; `legacy/local/physical/eca4d773/BOOT1.LOG`, read-only)

- Owner: opening an MP3 from the stick starts the player but the file is never loaded ("non carica proprio l'mp3"); on the previous image the open happened on a quarantined device, this time USB was healthy. The owner also wants the open dialog to show Places so an external volume is easy to pick.
- `[launch] name=music pid=4 … first_frame_us=17278000` and nothing else: the app logs no open/decode result, so the failure is invisible. `decoder.c` supports WAV, OGG (stb_vorbis) and MP3 (minimp3, `MP3_INPUT 16384`); `player.c:36 music_decoder_open(...)` on the UI thread via `fopen`; the stick delivers about 340 KB/s with 28 ms per command.

## Required

1. Diagnostics first: `[music] open path=… result=… ms=…`, `[music] decode format=… rate=… channels=… first_pcm_ms=…`, `[music] error stage=… errno=…` through the native diagnostic sink (as the desktop does), and a visible error line in the player ("Cannot open …: reason") instead of silence.
2. Reproduce on the host with the real decoder and a fake slow volume (25 ms per read, 8 KiB reads) using a real MP3 fixture (generate a short one with a permissive tool in the test, or use an existing fixture; no copyrighted material); find why the open/decode fails or stalls (candidates: open on the UI thread blocking the event loop so the "Not responding" path abandons it, `fopen` on `/mnt/e/...` paths with long names, read errors on short reads, decoder needing more than one 16 KiB input chunk before the first frame, seek on a non-seekable stream) and fix it. Loading and decoding run on the player's worker; the UI shows "Loading…" and stays responsive; playback starts within 2 s on the slow fake volume.
3. `du_file_dialog`: a Places pane (left column) listing Desktop, Documents, Music, Pictures, the fixed volume and every mounted removable/optical volume with its label (from the desktop media state through the existing observer or the `/dev/media` devctls), with the current folder highlighted; selecting a place navigates; the pane updates when a volume appears or disappears while the dialog is open. Same dialog for Files-style "Open" in other apps where `du_file_dialog` is used.
4. Host coverage: music open/decode/play from the slow fake volume (MP3, WAV, OGG), error reporting, dialog Places rendering and navigation (golden hashes updated deliberately), `make CAP= apps`, `python3 -B -m unittest discover -s tests/host/desktop -t .`, gate.
5. Report `build/codex/f4-30-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. UI copy in English. No copyrighted audio in the repository.
