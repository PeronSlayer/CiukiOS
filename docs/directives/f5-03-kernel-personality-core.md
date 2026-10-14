# f5-03 — Kernel personality core for Win32: frozen record header first, then personality, address space, FS/TEB, FPU state, exceptions, counters

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f5-03` from `main`. Host-testable; the lead runs the F0-F4 gates and new QEMU cases.

## Input

`docs/design/win32-personality-contract.md`: sections "Personality architecture", "Process, thread and object model" (process creation, threads/TEB/FS), "Memory" (address space, virtual-memory service), "Exceptions and processor state", "Version identity", the directive paragraph "f5-03 — Kernel personality core", the cross-review's section 2 contradictions touching these areas and section 3.1 M02/M04 (calling conventions, SEH, PEB/TEB, SSE enablement on 98: Microsoft's 1999 press release says Windows 98 supports SSE natively), the research `docs/design/research/win32-layer-2026-10-14.md` section 4 (freeze the system-call record header first as f5-03a; loader facts: no TLS/bound/delay imports in the 46 censused images, DLLs based at 0x400000 need relocation), f5-02's fixtures and `config/toolchain.json`, the owner's decisions of 2026-10-14. `execution-abi.md` is amended, never broken for native ELF.

## Task

1. **f5-03a — record header freeze** (first deliverable, its own dated subsection in the contract and `execution-abi.md`): the `CIUKI_SYS_W32` syscall family numbering (global assignment, no collision with native calls), the record layouts (fixed 240-byte diagnostic records, the per-call argument records), personality/authority dispatch, error mapping (NTSTATUS/Win32 last-error conventions to native errno where they cross). Host test: layout and size assertions with the i686 compiler (`tests/host/abi_layout_test.c` style).
2. **Personality**: PE recognition at spawn (f5-02's parser rules for the accepted image set), `win32` personality flag on the process, refusal of native `mmap`/`brk` in `win32` processes, address layout per the contract (image, heaps, stacks, shared user data page, reserved ranges), the virtual-memory service (reserve/commit/decommit/release/protect with Windows semantics over `uaddr`), TEB allocation and the GDT index 9 FS descriptor per thread, per-process CR4.OSFXSR with FXSAVE/FXRSTOR contexts (dynamic CPU feature gating: Pentium II/E500 without SSE versus Pentium III), exception delivery to a user-mode dispatcher with continue semantics (#UD, #GP, #PF, #DE, #DB/#BP, x87/SSE faults, stack exhaustion with an emergency path), the PIT-backed performance counter, version identity records (Windows 98 SE 4.10.2222 A, DirectX 7.0a).
3. **Probes and gates**: F0-F4 probes unchanged except the GDT probe (index 9); new `f5-core` probe with QEMU cases `w32-exceptions-qemu-t23` and `w32-memory-qemu-t23` driving f5-02 fixtures; native SSE still raises #UD for ELF programs.
4. Host tests for every record and validation rule; the scheduler fixture (`test_signal_fault_records.py`) keeps compiling.

## Acceptance

Kernel host suite green; the lead runs `f0-smoke`, `f2-process`, `f1-usb` and the new cases on QEMU. Files: `src/kernel/proc/*` (new `win32/` directory for personality code), `src/kernel/arch/*` (FS/GDT/FXSAVE), `src/kernel/include/ciuki/abi.h` additions, probes, tests. Do not edit `src/kernel/vm/*` (f3-20) or the desktop.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`), record links and decisions in the relevant `docs/design/` document (append dated subsections; never rewrite earlier text), then implement and test. Report `build/codex/f5-03-report.md`: findings, changes, host tests, what the lead must verify on QEMU and the T23.
