# f4-07 — Music Player and the Image Viewer in the Programs menu

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search` for the decoders' licences. **Worktree:** `../CiukiOS-wt-f4-07` from `main`. Host-testable; the lead qualifies on QEMU/T23.

## Owner report (2026-10-13)

"Manca un lettore audio e un visualizzatore di immagini." The Image Viewer exists as an accessory (`/Programs/viewer`) but is not visible enough.

## Task

1. **Music Player** (`/Programs/music`, Programs > Music Player, Tango `audio-x-generic`/`multimedia-player`): a libdeskui program that plays WAV (PCM 8/16-bit, mono/stereo, any rate with resampling to the device rate), MP3 (minimp3, CC0, vendored under `third_party/minimp3` with licence) and Ogg Vorbis (stb_vorbis, public domain/MIT, `third_party/stb`), through the same audio path the system sounds use, with play/pause/stop/previous/next, seek bar, time display, volume linked to the master mixer, a playlist (add files/folders from Files, save as `.m3u` in `/Documents/Music`), window resizable, keyboard shortcuts. Decoding in a worker thread with a bounded ring so the UI never stalls; stops cleanly on close and on shutdown.
2. Files: `.wav`, `.mp3`, `.ogg`, `.m3u` open with Music Player (catalog); double-click plays.
3. **Image Viewer**: add it to Programs (top level, "Image Viewer") besides Accessories, associate `.bmp/.png/.jpg/.gif` (already), add Next/Previous within the folder and Fit/100% toggle if missing, and a desktop icon "Pictures" only if the owner's `/Documents/Pictures` exists (created by the image builder, empty).
4. Tests: decoder wrappers on fixtures (short generated WAV/OGG/MP3 files, licences respected), player state machine, playlist parsing, catalog associations, renders to `build/f4-07/*.png`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, one host suite at a time. Report `build/codex/f4-07-report.md` with decoder provenance.
