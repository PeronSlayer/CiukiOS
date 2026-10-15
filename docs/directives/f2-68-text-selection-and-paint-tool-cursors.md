# f2-68 — Desktop refinement: text selection with highlighting everywhere, paint tool cursors (owner request 2026-10-15)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-68` from `main`. Host-testable; the lead reviews PNGs and the T23. Coordinate with f2-67 only through the cursor request message it adds (if your worktree needs it before f2-67 lands, add the same message with the same name and the lead merges).

## Task

1. **Audit first.** Append to `docs/design/desktop-interaction-audit.md` (or write it if f2-67 has not yet) the state of every text control (Notepad, text fields of dialogs and the control panel, list boxes, the file manager's rename field, the terminal/console view) and of every paint tool: ok / wrong / missing. Deliver as milestone A within the first hour.
2. **Text selection.** Mouse drag, double-click word, triple-click line, Shift+arrows, Shift+Home/End, Ctrl+A; highlighted with the theme's selection colours (inverted text), caret hidden inside a selection; copy, cut, paste and delete through the clipboard service; replace-on-type; works in every text control of the toolkit, including single-line fields and list boxes (item selection with highlight).
3. **Paint program.** The cursor becomes the selected tool over the canvas: pencil, brush (size preview), fill bucket, colour picker, text I-beam, rectangular and free-form select (crosshair), zoom (magnifier), eraser (square); arrow outside the canvas; hot spots exact.
4. Goldens and PNGs under `build/host/f2-68/`.

## Acceptance

Audit section delivered; desktop host suites and gate green with re-recorded goldens; PNGs for the lead; the owner judges on the T23. Files: `apps/libdeskui/*` (text controls), `apps/notepad`, `apps/paint` (CiukiPaint), `apps/control`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f2-68-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
