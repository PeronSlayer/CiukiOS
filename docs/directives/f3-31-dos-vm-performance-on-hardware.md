# f3-31 — DOS VM performance on the T23: Quake crawls, DOOM lags (image a395ffa8); complete diagnosis and fixes

- **Model/effort:** `gpt-6-astra`, `xhigh` (owner's explicit request for a deep diagnosis; Sol has not solved this in three rounds). **Worktree:** `../CiukiOS-wt-f3-31` from `main`.

## Evidence

- Owner: Quake now starts (f3-25, f3-28) but runs far below playable; DOOM loads faster since f3-24 but still lags in the window; both on a 1 GHz Pentium III-M with 512 MiB.
- `disk-BOOT.LOG` (Quake session, VM gen 96273): `vm_beat_vga_mem n=1…53 harvests 35→3197 dirty_pages 532→50718 harvest_us cumulative 2.14 s over 53 s`: about 1000 dirty VGA pages per second harvested by the presenter (4 % CPU); no `vm_perf_cost`/`vm_perf_share` records in the disk log (they are behind the diagnostic flag and the owner booted normally); the earlier T23 log of image `a738204a` measured `rm_int 420/s at 1.18 ms each` and `guest_permille=1` during Quake's start; `a4f67ac1`'s Quake stayed in text mode. Research notes: `docs/design/research/dos-vm-video-2026-10-14.md` (DOOM: ~1279 VGA port-write traps per frame with 16-page remaps per plane switch; f3-19 added the batch page-table swap and the Mode X/Y presenter), `docs/design/dos-dpmi-contract.md`, the f3-15/f3-19/f3-23/f3-24 validation notes.

## Task

1. **Measure before changing.** Make the cost breakdown available on every hardware boot at low rate (one `vm_perf_*` summary every 5 s, not per second, always on): guest vs monitor share, exits per cause (port I/O, page remaps, software interrupts, hardware interrupts, timer, DPMI calls, VGA harvest, presenter), the top five ports and vectors by cost, DMA/audio service cost. Replay the Quake and DOOM startup and gameplay on the host harness with the real images to obtain the same breakdown on the host; cross-check with QEMU's `quake-qemu-t23`/`doom` runs.
2. **Diagnose the architecture against the reference**: a Windows 9x-class VM on this CPU runs DOS games near native speed because real mode I/O to VGA is executed directly (ports granted, A000 aperture mapped) and only exceptional events trap; measure which of our traps are avoidable: VGA port writes (grant 3C0–3DF directly to the windowed VM when the presenter can read the state back, like the full-screen path grants them), A000 window writes (map the aperture directly to a shadow and harvest by dirty bits instead of page faults), PIT/PIC port I/O (VME-style virtual interrupt flag, timer reads), INT 10h/16h/21h paths that go through the storage task, DPMI calls, keyboard polling. Quantify each with the measured counts.
3. **Implement the fixes with the largest measured gain** that keep the window model safe (the presenter still owns the screen; the VM never touches the real VGA outside full screen): direct VGA port grants with register shadowing, direct aperture with dirty-bit harvesting, batch INT services, VIF handling; keep the existing probes and QEMU cases green and add host benchmarks with numbers in the report.
4. Targets on the T23 (the owner judges): Quake demo at 640×480 playable (above 15 fps measured by the presenter's harvest rate and the guest's own counters), DOOM window above 25 fps.

## Acceptance

Host benchmarks before/after in the report; kernel host suite green; QEMU `f3-vm`, `f3-workloads` dos4gw and quake (with f3-30's timing) green; the next T23 log carries the 5 s summaries and the owner sees the difference.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Build the kernel (`python3 scripts/build_kernel.py`) and the DOS image (`python3 scripts/build_dos_image.py`) before the report. Evidence (read-only): `legacy/local/physical/a395ffa8/` (`disk-BOOT.LOG` latest normal boot with the `[blackbox]` head of the previous boot, `disk-BOOT1.LOG` first boot, `disk-BOOT2.LOG` the Hardware check boot, `serial.log` corrupted). Owner's standing rule: every hardware decision is grounded in the supplied driver package (`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/` with the INF/registry tables of the vendor drivers, `third_party/drivers/` DOS drivers) and in upstream open sources found on the internet (X.Org, Linux, FreeBSD, SeaBIOS, vendor datasheets); cite them in the report; never program registers from reasoning alone. Report `build/codex/f3-31-report.md`: root causes (file:line), fixes, verbatim test output, what the lead must verify on the H page and on the T23.
