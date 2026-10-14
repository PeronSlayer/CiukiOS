# f3-17 — A DOS window slows the whole desktop: Windows 9x-style idle detection for DOS VMs

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f3-17` from `main`. Host-testable (scheduler harness `tests/host/proc`, `test_signal_fault_records.py` extracts `sched_tick`, dosvm host tests); the lead measures on QEMU and the T23.

## Evidence (T23, image fb3147dc)

Owner, 2026-10-14: "when CiukiDOS starts in the desktop everything begins to go slower". f3-14 reserves 8 of 10 ticks for the focused DOS window (`task.c rq_pop`, `dosvm_task_quantum`/`dosvm_task_focus` in `dosvm.c:441-450`) regardless of what the guest does; `vm_perf_share vm_permille=811 guest_permille=794` even while the guest only spins (f3-15). A DOS prompt that polls the keyboard is "busy" to our scheduler. Owner mandate: DOS applications must open and run as seamlessly as in Windows 98/ME.

## How Windows 9x did it (verify and cite)

The Win9x VMM runs a DOS box only while it is not idle: idle is detected from INT 2Fh AX=1680h ("release current virtual machine time slice", issued by MS-DOS 7 `COMMAND.COM`, `DOSIDLE`, Windows-aware programs), INT 28h (DOS idle interrupt from the INT 21h input functions), repeated INT 16h AH=00/01/10/11 keyboard polling without a key ("Idle Sensitivity" in the PIF settings), and HLT; an idle VM is suspended until an event for it (keyboard, timer tick it has armed, IRQ for its devices). Background DOS boxes keep a smaller share (foreground/background priority in the PIF). References: Windows 95 DDK VMM documentation (`Release_Time_Slice`, `Call_When_Idle`, `Begin_Critical_Section`, VPICD), Microsoft KB on "Idle Sensitivity" and INT 2F 1680, Ralf Brown's Interrupt List INT 2F/AX=1680h and INT 28h, MS-DOS 6/7 `COMMAND.COM` idle behaviour, DOSBox `dos.cpp`/`callback` idle handling for comparison. Our DOS personality already answers INT 2F/1680 (`src/boot/floppy_stage1.asm` `.fn_16xx_idle`, around line 23339; use `rg`/semble, do not read the whole file).

## Task

1. **Monitor-side idle detection** (`dosvm.c` and the V86 reflection points): INT 2F/1680, INT 28h, INT 16h AH=01/11 returning no key more than N times within a tick, INT 16h AH=00/10 blocking waits, HLT with IF=1 → the VM task blocks until the next event that can change its state (keyboard/mouse input, a PIT edge the guest armed, an IRQ for its emulated devices, a presentation request), with a maximum sleep bounded by the guest's PIT rate. Busy guests (DOOM) are untouched.
2. **Focus budget only for non-idle VMs:** `dosvm_task_focus()`/`dosvm_task_quantum()` give the 8/10 reservation only to a VM that was not idle in the last window; an idle VM costs < 3 % of a core. Per-VM counters: `idle_blocks`, `idle_reasons` (2f1680/28h/16h/hlt), `busy_permille`, published in `vm_perf_share`.
3. **CiukiDOS:** its command prompt and INT 21h input functions (0Ah, 01h, 07h, 08h, 0Ch) and INT 16h waits issue INT 28h and INT 2F/1680 and `HLT` with IF=1 while waiting, like MS-DOS 7; check `.fn_16xx_idle` and the keyboard wait loops. Build the DOS personality with the existing scripts and prove with a host test (the dosvm guest harness boots the DOS image?) or a static check of the generated code.
4. **Targets** (records): a CiukiDOS window at the prompt → `vm_permille < 30`, desktop frame p95 unchanged with the window open; DOOM running → its share as before.

## Acceptance

Scheduler and dosvm host suites green (`test_signal_fault_records.py` keeps compiling the extracted `sched_tick`; update its stubs if `rq_pop`/`sched_tick` change); the lead runs `f3-vm` + `vm-starve-qemu-t23` + `desktop-shell` on QEMU, then the T23: open CiukiDOS, the desktop stays fluid. Files: `src/kernel/vm/dosvm.c` (idle/focus), `src/kernel/core/task.c` (focus gate), `src/boot/floppy_stage1.asm` (idle calls), `dosvm_perf.inc` counters. Do not touch `dpmi_dispatch.c`/`v86.c` (f3-15) nor `dosvm_vga.c`/`virtual_vga.c` (f3-16).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f3-17-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
