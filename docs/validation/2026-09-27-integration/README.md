# Integration audit evidence — 27 September 2026

These files are exact copies of the local records listed below. They preserve
the initial cross-track audit, scheduler instruction-level regression, and
peripheral namespace/combined-link correction, followed by the explicitly listed
runtime checks. Initial reports **predate** those corrections and are not
retroactively upgraded. See [the audit and scope](../../vm-integration-audit-2026-09-27.md).

| Archived file | Original local record | Meaning |
| --- | --- | --- |
| `lifetime-initial.json` | `build/tests/integration-audit-2026-09-27/lifetime/report.json` | Initial combined lifetime PASS with module `5e584d…`, HDPMI `ad1a8f83…` and qualified window payload |
| `input-initial.json` | `build/tests/integration-audit-2026-09-27/input-manifest.json` | Baseline, private input and kernel/shell/runtime identities |
| `invocation-initial.json` | `build/tests/integration-audit-2026-09-27/invocation.json` | Exact command, test/input/module-manifest hashes and preserved scope |
| `lifetime-r110.json` | `build/tests/dpmi-lifetime-run-r110/report.json` | Earlier lifecycle result with a different module, protected host and shell |
| `module-r110.json` | `build/full/vm-session/manifest.json` at audit time | Source hashes for the earlier `5de57c…` module |
| `module-video-session7.json` | `build/full/vga-video-2026-09-27/session7/manifest.json` | Source hashes for the video-track `5e584d…` module |
| `scheduler-before.json` | `build/tests/scheduler-guard-2026-09-27/before/report.json` | Expected failure of the unchanged pre-fix IRQ routine |
| `scheduler-corrected.json` | `build/tests/scheduler-guard-2026-09-27/current/report.json` | Four passing CPU checks of the corrected routine; no real IRQ-reentry claim |
| `device-link-before.json` | `build/tests/vm-device-link-2026-09-27/before/report.json` | Actual compiler failure from conflicting `cvp_` / `CVP_` declarations |
| `device-link-after.json` | `build/tests/vm-device-link-2026-09-27/after/report.json` | Combined video/presenter/peripheral host ASan/UBSan execution and freestanding OpenWatcom link PASS |
| `peripherals-renamed.json` | `build/tests/guest-peripherals-integration-2026-09-27/report.json` | 912 isolated model assertions PASS after the peripheral `cvgp_` / `CVGP_` rename; guest/runtime integration flags remain false |
| `cleanup-gate.json` | `build/tests/vm-integration-2026-09-27/cleanup-gate/report.json` | Scheduler guard + active-UNBIND refusal: COM/MZ/storage, negative lifecycle, framebuffer copy, unload and desktop return PASS on module `afb651…` |
| `module-cleanup.json` | `build/full/vm-integration-2026-09-27/session-cleanup/manifest.json` | Exact source/module identity for that cleanup gate |
| `vga-integration.json` | `build/tests/vm-integration-2026-09-27/vga-final/report.json` | VGA pixel comparisons, FIRE, Costa, rejected active-UNBIND on the real LFB binding, restoration and desktop return PASS on module `9c816e…`; Wolf memory limit retained |
| `module-integration.json` | `build/full/vm-integration-2026-09-27/session-final/manifest.json` | Inputs snapshotted before/after compilation, tool/include hashes and module `9c816e…` |
| `hdpmi-build-final.json`, `module-final.json`, `jemm-profile-build-final.json` | build manifests | Source/tool/output identities of the final HDPMI, session module and V86 IF profile |
| `final-lifetime-ordinary-jemm.json`, `final-lifetime-profile-{1,2,3}.json` | `build/tests/vm-completion-2026-09-27/{lifetime-ord2,pl5-*}` | Combined HDPMI lifetime PASS with the final HDPMI/module, ordinary Jemm and three V86 IF profile runs |
| `final-lifetime-{ordinary-jemm,profile}-manifest-binary.json` | `…/final-lt-{ord,prof}` | Lifetime PASS with the manifest-bound HDPMI rebuild `6a3f53a4…` (timestamp-only difference from `3af7ae37…`) |
| `final-v86cli-profile.json` | `…/v86cli-profile5` | V86 CLI/IRQ contract PASS with the final profile |
| `final-vga-{ordinary-jemm,profile}.json`, `final-window-{ordinary-jemm,profile}.json` | `…/final-vga-*`, `…/final-window-ordinary`, `…/window-profile2` | VGA acceptance and native DOS window PASS under both Jemm builds |
| `final-legacy-{v86,dpmi,v86-profile}.json` | `…/final-legacy-*` | Earlier session gates PASS with the final module |
| `dos-window-lifecycle-final.json` | `…/lifecycle` | 13 refused-cleanup lifecycle cases on the current SHELL/DOSWIN sources |
| `failure-*.json`, `failure-profile-eoi-ownership-ring.txt` | `…/<run>` named in the audit | Failures found during completion, each followed by its fix (see the audit's completion section) |

The initial lifetime report SHA-256 is
`9c4a18ba4fe75df2b54cd637acda8d792a7f384e962cde8d161ead5f03d59e24`.
Its `original_client` is the unchanged packaged **doom-vanille** DOS executable
`APPS/DOOMVAN/PCDMCORE.EXE`, not proprietary original Doom or a cooperative
CiukiOS window port. Reports are not edited to change their field names.

This archive contains JSON and documentation only. It includes no disk image,
executable, WAD, memory dump or proprietary application payload. Larger local
captures remain under the original ignored build paths. Source and invocation
records describe their qualification point, not future edits or rebuilt files.
