# f2-98a — Windows 9x update model in the toolkit and every native application

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f2-98a` from `main`.
- Evidence: T23 trial of image 3423912f… (2026-10-22 ~22:40–22:50): serial `build/lead-scratch/t23-3423912f-serial.txt` (14,803 lines), disk log `build/lead-scratch/t23-3423912f-BOOT.LOG`, `legacy/local/physical/3423912f/NETSURF.LOG`. Music and window moves are now smooth (owner).
- Owner (2026-10-22 22:55): CiukPaint opens slowly and lags while drawing; the compositor must be fixed everywhere, not only in the first things seen; the behaviour must be the Windows 9x one. Records: `[cpu] top=control#63:756k…`, `control#66:334k` (Control Panel up to 38 % of the CPU), `idle_pm=0` in 28 two-second windows; f2-95 fixed the desktop's own workers only.
- Model to implement (Windows 9x USER/GDI semantics, cite the Win98 DDK/SDK and the Resource Kit under `build/rskt` of the main checkout, read-only): a window repaints only its update region (InvalidateRect/WM_PAINT-equivalent with the clip region set), scrolling and moving content use a blit of the unchanged pixels plus a repaint of the newly exposed strip (ScrollWindowEx), idle windows cost zero CPU, timers and animations invalidate only what changes, and the toolkit coalesces invalidations per frame.

## Required

1. **Audit** every drawing path of `apps/libdeskui` and every native application under `apps/` (CiukPaint, Files, CiukNote, Control Panel and every applet, Music, Terminal, Image Viewer, Task Manager, Accessories, games launchers): one table in the report with, for each, what triggers a repaint, what area it repaints and presents, timers, and the measured cost on the host.
2. **Fix** each path to the model: per-widget update regions in libdeskui; present only the damaged rectangles of the window surface (the desktop already composes regions since f2-95); no full-surface redraw on mouse move, hover, caret blink, timers or progress; CiukPaint strokes repaint only the stroke's bounding box, its canvas uses a blit for scroll and no per-pixel calls; startup of every application without synchronous full-tree reads or decodes on the UI thread (lazy, cached).
3. **Budgets on the T23 model** (extend `tests/host/desktop/t23_cpu_model_test.c` or a sibling): every application idle with its window visible ≤ 1 % CPU; Control Panel and every applet idle ≤ 1 %; CiukPaint freehand stroke at 60 events/s ≤ 15 %; any application cold start to first frame ≤ 500 ms on the model; report the numbers per application.
4. **Tests:** golden frames unchanged (or justified), the invalidation/present region per path, the budgets.

Do not edit `apps/desktop/compositor.c` (directive f2-98b) or cursor code (f2-99). Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.
