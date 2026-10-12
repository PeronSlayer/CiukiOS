# f3-09 — DOOM stops at I_StartupTimer on the T23 (Pentium III): analysis, diagnostics and fix candidates

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-09` (branch `wt/f3-09` from `main`). Host-testable; the lead reproduces on the T23 with the serial log (`L` boot).

## Evidence (2026-10-12, image 58c835bd / hardware image 569b3fca)

On the ThinkPad T23 (Pentium III-M 1.13 GHz, CPUID 6B1) DOOM 1.9 shareware launched from Programs > Games prints its startup up to `I_StartupTimer()` and then stays there (cursor, no mode 13h switch). The quiet-boot serial log shows nothing after the desktop spawn. On QEMU (pentium3, TCG) the same image completes five timedemo loops (`f3-workloads` doom cases PASS on the previous images of the day). Wolfenstein 3D was not tried yet. Before today's DPMI changes DOOM had not been run on the T23.

`I_StartupTimer` (DOOM `i_ibm.c`) installs the IRQ0 handler through the DPMI host (set protected-mode interrupt vector 8 / real-mode vector 8), programs the PIT (ports 40h/43h) for 140 Hz, and returns; the next step is `I_InitGraphics` (mode 13h).

## Task

1. Static analysis of the kernel path used by that sequence on a real Pentium III versus TCG: `src/kernel/vm/dpmi_dispatch.c` (vectors, virtual IF, the DR6.BS/TF trap recognition added by f3-08), `src/kernel/vm/dpmi.c`, the PIT/PIC virtualisation for VMs (`src/kernel/vm/*pit*`, `*pic*`, IRQ0 delivery to a PM client), `src/kernel/vm/dosvm.c` #DB handling (`dosvm.c` ~240: DR6 read; on real CPUs DR6 is sticky and is never cleared by hardware, TCG may differ: check that the kernel clears DR6 after each #DB and that stale BS/B0-B3 bits cannot misroute the trap), MOV/POP SS delayed #DB, IF/TF interplay on IRET to V86/PM, and the host timer IRQ sharing between the kernel scheduler and the VM's virtual PIT. Cite the Intel SDM sections.
2. Produce a ranked list of causes with, for each, the exact check (a serial record or a host test). Implement the ones that are defects with host tests (dosvm host harness, `dosvm_guest` replay).
3. Diagnostics for the next hardware test: in verbose boot (`L`) the kernel emits bounded records for DPMI vector installs, PIT programming, the first ten IRQ0 deliveries to the client (mode, CS:EIP, IF state), #DB traps (DR6, TF, IF, armed state) and VM faults with EIP, so one serial capture explains the stop. Keep the records off in quiet boot and within the record grammar.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`; keep `f3-workloads` host replays green. Report to `build/codex/f3-09-report.md`: ranked causes, fixes with file:line, host test summaries, and the serial lines the lead must look for on the T23.
