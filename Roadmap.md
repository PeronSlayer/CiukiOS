# CiukiOS roadmap

**Current build: pre-Alpha 0.8.0.** This page tracks the work ahead. The
[project status](docs/project-status-2026-10-01.md) explains what works now;
the [historical roadmap](docs/history/roadmap-through-2026-10-01.md) keeps
older phase details and debugging notes.

## Where the project stands

| Track | State | Next proof needed |
| --- | --- | --- |
| Native DOS kernel and desktop | Working in the main FAT16 image. | Broader external-app regression and physical-PC trials. |
| M4: a VM per DOS window | Complete for the tested QEMU scope; DOOM and two text windows pass. | Fix Wolf4GW's mouse/close defect without regressing other VMs. |
| M5: protected-mode concurrency | Open. | Multiple demanding DPMI workloads, independent lifecycle and bounded close. |
| Native desktop apps | System `.APP` modules work. | Installable third-party module manifest, loader and SDK. |
| Network and browser | Packet networking, IPv4/DHCP, FTP and CiukWeb's first HTTP view. | Native TCP service, then HTTPS and richer pages. |
| Graphics | VBE desktop and TinyGL software subset. | Larger framebuffer API, named free-app compatibility and later GPU research. |
| Windows 95/98 | Registry groundwork and PE detection only. | Free PE probes running through loader and Win32 APIs. |
| Release and hardware | 0.8.0 is a development snapshot. | Full bounded regressions, license audit and real-PC evidence. |

M4's earlier automated 0.8.0 profile passed 26/26 QEMU gates. On the newer
CiukWeb/TinyGL image, 26/26 gates passed across serial recovery runs after a
host reboot; this is not one uninterrupted profile invocation. The
[validation record](docs/validation/2026-10-01-native-app-gl/README.md)
keeps the exact scope. QEMU runs on the host, not inside CiukiOS.

## Next milestones

1. **Stabilize DOS windows and M5.** Reproduce and fix Wolf4GW input and
   close. Test multiple protected-mode guests with video, sound, focus and
   clean exit. Continue Phase 6's external DOS corpus and full-CD matrix;
   Phase 7 audio compatibility remains open.
2. **Make native apps installable.** Freeze a C/OpenWatcom `.APP` contract,
   publish a small SDK and independently built sample, then add a module
   loader that does not require editing the system build.
3. **Grow CiukWeb carefully.** Move HTTP transfer from `HTGET.EXE` into a
   native TCP service. Add history, cache and bounded parser tests before
   TLS/HTTPS, images, CSS or scripts.
4. **Approach Win32 in stages.** Run the free `HELLO.EXE` probe with a PE32
   loader and `KERNEL32`; then run `SETUP.EXE` with file and registry APIs.
   Add GUI, installer and game APIs only against named tests. Recognition of
   an EXE is not execution. [Compatibility plan](docs/windows-compatibility-and-layout-2026-09-30.md).
5. **Qualify graphics, devices and a release.** Extend the software graphics
   presentation path, license and test more drivers, address CD/USB support,
   migrate old root directories safely, and validate on physical legacy PCs.

## Phase ledger

| Phase | Current boundary |
| --- | --- |
| 0–5 | Foundation, boot, native DOS runtime, DOS graphics, installer MVP, DOOM gameplay and kernel ownership: historical milestones closed within their original scope. |
| 6 | DOS application compatibility: active; corpus and full-CD matrix incomplete. |
| 7 | Legacy audio compatibility: groundwork and bounded games pass; broader external-app and real-hardware qualification open. |
| 8 | Legacy networking: bounded packet/IPv4/FTP milestone passed; wider devices and encrypted protocols open. |
| 9 | Windows 3.1 workflow was removed from the current image; Windows 95/98 execution has not begun. |
| 10 | Build/release discipline: active; 0.8.0 is not a finished release. |

A phase is complete only when every required scope item has a fresh result
on the relevant image. A QEMU pass does not qualify a physical PC, and a
narrow workload pass does not imply general DOS or Windows compatibility.

## Design and evidence

- [M4/M5 architecture](docs/roadmap-dos-vm-desktop-2026-09-28.md) and
  [current-image QEMU evidence](docs/validation/2026-10-01-native-app-gl/README.md).
- [Native app and software OpenGL plan](docs/native-apps-and-opengl-2026-10-01.md).
- [Build and run guide](docs/build-and-run.md), [DOS compatibility matrix](docs/dos-compatibility-matrix-v0.1.md)
  and [legacy audio plan](docs/legacy-audio-bring-up-plan-v0.1.md).
- [Archived detailed roadmap](docs/history/roadmap-through-2026-10-01.md)
  for dates, earlier decisions and closure evidence.
