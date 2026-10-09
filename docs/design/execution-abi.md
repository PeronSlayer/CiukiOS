# Execution and ABI contract

Contract 3 of 7 required by decision D11. Author: Claude (lead). Reviewer:
Codex. Status: approved after the Claude–Codex cross-review, 2026-10-09.

## Decision

Ciuki VMM is built with pinned clang, ld.lld and NASM as a freestanding i386
ELF executable linked at `0xC0100000`. Native programs are static ELF32
executables with no interpreter. Ring-3 code enters the kernel only through
the DPL 3 interrupt gate `0x80` with arguments in registers. The kernel is
uniprocessor, non-preemptible in F0–F1 (it runs with interrupts enabled and
yields only at defined points), schedules threads preemptively by fixed
priority with round-robin inside a priority, keeps x87 out of the kernel,
and switches user FPU state lazily through `#NM`. Every process resource
has one owner and a defined teardown order.

## Sources

- Clang, [Users Manual: freestanding builds](https://clang.llvm.org/docs/UsersManual.html):
  a freestanding build may still emit calls to `memcpy`, `memmove`, `memset`
  and `memcmp`, which the environment must provide.
- LLD, [ELF linker scripts](https://lld.llvm.org/ELF/linker_script.html).
- [System V ABI, Intel386 supplement](https://www.sco.com/developers/devspecs/abi386-4.pdf):
  cdecl calling convention, preserved registers `EBX ESI EDI EBP`, ELF32
  relocation and program-header rules.
- [ELF specification](https://refspecs.linuxfoundation.org/elf/elf.pdf): `ET_EXEC`,
  `PT_LOAD`, `PT_INTERP`, `PT_GNU_STACK`.
- Intel, [64 and IA-32 Architectures SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  Vol. 3A chapters "Task Management" (TSS, stack switch on privilege change),
  "Interrupt and Exception Handling" (gates, DPL checks, error codes, `CR2`)
  and "System Architecture Overview" (`CR0.WP`, `CR0.TS`, `CR0.NE`); Vol. 1
  "Programming with the x87 FPU" and the `FXSAVE`/`FXRSTOR` instruction
  reference for lazy FPU switching.
- Linux i386 `int 0x80` convention (`eax` number, `ebx ecx edx esi edi ebp`
  arguments), documented in
  [syscall(2)](https://man7.org/linux/man-pages/man2/syscall.2.html), used
  here only as a familiar register convention, not as an ABI promise.
- Repository facts: the existing CPL3 gate (`src/native/native_entry.asm`,
  `src/vm/session_native_process.inc`) already runs a process with a private
  page directory, `NPROC_MAX_SLOTS 4`, and fixed code/data/stack addresses;
  it is a reference, not reused code (`foundations-transition.md`).

## Toolchain (mandatory from F0)

| Item | Rule |
| --- | --- |
| Compiler | `clang --target=i686-unknown-elf`, version recorded by the build; the build fails if the major version differs from `config/toolchain.json` |
| Linker | `ld.lld -m elf_i386` with a linker script; never `--image-base` alone |
| Assembler | NASM `-f elf32` for kernel objects, `-f bin` for MBR and loader |
| Kernel flags | `-ffreestanding -fno-pic -fno-pie -fno-builtin -nostdlib -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-red-zone -fno-omit-frame-pointer -fno-strict-aliasing -std=c17 -O2 -Wall -Wextra -Werror` |
| User flags | as kernel, but x87 allowed (`-mno-80387` dropped) and SSE only after F2 adds `FXSR` checks |
| CPU baseline | i686 (Pentium Pro instruction set: `CMOV` allowed); `-march=pentiumpro` |
| Stack | 4-byte aligned at calls (`-mstack-alignment=4`); kernel entry stubs keep it aligned |
| Stack protector | `-fstack-protector-strong` in the kernel from F1 with a boot-time random guard; off in F0 |
| Runtime helpers | 64-bit division and shifts (`__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`, `__ashldi3`, `__lshrdi3`) implemented and unit-tested in `src/kernel/lib/`; the link MUST fail on any other undefined symbol |
| String routines | `memcpy`, `memmove`, `memset`, `memcmp`, `strlen` in `src/kernel/lib/`, with host unit tests |
| Output | `VMM.ELF` (physical load address `0x00100000`, virtual `0xC0100000`), plus a symbol map for crash decoding |

The kernel linker script defines `.text`, `.rodata`, `.data`, `.bss` with
4 KiB-aligned boundaries, `AT()` load addresses equal to virtual minus
`0xC0000000`, symbols for each section's start and end, and discards
`.eh_frame` and `.comment`.

## Privilege model and descriptors

- GDT: null, kernel code (DPL 0), kernel data (DPL 0), user code (DPL 3),
  user data (DPL 3), one TSS, and LDT/V86 entries reserved for
  `dos-dpmi-contract.md`. All code and data segments are flat 0–4 GiB;
  protection is by paging.
- One TSS; `ESP0` is updated on every switch to the incoming thread's kernel
  stack. The I/O permission bitmap is empty (all ports denied to ring 3)
  except where `device-firmware-ownership.md` grants a V86 VM a port.
- IDT: exception vectors `0x00–0x1F` (interrupt gates, DPL 0, except `#BP` at
  DPL 3), PIC vectors `0x20–0x2F`, syscall vector `0x80` as an **interrupt
  gate** with DPL 3; the handler re-enables interrupts only after the
  complete kernel frame is saved. `CR0`: `PE`, `PG`, `WP`, `NE` set, `EM`
  clear, `TS` managed by the FPU policy; `CR4`: `PSE`/`PGE` only as allowed
  in `boot-memory.md`, `OSFXSR` only when CPUID reports FXSR, `VME`/`PVI`
  clear (`dos-dpmi-contract.md`).
  Double fault uses a task gate to a dedicated TSS and stack so a kernel
  stack overflow still reaches the crash screen.
- Kernel stacks: 8 KiB per thread plus an unmapped guard page below, in the
  `F0000000` region (`boot-memory.md`).

## Address space of a native process

| Range | Use |
| --- | --- |
| `00000000–003FFFFF` | unmapped (null-pointer and low-address faults) |
| `00400000–` | ELF `PT_LOAD` segments |
| above segments | heap, grown by `mem_map`; never above `BFBFFFFF` |
| `BFC00000–BFFFFFFF` | main thread stack (default 1 MiB committed on demand, guard page below) |

The kernel half is shared and supervisor-only. A process cannot map pages in
the kernel half or at page 0.

## Native executable format

- ELF32, `ET_EXEC`, `EM_386`, no `PT_INTERP`, no dynamic section; `PT_LOAD`
  segments page-aligned, inside `00400000–BFBFFFFF`, not overlapping;
  writable and executable at the same time is rejected; `PT_GNU_STACK` must
  not request an executable stack.
- Entry: `ESP` points to `argc`, `argv`, `envp` as in the SysV i386 process
  start-up; `EAX..EDI` zero; `EFLAGS.IF = 1`; FPU in its reset state.
- PE32 is not accepted by the native loader; it is a later compatibility
  personality (decision D4).

## System calls

- Entry: `int 0x80`. `EAX` = call number; arguments in `EBX, ECX, EDX, ESI,
  EDI, EBP`; result in `EAX`. Values in `[-4095, -1]` are errors
  (`-CIUKI_E*`); everything else is success. All other registers are
  preserved except `EFLAGS` arithmetic flags.
- Numbers are stable once published; a removed call returns `-CIUKI_ENOSYS`.
- User pointers are checked against the process's user range and copied with
  `copy_from_user`/`copy_to_user`, which use an exception-fixup table: a fault
  inside them returns `-CIUKI_EFAULT` instead of crashing the kernel. The
  complete buffer (start, length, wrap-around, user range, and for output
  buffers writability) is validated before any externally visible side
  effect.
- No call blocks with interrupts disabled. Long operations are interruptible
  by process termination.

F0 calls are frozen now (numbers, registers, errors):

| No. | Call | Arguments | Result / errors |
| --- | --- | --- | --- |
| 0 | `exit` | `EBX` exit code | does not return |
| 1 | `yield` | — | `0` |
| 2 | `debug_write` | `EBX` buffer, `ECX` length ≤ 240 | bytes written; `-EFAULT`, `-EINVAL` |
| 3 | `probe_report` | `EBX` record buffer, `ECX` length ≤ 240 | `0`; `-EFAULT`, `-EINVAL` (record grammar in `f0-acceptance.md`) |
| 4 | `sleep_ms` | `EBX` milliseconds ≤ 60,000 | `0`; `-EINVAL` |
| 5 | `probe_query` | `EBX` output buffer, `ECX` capacity ≤ 4,096 | bytes written into the user buffer (task id, tick count, probe parameters); `-EFAULT`, `-EINVAL`, `-ENOSPC` |

Error values: `EFAULT` = 14, `EINVAL` = 22, `ENOSPC` = 28, `ENOSYS` = 38
(returned negated). `probe_query` writes, packed little-endian,
`task_id: u32`, `ticks: u64`, `request_len: u32`, then `request_len` ASCII
selector bytes; insufficient capacity returns `-ENOSPC` without writing.
Other numbers return `-ENOSYS`. `probe_query` is the output-buffer path used
by the `syslife` probe for unwritable-destination tests.

| Phase | Further calls |
| --- | --- |
| F2 | `spawn`, `wait`, `mem_map`, `mem_unmap`, `open`, `read`, `write`, `seek64`, `close`, `stat`, `readdir`, `time`, `handle_close`, event queue for input, shared-memory objects, display/compositor channel |

The F2 list is indicative; each call is specified with its errors before it
is implemented, in an update to this contract.

## Threads and scheduling

- A process has one or more threads (F0 probes: one each). Kernel threads
  exist for deferred device work.
- Fixed priorities, highest first: `device` (deferred interrupt work and
  audio refill), `interactive` (desktop, focused DOS VM), `normal`, `idle`.
  Round-robin within a priority; time slice 10 ms (10 PIT ticks).
- Preemption of ring-3 code at any timer tick. The kernel is non-preemptible
  in F0–F1: a kernel path runs until it returns to user mode or blocks. F2
  MAY add kernel preemption only together with the locking rules below.
- Interrupt handlers do the minimum (acknowledge, capture state) and defer
  the rest to a `device` thread. Handlers never call the scheduler directly
  except to mark a thread runnable.
- Interrupt-disabled sections are bounded; the budget is set in
  `device-firmware-ownership.md` and measured by an F0 probe.
- Starvation guard: a runnable `normal` thread that has not run for 1 s is
  boosted once (logged).

## Locking (uniprocessor)

- `irq_lock` (save `EFLAGS`, `cli` / restore) protects data shared with
  interrupt handlers. Held only for short, non-blocking sections.
- `mutex` (sleeping) protects data shared between threads; never taken in an
  interrupt handler; ordering documented per subsystem, with a debug build
  that checks the order.
- No lock is held across a return to user mode or across a BIOS call.

## FPU policy

- Kernel arithmetic is integer-only: C code is compiled without x87, MMX or
  SSE (flags above). A T1 audit scans the executable kernel ranges for
  x87/MMX/SSE opcodes, with explicit exceptions only for the FPU
  state-management routines (`FNSAVE`/`FRSTOR`/`FXSAVE`/`FXRSTOR`/`FNINIT`/
  `CLTS`); embedded probe payloads are audited separately because the FPU
  probe uses x87 at ring 3.
- User and VM FPU state: lazy switching. On a context switch the kernel sets
  `CR0.TS`; the first FPU instruction raises `#NM`. The `#NM` handler clears
  `TS`, saves the previous owner's state (`FXSAVE` when CPUID reports `FXSR`,
  else `FNSAVE`) into that owner's resident, correctly aligned (16-byte for
  `FXSAVE`) buffer, then restores the new owner's state; on first use it
  restores a fully initialised state image (all registers, and for FXSR
  also `MXCSR` and the XMM registers) — `FNINIT` alone is insufficient.
  When the owner is destroyed, ownership is dropped without saving.
  `CR0.MP = 1` and `CR0.NE = 1`. From F0, an unmasked x87 exception
  (`#MF`, or `#XM` with SSE) in ring 3 terminates only the faulting process
  like any other user fault; from F2 it is delivered to the faulting thread
  as a fault event that the program can handle.
- F0 criterion (`f0-acceptance.md`, probe `fpu`): two preempted probes with
  different x87 control and data state keep it through 1,000 switches and
  the destruction of one of them; a probe that faults in the FPU does not
  disturb the other.

## Faults and process teardown

- A fault in ring 3 (`#GP`, `#PF` outside a valid mapping, `#UD`, privileged
  instruction, unauthorized `in`/`out`) terminates that process with an exit
  code naming the vector; other processes continue (F0 criterion).
- A fault in ring 0 outside a fixup range is a kernel panic: the crash screen
  shows vector, error code, `EIP`, `CR2`, registers and a symbolized
  backtrace when the map is present, on screen and COM1, then halts. The
  panic path allocates nothing, takes no lock other than a re-entry guard,
  and performs no disk I/O.
- Teardown order on exit or kill: stop threads (no thread of the process can
  run again); cancel pending waits and timers; close handles (files, events,
  shared memory, device grants); release device ownership and DMA buffers;
  release FPU ownership; unmap user pages and free page tables; free the
  kernel stack after switching away from it; record the exit code; wake
  waiters. Each step is idempotent so a failed teardown can be retried.
- F0 criterion: 100 create/fault/exit cycles return the physical free count
  and kernel heap usage to their starting values.

## Acceptance tests

- T1: build from a clean clone with the pinned toolchain; the link fails on
  an undefined symbol; the opcode audit passes with only the listed FPU
  state-management exceptions; host unit tests for `src/kernel/lib/` pass.
- F0 probes (names in `f0-acceptance.md`): two ring-3 probes with identical
  virtual addresses and different backing pages; 10,000 preemptive switches
  between non-yielding probes; user access to kernel memory, `cli`, `hlt`,
  `in`/`out`, and an invalid syscall buffer each fault or fail locally; FPU
  isolation; 100 create/fault/exit cycles without leaks; kernel `#PF` and
  stack overflow reach the crash screen (destructive test, run last).

## Open questions

- Time-slice length and the `device` priority's share need measurement with
  audio in F4; 10 ms is a starting value.
- Whether SSE in native programs is worth enabling before F4 (FXSR is on the
  P-III; QEMU `pentium3` reports it). Needs a use case.
- Whether F2 needs kernel preemption at all, or only shorter kernel paths.

## Interfaces required from other contracts

- `boot-memory.md`: virtual regions for kernel stacks and the direct map.
- `device-firmware-ownership.md`: interrupt-disabled budget, I/O permission
  grants, deferred-work model for drivers.
- `dos-dpmi-contract.md`: GDT/LDT entries, V86 and DPMI client FPU state, VM
  threads and their priority.
- `f0-acceptance.md`: probe names and `probe_report` record format.
