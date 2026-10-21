# f2-90 — Wallpaper must be ready when the desktop appears

- **Phase/model:** F2, Claude agent (Sonnet; Opus if the fix is in the supervisor/kernel). **Worktree:** `wt/f2-90` from `main`.
- **Evidence:** owner (T23, 2026-10-21 23:58): the wallpaper keeps loading slowly after the desktop appears; it must already be loaded. Serial: `[desktop] stall stage=ipc … wallpaper_wait=mailbox-wait` repeated; the wallpaper is decoded by a worker after the first frame and its read on C: competes with the holds of f1-93.

## Required

1. The wallpaper is available before the first desktop frame: store it in the image in the framebuffer's native raw format (built by `build_image.py` from the approved asset, deterministic), read it with one sequential read before the desktop's first paint (or mapped by the supervisor), and paint it in the first frame; no decode on the desktop's worker after start.
2. Budget on the T23 model: wallpaper ready ≤ 150 ms after the desktop process starts (one 1024×768×32 read at the measured device rate); report the measured number from the host model.
3. Fallback: if the raw file is missing or corrupt, the solid colour background is painted in the first frame and the decode happens later; never a partially painted frame.
4. Host tests: first frame contains the wallpaper (golden), the fallback, the raw file's determinism in the build (`tests/host` image checks).

Rules as f2-89. Report `build/codex/f2-90-report.md`.

## Amendment 1 (lead, 2026-10-22 00:1x, decision on the delivery)

The synchronous preload before the first paint is accepted. The 1,123 ms
figure was computed at the 2.8 MB/s of the failed trial, which ran on the PIO
fallback of the ATA quarantine (f1-92); at the UDMA2 rate the read is about
190 ms, within what the owner asked ("already loaded"). A bounded wait with a
solid first frame and a swap is a follow-up only if the T23 measurement after
f1-92 exceeds 300 ms. Corrupt-file detection: follow-up (size and a header
magic in the raw frame).
