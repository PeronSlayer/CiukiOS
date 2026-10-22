# f2-89 — Removable media window: transparent, unreadable background

- **Phase/model:** F2, Claude agent (Sonnet). **Worktree:** `wt/f2-89` from `main`.
- **Evidence:** owner report (T23, 1.0.0 alpha, 2026-10-21 23:58): clicking the removable media icon (USB stick, drive E:) opens a window whose background stays transparent, so the content is unreadable. The volume mounted fine (`[usb-storage] volume drive=E part=1 fat=32 mode=ro … mount_ms=37`).

## Required

1. Reproduce with the desktop/Files host harness: the window opened for a removable volume (USB, ro mount) must paint its background and chrome before any content, exactly like a C: folder window; find why the first paint is skipped (surface never cleared, paint deferred behind a slow directory read, or the ro/removable path missing the background fill).
2. Every libdeskui window must clear its surface in its first frame regardless of content readiness; add the regression test (golden frame of a removable-media window with an empty and with a slow directory).
3. Verify the same for the Files window opened on D: and the optical drive.

Rules as f1-92 (no QEMU, no image build, no git mutations, no diary edits, nothing deleted outside the worktree, `date` before timestamps, English). Report `build/codex/f2-89-report.md`.
