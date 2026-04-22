# Copilot Task Pack - Third Agent CIUKEDIT Final Polish 001

Baseline: CiukiOS Alpha v0.8.7. Do NOT touch `main` directly.

## Goal

Deliver a final user-facing polish pass on three visible runtime surfaces:

1. make `CIUKEDIT.COM` feel definitively usable rather than still transitional
2. remove user-visible debug/noise lines that appear when launching `.COM` and `.EXE` programs from the shell
3. make the shell title/header show the current CiukiOS version, with a white background and black text

This is product-surface cleanup, not a broad runtime redesign.

## Context

CiukiOS now has a presentable shell, graphics demos, and direct DOS-like program launch flow, but a few visible surfaces still feel like bring-up/debug output instead of a clean product:

1. `CIUKEDIT.COM` still needs a stronger final pass on UX/polish.
2. Running a `.COM` or `.EXE` still shows noisy user-facing lines such as launch/runtime diagnostics that should remain serial-visible when useful, but should not clutter the visible shell surface.
3. The shell top bar/title currently does not present the current version the way the user wants.

## Read First

1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/sr-edit-001.md`
5. `docs/copilot-task-sr-edit-001.md`
6. `com/ciukedit/ciukedit.c`
7. `stage2/src/shell.c`
8. `stage2/src/ui.c`
9. `stage2/src/stage2.c`
10. `stage2/include/version.h`
11. relevant existing handoffs in `docs/handoffs/` for `CIUKEDIT`, shell UX, and OT-DEMO work

Before implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on this same area. If occupied, stop and choose a different section instead of overlapping.

## Scope

### A) CIUKEDIT final polish

Improve `CIUKEDIT.COM` so it feels like a finished first-party CiukiOS tool rather than a provisional line editor.

Possible improvement areas include, if they fit the existing architecture:

1. stronger and cleaner header/help/banner behavior
2. less noisy internal markers on the visible output
3. clearer success/error text for open/save flows
4. better command/help discoverability for normal editing use
5. any small UX cleanup that materially improves perceived completeness without turning it into a full-screen editor or redesigning the DOS ABI

Do NOT broaden into a whole new editor architecture. Stay line-oriented unless a very small focused change is justified.

### B) Remove visible shell launch debug noise

When the user launches a `.COM` or `.EXE`, visible shell output should no longer show bring-up/debug-style lines such as launch diagnostics unless they are truly user-facing and necessary.

Important distinction:

1. preserve deterministic serial/runtime markers that are still valuable for tests and validation when practical
2. reduce or remove the user-visible framebuffer/shell text noise that currently appears during ordinary program launch

The result should feel cleaner for normal shell usage and for video capture.

### C) Shell title bar version + colors

Update the shell title/header so:

1. it shows the current CiukiOS version
2. the bar/background is white
3. the text is black

Do this coherently across the actual shell surface the user sees, not just in one unrelated boot-only code path.

## Hard Requirements

1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the task scoped to `CIUKEDIT` polish, visible shell launch-noise cleanup, and shell title/header version styling.
4. Do not remove deterministic serial markers just to hide output; prefer separating serial validation from visible shell noise.
5. Do not break the existing DOS run path, shell command flow, or current graphics/runtime behavior.
6. Update `docs/collab/diario-di-bordo.md` when the task is done, and do not add that file to Git.
7. Add a handoff file: `docs/handoffs/YYYY-MM-DD-third-agent-ciukedit-final-polish-001.md`

## Validation Required

1. `make all`
2. `make test-stage2`
3. `bash scripts/test_ciukedit_smoke.sh` if still applicable, or an updated equivalent if you intentionally revise the validation path
4. any direct validation added for launch-noise cleanup or shell-title rendering

If runtime capture remains host-limited, preserve a deterministic static/source fallback similar to the project’s other gates.

## Deliverables

1. Working code on a dedicated branch
2. Improved `CIUKEDIT.COM` user-facing polish
3. Cleaner visible shell program-launch surface for `.COM` / `.EXE`
4. Shell title/header showing current version with white background and black text
5. Minimal docs/test updates required to support the change
6. A handoff file documenting scope, decisions, tests, risks, and next step

## Final Handoff Must Explicitly Report

1. what changed in `CIUKEDIT.COM` and why it is now more final/user-facing
2. which visible launch/debug lines were removed, hidden, or reclassified to serial-only behavior
3. exactly where and how the shell title/header now shows the current version
4. tests run and their status
5. any remaining rough edges before the shell/editor feel fully polished