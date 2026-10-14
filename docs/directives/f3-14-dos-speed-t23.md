# f3-14 — DOS games very slow on the T23: read the f3-13 performance records and remove the cost

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-14` from `main`. Host-testable; the lead measures on the T23.

## Evidence (T23 Pentium III-M 1.13 GHz, image 8c88b1ab, DOOM 1.9 shareware, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/8c88b1ab-garbled-usb-regression/disk-log/BOOT.LOG`; f3-13 records `vm_perf_irq`, `vm_perf_v86` sampled every window, extracted to `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/t23-vm-perf-8c88b1ab.txt`)

Per sampling window (about two seconds): `vm_perf_v86 cause=0 exits=411, cause=1 exits=203, cause=2 11, cause=3 11, cause=4 23, cause=5 319` and `vm_perf_irq counts=111,2,0,0,0,0,0,80` (IRQ0 111, IRQ1 2, IRQ7 80). The owner reports DOOM and Wolfenstein 3D "very slow" (f3-13's targets were 35 and 70 fps). The desktop presents small damage at 9 ms per frame with p95 17 ms, so the window presenter is not the bottleneck at these rates; the VM exits and what they cost are.

## Task

1. Decode the f3-13 records (`src/kernel/vm/dosvm_perf.inc`): what each `cause` is (port I/O trap, VGA memory fault, PIT/PIC, DPMI switch, page fault on the chained window, HLT, …) and the per-exit cost on this CPU; compute from the windows above the time the VM spends in the monitor versus running, and name the dominant cause. If the records lack the time per cause, add it (TSC-based, verbose only) with a bounded table, and add the frames harvested per second and the VM run share.
2. Remove the dominant costs: VGA mode 13h must run through the direct linear window with zero traps in steady state (f3-07/f3-13), palette and sequencer writes batched, PIT/PIC emulation fast-pathed (IRQ0 at 140 Hz is 280 exits per 2 s alone, plus DPMI reflection), keyboard IRQ1 cheap, the DPMI real/protected switches not forced by diagnostics, the rm_ss single-stepping of real-mode STI shadows (41 `vm_db` records per boot) limited to what correctness needs; the VM's time share when its window is focused must be ≥ 80 % of the CPU.
3. Host replay with the DOOM startup sequence counting exits per cause before and after; targets on the T23: DOOM ≥ 35 fps, Wolf3D ≥ 70 fps, measured by the records.

## Acceptance

`f3-workloads` and kernel host suites green; the lead measures on the T23.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f3-14-report.md`.
