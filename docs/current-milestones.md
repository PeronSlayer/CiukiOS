# Current CiukiOS milestones

**Updated 1 October 2026 · pre-Alpha 0.8.0**

This is the short phase ledger. The [project status](project-status-2026-10-01.md)
explains current features and limits; the [archived ledger](history/current-milestones-through-2026-10-01.md)
keeps dated kernel sizes, older application results and investigation notes.

## Current snapshot

| Milestone | Result |
| --- | --- |
| M1–M3 | Forked DOS VMs with private memory, sessions, input and audio passed their QEMU scope. |
| M4 | Desktop presentation, focus, close and Task Manager VM actions passed with DOOM and independent text VMs. [M4 record](validation/2026-09-30-m4/README.md). |
| Latest 0.8.0 image | 26/26 VM-window gates passed across serial recovery runs on one image after a host reboot. The single-command profile did not finish. [Exact combined result](validation/2026-10-01-native-app-gl/serial-summary.json). |
| M5 | Broader simultaneous protected-mode workload qualification remains open. |

Focused QEMU gates also pass held-stroke CiukPaint rendering, CiukWeb's first
HTTP page, a TinyGL software triangle and cursor movement during DOOM level
1. Earlier long-name, driver, registry, desktop-polish and network gates ran
on earlier integration images; they are not folded into the current 26/26
claim. Physical hardware is not qualified by these results.

## Phase status

| Phase | Current boundary |
| --- | --- |
| 0–5 | Foundation, boot, kernel, desktop/graphics, installer MVP, DOOM gameplay and runtime ownership closed within their recorded original scope. |
| 6 — DOS apps | Active. The required ten-program external corpus, five-category full-CD matrix and wider file/process semantics remain open. |
| 7 — legacy audio | Not closed. Bounded SB16/OPL paths pass; original Doom SB16/DMX, more external workloads, full-CD and physical tests remain. |
| 8 — networking | First bounded packet/IPv4/FTP milestone passed. Wider hardware, encrypted protocols and physical networking remain. |
| 9 — Windows | Historical Windows 3.1 workflow was removed from the current image. Windows 95/98 PE execution and installers are unsupported. |
| 10 — release | Active. 0.8.0 is a development snapshot, without a general compatibility or physical-PC release claim. |

The [roadmap](../Roadmap.md) gives the next implementation order. A narrow
PASS does not close a broader phase.

## Runtime and compatibility rules

- `CIUKIDOS.SYS` owns the DOS kernel; the build enforces a 43,264-byte
  maximum. New features belong in modules or resident extensions when they
  do not fit the kernel's physical memory window.
- Desktop `.APP` modules have a 64 KiB code/data/stack ceiling. Files is
  close to that limit. [Native app formats](native-apps-and-opengl-2026-10-01.md).
- Mouse, memory allocation, DOS EXEC and window focus should use general
  interfaces rather than rules for named games.
- Commercial DOS game data and Windows media stay local and untracked.
  Missing payloads must be reported as skipped, not passed.
- Windows 95/98 `.EXE` recognition is not execution. The [free PE probes](windows-compatibility-and-layout-2026-09-30.md)
  define the first future loader/API milestone.

## Validation and release work

Build and focused-test commands are in [Build and run](build-and-run.md).
The current VM-window evidence is in the
[1 October record](validation/2026-10-01-native-app-gl/README.md).
Before a release claim, run a resource-bounded regression on the intended
image, audit redistributed assets and test actual legacy PCs. Keep `full`,
`full-cd` and optional local-game results distinct.
