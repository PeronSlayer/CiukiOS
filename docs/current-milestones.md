# Current CiukiOS milestones

**Updated 4 October 2026 · pre-Alpha 0.8.3**

This is the short phase ledger. The [project status](project-status-2026-10-04.md)
explains current features and limits; the [archived ledger](history/current-milestones-through-2026-10-01.md)
keeps dated kernel sizes, older application results and investigation notes.

## Current snapshot

| Milestone | Result |
| --- | --- |
| M1–M3 | Forked DOS VMs with private memory, sessions, input and audio passed their QEMU scope. |
| M4 | Desktop presentation, focus, close and Task Manager VM actions passed with DOOM and independent text VMs. [M4 record](validation/2026-09-30-m4/README.md). |
| 0.8.0 historical image | 26/26 VM-window gates passed across serial recovery runs after a host reboot; this was not one uninterrupted run. [Exact combined result](validation/2026-10-01-native-app-gl/serial-summary.json). |
| 0.8.3 scoped runtime gate | PASS for the recorded build. DOOM ran at 34.78 loops/s and exited with GPU resources released; Files wheel input round-tripped by pixel; native CiukWeb completed HTTP 200 and reload; 32-bit preview timed out and reverted, 16-bit preview reverted on Escape, and 640×480 was accepted. A later focused check verified About's default-show preference across reset and title focus repair. |
| CiukWeb HTML, styles and scripts | Basic HTML/GET forms and PNG/GIF/baseline-JPEG XMS images remain available. Bounded CSS, a BearSSL TLS 1.2 worker and an mQuickJS classic-script/DOM worker now compile; JS and page-adapter host checks pass. End-to-end HTTPS/CSS/JS browser runtime validation is pending. Limits include 128-character URLs, 24 images, 6,144 parser nodes, 12 MiB decoded RGB, a 1 MiB page, 16 scripts and a 32-element text snapshot. No full CSS/DOM or IE equivalence. |
| AC'97 latency | Queue changed to eight 256-frame descriptors (~46 ms at 44.1 kHz, versus ~186 ms before). Doom playback and the PCM capture passed; no new underruns were recorded after an initial diagnostic sample. Host speaker latency/listening remains unqualified. |
| WebNet | Native browser networking passed DNS resolution, HTTP 200 with a 577-byte response body, wrapped page rendering and Ctrl+R reload in the tested Linux QEMU/KVM NE2000 PCI NAT profile. See the [runtime record](validation/2026-10-04-web-network.md). |
| M5 | Broader simultaneous protected-mode workload qualification remains open. |

Earlier focused QEMU gates passed held-stroke CiukPaint rendering, CiukWeb's
first HTTP page, a TinyGL software triangle and cursor movement during DOOM
level 1. The 0.8.3 scoped runtime evidence is recorded in the validation
documents linked below. Earlier long-name, driver, registry, desktop-polish
and network gates ran on earlier integration images; they are not folded into
the 0.8.0 VM count. Physical hardware is not qualified by these results.

## Phase status

| Phase | Current boundary |
| --- | --- |
| 0–5 | Foundation, boot, kernel, desktop/graphics, installer MVP, DOOM gameplay and runtime ownership closed within their recorded original scope. |
| 6 — DOS apps | Active. The required ten-program external corpus, five-category full-CD matrix and wider file/process semantics remain open. |
| 7 — legacy audio | Not closed. Bounded SB16/OPL paths pass; original Doom SB16/DMX, more external workloads, full-CD and physical tests remain. |
| 8 — networking | Bounded packet/IPv4/FTP and native WebNet DNS/HTTP browser milestones passed in their recorded QEMU scopes. Basic HTML/images and bounded CSS, TLS 1.2, and JavaScript/DOM worker code are implemented; browser-level HTTPS/CSS/JS runtime validation, wider hardware and physical networking remain open. TLS 1.3, full CSS/DOM, and IE equivalence are not claimed; Adam7 PNG and progressive JPEG are unsupported, and GIF uses its first frame. |
| 9 — Windows | Historical Windows 3.1 workflow was removed from the current image. Windows 95/98 PE execution and installers are unsupported. |
| 10 — release | Active. The scoped 0.8.3 runtime gate passed; broader regressions and physical-PC qualification remain before any general compatibility claim. |

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
Previous VM-window evidence is in the
[1 October record](validation/2026-10-01-native-app-gl/README.md).
The current 0.8.3 feature and gate state is in the
[project status](project-status-2026-10-04.md), and the native browser result
is in the [WebNet runtime record](validation/2026-10-04-web-network.md). The
final graphics sample measured a 30.87 ms 95th-percentile GPU submission interval,
67.73 ms maximum, and two gaps above 42 ms over 9.9 seconds; host CPU-quota
throttling was zero. These are software intervals, not monitor FPS.
For broader qualification, run resource-bounded regressions,
audit redistributed assets and test actual legacy PCs. Keep `full`,
`full-cd` and optional local-game results distinct.

The latest browser integration compiled in the full build; browser HTTPS/CSS/JS
runtime validation is still pending. About startup/focus and AC'97 queue
behavior have focused runtime evidence in the [About startup/focus note](validation/2026-10-04-about-startup-focus.md)
and [AC'97 queue note](validation/2026-10-04-ac97-audio-latency.md). See also
the [image decoder design](validation/2026-10-04-webimg-codec.md).
