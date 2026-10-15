# f2-71 — CiukiPaint strokes are irregular (T23, image a4f67ac1)

- **Model/effort:** `gpt-6-luna`, `medium`. **Worktree:** `../CiukiOS-wt-f2-71` from `main`.

## Evidence

Owner: drawings in CiukiPaint are irregular, as if the pointer were sampled unevenly. Pointer motion arrives as discrete events (PS/2 at 100 Hz, USB at 125 Hz, coalesced per desktop turn since f2-65 for the demo); a pencil or brush that plots only the reported points leaves gaps when the pointer moves fast.

## Task

Every freehand tool (pencil, brush, eraser, spray, free-form select) draws the segment between the previous and the current pointer position (Bresenham for the pencil, stamped brush along the segment at the brush's spacing); verify that the desktop does not drop or coalesce motion events for clients that asked for raw motion (check `apps/desktop/input.c` and the client FIFO: coalescing must keep the last position per turn only when the client is behind, never while it keeps up); host test with a fast diagonal stroke asserting a continuous line.

## Acceptance

Paint tests and the desktop suite green; PNG before/after under `build/host/f2-71/`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f2-71-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
