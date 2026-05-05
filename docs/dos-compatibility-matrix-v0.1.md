# DOS Compatibility Matrix v0.1

## Objective
Track repeatable DOS application evidence after the validated five-service runtime tranche without overstating current compatibility breadth.

## Evidence Rules
1. A PASS requires a repeatable validation lane or a dedicated validation bundle.
2. The new DOS compatibility smoke lane is currently full-only; it does not yet prove arbitrary application support from the full-CD D: environment.
3. Full-CD currently has stronger evidence for shell and setup flows than for arbitrary DOS applications.
4. Internal CiukiOS programs are useful proxies, but they do not count as proof of broad third-party compatibility by themselves.

## Status Legend
- PASS: repeatable evidence exists for the current scoped workload.
- PARTIAL: a real compatibility boundary is reached, but the full workload is still blocked.
- PASS (internal proxy): the workload is stable and repeatable, but the program is an internal CiukiOS proxy rather than an external DOS application.

## Current Matrix
| Workload | Category | Status | Scope | Evidence | Current blockers / notes |
|---|---|---|---|---|---|
| DRVLOAD.COM | Driver/helper compatibility | PARTIAL | Full helper lane | `make qemu-test-full-drvload-smoke` is green, and the optional DEVLOAD evidence path reaches native `QCDROM.SYS` INIT and device-detection boundaries. | MSCDEX still blocks at child exit `0x11` until DOS List-of-Lists, CDS, device-chain, and device-handle compatibility are stronger. |
| CIUKEDIT.COM | Text editor proxy | PASS (internal proxy) | Full-only DOS compatibility smoke lane | `make qemu-test-full-dos-compat-smoke` passes by launching `run \APPS\CIUKEDIT.COM MATRIX.TXT`, waiting for `[CIUKEDIT:BOOT]`, collecting one input line, waiting for `[CIUKEDIT:OK]`, and verifying prompt return. | Internal bundled editor; validates argv, console I/O, prompt recovery, and return path, but does not yet prove external editor breadth. |
| GFXSTAR | Graphics/demo proxy | PASS (internal proxy) | Full-only DOS compatibility smoke lane | The same lane then runs the built-in `gfxstar` command, waits for `[GFXSTAR-SERIAL] PASS`, and verifies prompt return. | Internal built-in command path only; useful as a graphics/command proxy, but not evidence for arbitrary third-party graphics software. |
| DOOM.EXE | DOS extender game | PASS | Full profile with local DOOM payload present | The validated service-5 bundle includes `env DOOM_TAXONOMY_MIN_STAGE=runtime_stable make qemu-test-full-doom-taxonomy` PASS, and the wider active baseline already records stronger DOOM gameplay evidence. | Legacy audio is still an open milestone; current evidence says extender/video/gameplay paths are strong enough, not that audio is working. |
| SETUP.COM | Setup/install utility | PASS | Strongest on full-CD live/install and dedicated setup lanes | Full-CD shell/setup evidence remains strong through `make qemu-test-full-cd`, `make qemu-test-full-cd-shell-drive`, and `make qemu-test-setup-runtime-hdd-install`. | This is better evidence for setup and shell behavior on full-CD than for arbitrary DOS app launch from D:; a dedicated full-CD arbitrary-app lane is still missing. |

## Current Interpretation
1. The new DOS compatibility smoke lane is a real improvement because it validates one argument-taking internal COM program plus one built-in graphics command end to end.
2. The lane is still conservative because both workloads are internal proxies and because it covers the full profile only.
3. Full-CD is currently better evidenced for live shell behavior and setup/install flows than for arbitrary DOS applications copied onto the media.

## Primary Blocker Classes
1. Driver/device-stack gaps still block broader CD driver and MSCDEX compatibility.
2. The matrix still needs at least one external real-mode utility or editor to reduce dependence on internal proxies.
3. A dedicated full-CD arbitrary-application smoke path does not exist yet.
4. Audio detection, initialization, and playback remain separate follow-up work rather than completed compatibility proof.

## Immediate Next Expansion
Add one external real-mode DOS utility or editor to the full-only smoke lane, keep the current CIUKEDIT and GFXSTAR proxy checks, and only then decide whether the next matrix gap is broader full-CD app launch coverage or a second external application category.