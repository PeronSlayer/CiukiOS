# f3-15 — DOOM freezes at the menu on the T23: protected-mode virtual IF stuck at 0 while the guest spins

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f3-15` from `main`. Host-testable (dosvm guest harness `tests/host/dosvm_guest*.c`, `dosvm_irq_stall_test.inc`, `dosvm_menu_test.inc`); the lead runs `f3-workloads` (doom cases) on QEMU and the T23 test.

## Evidence (T23 Pentium III-M 1.13 GHz, image fb3147dc, DOOM 1.9 shareware, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean`)

- For more than 25 s: `vm_beat_irq irr=0083 isr=0000 imr=ff78 irq0=2621 irq1=138` — IRQ0, IRQ1 and IRQ7 pending, all three unmasked, nothing in service, the IRQ0 delivery counter frozen at 2621; `vm_perf_pit edges=140` per second with `vm_perf_irq counts=0,0,0,0,0,0,0,0`; `vm_perf_cpu guest_us=794247 monitor_us=17709` (the guest runs 79 % of wall time), `vm_perf_video frames=0`; `[vm-audio] sb ... edges=1156 ack=1155 ... reason=irq_late` (the last SB IRQ was never acknowledged).
- `vm_heartbeat pm=1 cs=01df eip=00252998 … 00252a13 if=1 vif=0 arm=0 sh=0 depth=0`: a 32-bit DOS/4GW client loop at 01df:00252998-00252a13 spinning on a variable (DOOM's tic counter) while the monitor's virtual IF is 0, so no IRQ is ever delivered and the guest never advances.
- 65 `vm_if` records: `restore=0 matched=0 vif=0 armed=0 seed=1 seed_cs=01df seed_ip=00248205 | 0024821d | 002482da` (and once `seed_cs=00ff seed_ip=00000020`, the DOS/4GW IRQ stub). `dpmi_dispatch.c:845-852`: at a CLI (0xFA) the monitor arms `arm_flags_image` only if `vif` was 1 and clears `vif`; the matching restore (POPF/POPFD/IRETD image with IF=1, or STI) was never recognised, so `vif` stayed 0. The CPU runs the PM guest at IOPL 0 (`v86.c:1,220`): CLI/STI fault and are emulated, but POPF/POPFD/IRET at CPL 3 with IOPL < 3 leave IF unchanged **without any fault** (Intel SDM Vol. 2 POPF, Vol. 3 §20.3), so an IF restore through POPF cannot be seen by trapping. On real DOS (CWSDPMI, DOS/4GW raw mode) the client runs at IOPL 3 and POPFD restores IF for real; DOOM's Watcom runtime uses `pushfd/cli … popfd` around critical sections.
- `vm_perf_*` samples exist only from `n=66` on (first `vm_perf_video` at line 1779); the earlier gameplay seconds were never sampled on this log.

## References

Intel SDM Vol. 3 §20.3 (interrupt handling in V86), §20.4 (PVI: CR4.PVI makes CLI/STI operate on VIF at CPL 3, STI with VIP set faults; PVI does **not** affect POPF/IRET), Vol. 2 POPF/IRET/STI (IOPL rules, STI shadow); DPMI 0.9 and 1.0 specification (virtual interrupt flag, INT 31h 0900h-0902h, note that POPF/IRET may not restore the client's interrupt state under a virtualising host); DOSEMU `src/dosext/dpmi/dpmi.c` (how it handles `pushf/cli/popf` for DPMI clients); Windows 95 DDK VMM documentation on virtual interrupts (VPICD) for how a Win9x DOS box delivers after a POPF.

## Task

1. **Make the stuck state impossible.** When the PM guest has `vif=0` and an unmasked IRQ is pending, bound the wait deterministically: single-step the guest (TF; the monitor already has `vm_db … step=pm_return` stepping and the `return_step_safe` rules) until the IF-restoring instruction retires (POPF/POPFD/IRET/IRETD whose stack image has IF=1, or STI), then set `vif=1` and deliver. Decide from the references whether a bounded instruction/time budget (for example 4096 instructions or 2 ms, then treat IF as restored and record it) is needed as a last resort, and justify it. Fix the `arm_flags_image`/match logic so DOOM's three seeds match (the harness can execute the exact byte sequences; if the sequences are unknown, extract them with the guest CPU harness from the DOOM.EXE LE object in `third_party/games/doom-shareware` or from a traced run, and document them).
2. **Optional, only if cheap and runtime-detected:** CR4.PVI on CPUs that report VME (CPUID.1:EDX bit 1) so CLI/STI stop faulting; QEMU TCG lacks VME/PVI (records must say which path runs), so the trap path stays and the QEMU gates keep proving it.
3. **Watchdog record:** PM guest, unmasked IRR pending, `vif=0`, no delivery for 100 ms → one `vm_if_stall` record (cs:eip, vif, armed, seeds, IRR/ISR/IMR, 16 code bytes at eip, stack image) followed by the recovery of item 1.
4. **Perf samples from VM start:** make `vm_perf_*` sampling begin at the first second of the VM and stay bounded (why did it start at n=66?).

## Acceptance

Kernel host suite incl. `dosvm_irq_stall_test.inc`, `dosvm_menu_test.inc`, `dosvm_dpmi_test.inc` extended with the DOOM CLI/POPFD/IRETD sequences and the stall watchdog; the lead runs `doom-qemu-t23` and `doom-sound-qemu-t23`, then the T23: DOOM must run through demo, menu and game without freezing and its tic rate must be visible in the records. Files: `src/kernel/vm/dpmi_dispatch.c`, `v86.c`, `dpmi*.c`, `dosvm_perf.inc`, `dosvm_pic.c` records. Do not touch `dosvm_vga.c`/`virtual_vga.c`/`vga_presenter.c` (f3-16) nor the focus scheduling in `dosvm.c`/`task.c` (f3-17); the lead merges the three.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f3-15-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
