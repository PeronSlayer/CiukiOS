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
- Kernel stacks: 32 KiB per thread (16 KiB until 2026-10-11, raised after the
  optical probe path double-faulted on QEMU) plus an unmapped guard page below, in the `F0000000`
  region (`boot-memory.md`), superseding F0's 8 KiB bound for f2-20. The T23 sweep
  `44444444` (boot 33, image `cb33aec3…`, commit `f7286ca`) exhausted that bound after
  `inherit-cloexec` (ESP 8204 bytes below the top, faulting CR2 access at depth 8208).
  The capture reports 523260 KiB usable RAM, 3276 free DMA pages and 126816 free NORMAL
  pages; the pre-change build/map at `6c6dfc471aa1` places `c01139a0` inside `fat_next`
  (an operand, so the historical instruction needs the matching historical ELF). The
  next source operation is the legacy share test, with the identifiable chain
  `probe_f2_fd_table` (inlined `kernel_cases`) → `vfs_open` → `resolve` → `fat_lookup` →
  `fat_next` → directory/cache/device I/O; the first five compiler stack-usage entries
  alone total 7104 bytes before directory reads, callers and interrupts. Before
  enlargement, the production fd cases, FAT executable snapshot/load, inherited-fd spawn
  and durable cases measured 8208 touched bytes on a diagnostic host stack; a guarded
  8192-byte host stack faulted with and without a simulated IRQ (host x86-64 is not T23
  evidence). Choose four mapped pages rather than two for every thread, because native
  threads also run spawn/VFS syscalls: cost +2 physical pages (8 KiB) per live task, +2
  virtual pages per slot, no new page tables in the preallocated kernel region, same
  guard and dedicated double-fault TSS. Preserve the 4096-byte single-frame guard and
  stricter 1024-byte app/supervisor guard. [Intel SDM Vol. 3A
  §6.12.1](https://www.sra.uni-hannover.de/Lehre/SS22/V_BSB/doc/intel_manual_vol3.pdf)
  confirms same-CPL IRQ entry uses the current stack; [FreeRTOS's upstream
  fill/scan](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/tasks.c) informs the
  bounded, on-demand byte watermark: fill before the first frame, skip the reserved
  canary, report full size if it is damaged. This measures touched bytes, can
  underestimate untouched reservations/pattern collisions, and adds O(stack size)
  creation/scan work without scheduler/IRQ instrumentation; fd-table DATA and completed
  sweep SWEEP records carry task, size and high_water. Host regression reserves the
  68-byte IA-32 entry frame plus 512 handler bytes at every storage callback and keeps
  overflow negative controls; the guarded 16 KiB run passes with 8208 touched bytes,
  8176 bytes of headroom and 230 simulated interrupts; QEMU and T23 requalification
  remain the lead's acceptance gate.

## Decision for F2: a POSIX-compatible native API (2026-10-09)

The owner decided ([dev diary 2026-10-09-10](../../dev_diary/2026-10-09-10-scope-retrogaming-posix-rete.md))
that native programs use a POSIX subset through a ported C library instead
of a bespoke API. Reason: SDL, lwIP, Mesa, the open-source game engines and
Wine (the planned Win32 personality, LGPL-2.1-or-later) are written against
POSIX. A documented POSIX subset should reduce the porting work; it does
not remove platform backends, and Wine will additionally need host
services (signals with machine context, fine-grained `mmap`) and a CiukiOS
backend of its own. F2 does not guarantee Wine compatibility. The kernel
keeps its own system-call numbers (this contract); the library translates.
Before F2 code this contract MUST:

- select the libc (newlib, whose porting model is a board-support layer,
  or musl, which is Linux-oriented and needs more adaptation) and the set
  of supported interfaces;
- specify `errno` values, fundamental types, descriptor inheritance,
  process creation (`spawn`-style creation is the baseline; `fork` is
  marked supported or excluded explicitly), `exec`, `exit`, `wait`;
- specify threads (1:1 with kernel threads), TLS and pthreads, and any
  futex-like wait/wake primitive as a separate CiukiOS extension (futex is
  a Linux interface, not POSIX);
- specify signal masks, delivery context and return, including
  `SIGSEGV`/`SIGFPE`/`SIGILL` with the fault context;
- specify memory mapping semantics (`mmap`, `munmap`, `mprotect`, `brk`),
  blocking and interruption of system calls;
- specify files and directories (`open`, `read`, `write`, `lseek` with
  64-bit offsets, `stat`, `readdir`, `mkdir`, `rename`, `unlink`, `dup`,
  `fcntl`), time (`clock_gettime`, `nanosleep`), and path mapping between
  drive letters and POSIX paths;
- state what is excluded (users and Unix permissions, terminals); sockets
  arrive with F6.

Static ELF32 stays the initial executable format; dynamic loading is a
later decision. The F2 gate: an identified upstream application builds
against the CiukiOS SDK and passes named runtime tests. The F0 calls below
stay as the probe interface; the POSIX table extends the same numbering
space.

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
- Ring-3 timer preemption also honours a newly ready higher-priority task.
  Measurement counters preserve timer-caused transitions between measured
  users when an intervening device dispatch services the wake; intermediate
  kernel dispatches are not counted, and exact-target parking is retained.
  Ring 0 remains voluntarily scheduled: `kpreempt_point()` honours a pending
  reschedule only with IF set and no automatic-preemption hold. It closes the
  measured kernel stretch even when no switch is needed. Kernel loops target
  one point per 1 ms (5 ms limit), including each 64 KiB of ELF copying and
  each sixteen newly zeroed pages. A point is never inserted in an IRQ.
- F3-41 window presentation targets a stricter 4 ms stretch limit: a VGA
  source/destination row is one conversion/copy unit, with at most four rows
  between points; compositor submission yields after each staged tile
  (at most 4096 bytes), with its framebuffer
  mutex and drawing claim released. Retained surfaces/grants survive that
  point; cancellation, live display and mapping generation are rechecked.
  VGA scanout reads live planes between row groups, retaining only derived
  row addresses/geometry, palette and the previous scanout bytes used by
  exact comparison. Window tearing is acceptable: guest stores across a
  point may appear in this sample or the next. New dirt/change counters
  remain pending and changed rows are re-sampled next time. No complete
  plane snapshot or per-point timing is added; the host replay times whole
  samples separately from the production scheduler/cpustat stretch replay.
  VM creation reserves its commitment before clearing its unpublished slot
  in 4096-byte chunks, then yields after each initialized guest page.
- At a safe point a focused DOS VM with a PIC-pending virtual IRQ0 or SB16
  IRQ requests dispatch ahead of
  presentation work of the same priority. Device work still wins; the
  existing 8/10-tick focus budget is neither extended nor renewed by IRQs.
  After exhausting it, peers run until the next period. Masked/in-service
  IRQs, blocked/stopping VMs and unrelated input IRQs add no preference.
  Guest IF/STI shadows do not suppress this scheduling hint: running the
  guest lets it reach its next delivery boundary. The existing virtual IF,
  shadow and IRQ-acknowledgement rules remain in the monitor.
  This preserves the desktop's reserved share under continuous IRQ load.
  Source comparison and host/physical evidence limits are recorded in
  [F3-41 validation](../validation/f3-41-bounded-presentation.md).
- A device task consumes at most ten running PIT ticks without blocking before
  continuing at normal priority. Voluntary yields do not renew this allowance;
  a blocking dispatch restores its device priority and resets its allowance.
  Overruns request a switch at the next safe point and are reported by cpustat
  (sixteen distinct task names per window; further event loss is counted).
- The supervisor gives only the desktop's initial UI thread interactive
  priority, before publication. Further native threads retain normal priority.
- Interrupt handlers do the minimum (acknowledge, capture state) and defer
  the rest to a `device` thread. Handlers never call the scheduler directly
  except to mark a thread runnable.
- Interrupt-disabled sections are bounded; the budget is set in
  `device-firmware-ownership.md` and measured by an F0 probe.
- Starvation guard: a runnable `normal` thread that has not run for 1 s is
  boosted once (logged).

### Bounded output and CPU evidence (f0-21)

The [latency research](research/kernel-latency-2026-10-14.md) is the design
input. The implementation was compared with Linux's official
[console callback contract](https://docs.kernel.org/driver-api/tty/console.html)
(thread output versus emergency atomic output),
[RT throttling policy](https://docs.kernel.org/scheduler/sched-rt-group.html),
[bounded softirq processing](https://github.com/torvalds/linux/blob/v6.12/kernel/softirq.c),
and [mutex rules](https://docs.kernel.org/locking/mutex-design.html).
Ciuki uses these mechanisms as references; no upstream code is copied.

`serial_write` always copies a whole record into a bounded 64 KiB RAM queue,
including before scheduler start and during audio playback. It performs no UART
read, drain, sleep or yield. Overflow drops the whole incoming record and
increments a cumulative counter; the consumer inserts one `[serial] dropped=N`
notice when the backlog empties. This directive's nonblocking/drop requirement
supersedes the research proposal for lossless task-context back-pressure.
The device-priority `log-writer` sends at most 64 verified FIFO bursts per wake,
stops at the first busy THRE, and sleeps 1 ms. The timer retains its bounded
single-burst sender. Only explicit `serial_panic` enables synchronous draining,
including subsequent panic records. Disk capture remains independent.

Ordinary console writes append history and mark it dirty. The normal-priority
console task paints at most once per 100 ms. Pixel rendering holds presenter
claims with IF restored, releases the claim before each preemption point, and
checks display ownership/mapping again after yielding. A scroll moves existing
scanlines in bounded chunks and rasterizes only new rows and the partial line.
Panic output remains immediate; explicit operator/evidence overlays remain
synchronous. No normal console drawing occurs while the desktop owns the grant.

The device-priority cpustat task emits the research section 5 records every
2000 ms in verbose/test boots, and on a threshold in normal boots. A complete
`[desktop] stall` diagnostic submitted to the console stream wakes it for an
immediate report. Dispatch/IRQ boundaries charge calibrated TSC cycles (PIT ticks
before calibration); IRQ time is separated from task shares. Timer samples
separate kernel and user/VM86 execution. The class shares total 1000 permille
(rounding belongs to normal); reaping preserves class totals and reports
`exited_pm`. Native task names derive from the process basename. Top five entries use
eight-character names plus the full task ID (record delimiters in names become underscores);
`kpm` is a timer-sampled kernel share, not cycle-exact mode accounting. Records
retain the maximum stretch, its sampled EIP, reschedule latency, timing evidence
and active-CPU path totals. IRQ-off/timer-gap counters and work maximum retain
the existing timing/work subsystem's cumulative scope. Queue drops are
cumulative. All formatting/output happens on the cpustat task, never the IRQ.

Sleeping mutexes and filesystem mutexes count automatic-preemption holds per
task. Generic points defer under those holds; the existing audited `fs_service`
blocking/yield path remains responsible for progress under filesystem locks.
The UHCI schedule dump releases only its transfer mutex between records,
retaining the per-controller general-transfer reservation. Address-map zeroing
publishes a non-readable provisional extent before yielding: overlapping
mutations are busy and first-fit skips it. Protection changes reserve their
whole boundary extents with an internal pin across backing allocation; existing
readable pages remain readable. Failure removes provisional state/backing. Provisional thread IDs are hidden from
join/detach lookup, and thread creation rechecks process state before publication
when stack allocation has yielded.
New heap pages are already zero, so brk only clears the previously exposed tail.

Each USB controller gets its own host worker and keeps its own reservation.
Workers sleep to the next 250 ms root-scan deadline. The independent HID worker
retains 1 ms completion service: the endpoint's 8 ms hardware interval does not
justify adding up to 8 ms of software delivery delay, and the directive requires
1 ms service during another controller's bind. This intentionally resolves the
research C10 suggestion in favour of its existing measured HID cadence.
Storage-writer deadline changes and bind-log flush changes belong to f1-63 and
f1-62 respectively (`mount.c` and `usb_storage.c` are outside f0-21). Audio refill
and the firmware idle worker already use condition/deadline waits; diagnostic
self-test/backlog waits keep their existing bounds. Lazy CR3 (research C11)
remains conditional on measurements and is outside this directive.

Host evidence lives in the serial UART model, scheduler/console latency replays,
process allocation/ELF and desktop tests, and USB worker/register replays. These
do not establish the T23's 1/5/10 ms timing gates; QEMU and hardware qualification
remain with the lead.

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
  restores a fully initialised state image (all x87 registers, and, once
  `CR4.OSFXSR` is enabled in a later phase, also `MXCSR` and the XMM
  registers) — `FNINIT` alone is insufficient. In F0 `OSFXSR` stays clear:
  SSE instructions raise `#UD` in ring 3, so no XMM state exists to
  preserve, and `FXSAVE` is used only for the x87 part of the image.
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

## F2 extension: native ABI version 1

Author: Codex. Directive: f2-00. Revised after cross-review 2026-10-10;
lead approval pending. The text above is preserved verbatim. **[F2]** This
extension MUST be read with [posix-subset.md](posix-subset.md) and
[f2-acceptance.md](f2-acceptance.md). Its requirements MUST extend the frozen
F0 interface; they MUST NOT retroactively change F0 probe behavior. In
particular, the earlier statement that other numbers return ENOSYS describes
F0: F2 MUST additionally implement exactly calls 16–68 below. Numbers 6–15 and
unassigned numbers MUST return ENOSYS. The old indicative F2 names `mem_map`,
`mem_unmap`, `seek64`, `time` and `handle_close` MUST mean the finalized mmap,
munmap, lseek64, clock_gettime and close interfaces below, not extra calls.

**[F2]** Normative tables and record definitions in this extension MUST be
implemented exactly. `u8/u16/u32/u64` and `i32/i64` MUST denote fixed-width
little-endian integers. Every pointer MUST be a u32 user virtual address;
wire records MUST have four-byte alignment, explicit padding and the exact
sizes below. Output padding MUST be zero and input reserved fields MUST be
zero. ABI version 1 MUST use no host-compiler-dependent enums, bitfields,
`long` widths or pointers inside kernel-private objects.

### Types, constants and records

**[F2]** C MUST use ILP32: char 8 bits, short 16, int/long/pointer 32,
long long 64; plain char MUST be signed. `size_t/uintptr_t` MUST be u32;
`ssize_t/ptrdiff_t/pid_t` i32; `off_t/time_t/clock_t` i64; `ino_t` u64;
`mode_t/nlink_t/uid_t/gid_t/dev_t` u32; `blksize_t` i32; `blkcnt_t` i64;
`pthread_t` u32; `sigset_t` u64. Float/double MUST be IEEE binary32/binary64;
long double MUST be i386 x87 extended precision in a 12-byte, four-byte-aligned
object. The cdecl preserved registers and four-byte call alignment above MUST
apply to all static libraries. These are the original
[i386 SysV ABI](https://www.sco.com/developers/devspecs/abi386-4.pdf) conventions
with explicit CiukiOS 64-bit public offset/time types; they are not a modern
Linux i386 binary-layout claim.

**[F2]** Errno numbers MUST use Linux i386 numbering, independently of custom
syscall numbers and newlib's default headers. Kernel returns MUST be negative;
ordinary libc wrappers MUST return -1 (MAP_FAILED for mmap) and set the calling
thread's errno. Success MUST NOT clear errno. All error names used in this
contract MUST have these values:

```text
EPERM=1 ENOENT=2 ESRCH=3 EINTR=4 EIO=5 ENXIO=6 E2BIG=7 ENOEXEC=8
EBADF=9 ECHILD=10 EAGAIN=11 ENOMEM=12 EACCES=13 EFAULT=14 EBUSY=16
EEXIST=17 EXDEV=18 ENODEV=19 ENOTDIR=20 EISDIR=21 EINVAL=22 ENFILE=23
EMFILE=24 ENOTTY=25 ETXTBSY=26 EFBIG=27 ENOSPC=28 ESPIPE=29 EROFS=30
EPIPE=32 EDOM=33 ERANGE=34 EDEADLK=35 ENAMETOOLONG=36 ENOSYS=38
ENOTEMPTY=39 EOVERFLOW=75 EILSEQ=84 EMSGSIZE=90 EOPNOTSUPP=95
ETIMEDOUT=110 ECANCELED=125
EWOULDBLOCK=EAGAIN ENOTSUP=EOPNOTSUPP
```

The primary numbering sources are Linux's
[base error list](https://github.com/torvalds/linux/blob/v6.12/include/uapi/asm-generic/errno-base.h)
and [extended list](https://github.com/torvalds/linux/blob/v6.12/include/uapi/asm-generic/errno.h).
**[F2]** Headers MAY define the remaining Linux i386 errors for source tables;
syscalls MUST return only their closed error sets below. Invalid syscall
numbers MUST return ENOSYS before pointer validation.

**[F2]** The following constants MUST have these values. Recognized but
excluded mmap modes MUST return EOPNOTSUPP; unknown flag bits MUST return
EINVAL. Native permission bits MUST be informational, as defined in the subset.

```text
O_RDONLY=0 O_WRONLY=1 O_RDWR=2 O_ACCMODE=3
O_CREAT=0x40 O_EXCL=0x80 O_TRUNC=0x200 O_APPEND=0x400 O_NONBLOCK=0x800
O_DIRECTORY=0x10000 O_CLOEXEC=0x80000
F_DUPFD=0 F_GETFD=1 F_SETFD=2 F_GETFL=3 F_SETFL=4 F_DUPFD_CLOEXEC=1030
FD_CLOEXEC=1 SEEK_SET=0 SEEK_CUR=1 SEEK_END=2
S_IFMT=0170000 S_IFREG=0100000 S_IFDIR=0040000 S_IFCHR=0020000
PROT_NONE=0 PROT_READ=1 PROT_WRITE=2 PROT_EXEC=4
MAP_SHARED=1 MAP_PRIVATE=2 MAP_FIXED=0x10 MAP_ANONYMOUS=0x20
MAP_FAILED=0xffffffff
CLOCK_REALTIME=0 CLOCK_MONOTONIC=1 CLOCK_PROCESS_CPUTIME_ID=2
WNOHANG=1 CIUKI_SPAWN_NEW_GROUP=1 CIUKI_THREAD_DETACHED=1
_SC_PAGESIZE=1 _SC_OPEN_MAX=2 _SC_ARG_MAX=3 _SC_THREAD_KEYS_MAX=4
_SC_THREAD_STACK_MIN=5 CLOCKS_PER_SEC=1000000
SIG_BLOCK=0 SIG_UNBLOCK=1 SIG_SETMASK=2
SIG_DFL=0 SIG_IGN=1 SA_SIGINFO=4
SIGINT=2 SIGILL=4 SIGABRT=6 SIGBUS=7 SIGFPE=8 SIGKILL=9 SIGUSR1=10
SIGSEGV=11 SIGUSR2=12 SIGPIPE=13 SIGTERM=15 SIGCHLD=17
```

**[F2]** `O_EXCL` MUST require O_CREAT; O_TRUNC MUST require writable access;
O_DIRECTORY with creation/truncation or non-read-only access MUST fail EINVAL.
O_NONBLOCK MUST affect streams/channels/input, not turn regular disk I/O into
asynchronous I/O. F_SETFL MUST accept only O_APPEND and O_NONBLOCK, preserving
access mode; F_GETFL MUST return access mode and these status flags, without
open-only flags. F_SETFD MUST accept only FD_CLOEXEC. Other fcntl commands MUST
return EINVAL. Mode arguments MUST accept only octal 0777, ignore their access
meaning, and reject other bits with EINVAL.

**[F2]** These public structures MUST use the offsets and sizes shown:

| Record | Size | Fields in byte-offset order |
| --- | --- | --- |
| `timespec` | 16 | 0:i64 tv_sec, 8:i32 tv_nsec, 12:u32 reserved=0. tv_nsec MUST be 0–999999999. |
| `timeval` (library only) | 16 | 0:i64 tv_sec, 8:i32 tv_usec, 12:u32 reserved=0. |
| `stat` | 104 | 0:u32 st_dev, 4:u32 st_mode, 8:u64 st_ino, 16:u32 st_nlink, 20:u32 st_uid, 24:u32 st_gid, 28:u32 st_rdev, 32:i64 st_size, 40:i32 st_blksize, 44:u32 reserved, 48:i64 st_blocks, 56:timespec st_atim, 72:timespec st_mtim, 88:timespec st_ctim. |
| `dirent` | 792 | 0:u64 d_ino, 8:i64 d_off (next cookie), 16:u16 d_reclen=792, 18:u16 d_namlen (UTF-8 bytes), 20:u8 d_type, 21:u8 reserved[3], 24:char d_name[768], NUL-terminated and zero-filled. DT_DIR=4, DT_REG=8, DT_CHR=2, DT_UNKNOWN=0. |
| `ciuki_spawn_fd` | 8 | 0:i32 source, 4:i32 target; source=-1 means explicitly close target in child. |
| `ciuki_spawn_args` | 32 | 0:u32 size=32, 4:u32 path, 8:u32 argv, 12:u32 envp, 16:u32 fd_list, 20:u32 fd_count, 24:u32 flags, 28:u32 reserved=0. |
| `ciuki_thread_args` | 32 | 0:u32 size=32, 4:u32 entry, 8:u32 argument, 12:u32 return_trampoline, 16:u32 stack_bytes, 20:u32 flags, 24:u32 reserved[2]. |
| `ciuki_mmap_args` | 32 | 0:u32 size=32, 4:u32 hint, 8:u32 length, 12:u32 prot, 16:u32 flags, 20:i32 fd, 24:i64 offset. |
| `sigaction` | 24 | 0:u32 handler (one/three-argument union), 4:u32 flags, 8:u64 mask, 16:u32 restorer, 20:u32 reserved. Restorer MUST be the SDK's executable sigreturn trampoline when catching a signal. |
| `siginfo_t` | 32 | 0:i32 signo, 4:i32 code, 8:i32 error=0, 12:i32 sender_pid, 16:u32 fault_addr, 20:u32 vector, 24:u32 trap_error, 28:u32 reserved. |
| `utsname` | 336 | Five consecutive char[65]: sysname, nodename, release, version, machine; bytes 325–327 zero; 328:u32 abi_version=1; 332:u32 realtime_source (1=rtc, 0=build epoch). |

**[F2]** Stat MUST report st_dev as volume number (C=3, D=4, synthetic=0),
mount-lifetime stable node identities in st_ino, nlink=1 for named files and
directories, nlink=0 for open-unlinked files, uid/gid=0, rdev=0 except null=1
and console=2, and 512-byte allocation units in st_blocks. Regular modes MUST
be S_IFREG|0666 (0444 for FAT read-only); directories S_IFDIR|0777; streams
S_IFCHR|0666. St_blksize MUST be the cluster size for FAT and 4096 for synthetic
nodes. Directory/stream st_size MUST be zero. FAT timestamps MUST be interpreted
as UTC in this single-timezone subset; ctime MUST start as mtime on mount and
track in-memory metadata changes thereafter. Unavailable timestamps MUST be
zero. Subsecond precision MUST NOT be invented for on-disk timestamps.

### Register and validation rules

**[F2]** Calls MUST retain EAX=result and the six argument registers in the
frozen order. In the table, `B/C/D/S/I/P` MUST mean EBX/ECX/EDX/ESI/EDI/EBP;
`*` MUST mean a checked user pointer. Omitted registers MUST be ignored.
64-bit scalar inputs MUST use low then high u32 halves; signed values MUST
use two's-complement reconstruction. A 64-bit output MUST use a checked i64
pointer with EAX=0, never an EDX:EAX return that would break preserved EDX.
Successful addresses above 0x80000000 MUST be treated as unsigned pointers;
only 0xfffff001–0xffffffff MUST denote syscall errors.

**[F2]** The complete input/output ranges MUST be validated and held stable
against concurrent unmap/protection changes before side effects. Metadata,
paths, argv/envp, fd lists and descriptors MUST be copied into bounded kernel
storage once. User-modifiable fields MUST NOT be reread for authorization.
Output pages MUST remain pinned through completion; no lock protecting the
whole address space MUST remain held during device waits. A competing munmap
or mprotect of pinned pages MUST return EBUSY. Byte I/O contents MAY change
concurrently through another writable alias, but their address/range MUST remain
valid. Failed validation MUST produce zero external side effects. Partial
completed I/O MUST return its positive count rather than EINTR/EIO for the
uncompleted suffix. Backend failures after issuance MUST follow F1 quarantine.

**[F2]** Blocking classes MUST mean:

- `N`: no external wait, though normal scheduling/preemption still applies.
- `I`: interruptible wait; a caught signal eligible for delivery to the calling
  thread before progress MUST cause EINTR, subject to the issued-read drain
  rule below, then handler delivery before the interrupted code resumes.
- `D`: device/namespace operation; waits before commit MUST be interruptible
  by an eligible caught signal, except close and dup2, which MUST defer caught
  signals throughout and MUST NOT return EINTR. Once a mutation is committed
  or a command issued, the operation MUST finish or return its I/O failure,
  never ambiguous EINTR; process termination MUST cancel/quiesce safely.
- `X`: does not return to this invocation.

**[F2]** A caught signal MUST be eligible for delivery only if it is pending
for the calling thread (or selected for it from the process queue), unmasked,
has a catcher and the thread has no active handler. Merely pending catchers
MUST NOT interrupt a blocking call made inside a handler; such a call MUST
continue to completion, timeout or an independent error. Fatal default actions
MUST follow the separate rules in the signal section, without returning EINTR.

**[F2]** Command issuance MUST mean the first action that hands the request
to the backend for execution (the native command-register write or entry into
the serialized firmware operation). Queuing/reserving a request alone MUST NOT
count as issuance. A D-class commit MUST occur at its first namespace/data
mutation or command issuance, whichever comes first. For an I-class disk read
interrupted after issuance but before any byte is transferred to userspace,
the kernel MUST latch interruption, issue no further commands for that read,
retain its buffers/pins and drain the outstanding command under F1's original
deadline before handler entry. Data arriving during this drain MUST be discarded
for the interrupted read; its user buffer and description offset MUST remain
unchanged. A successful drain MUST yield EINTR; failure/timeout MUST yield EIO
and F1 quarantine. If bytes were already transferred, their positive count
MUST take precedence over EINTR/EIO. This adapts
[POSIX read interruption](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html)
to the qualified F1 device lifecycle.

**[F2]** Before entering a pending handler, the kernel MUST store the completed
syscall result (success, partial count or error) in the interrupted context's
EAX and finish its output writes. Sigreturn MUST restore that result unless the
handler explicitly edits the permitted context; the syscall MUST NOT run again.

**[F2]** SA_RESTART MUST be excluded. Libc MUST NOT retry partial writes or
mutating operations automatically. It MUST perform the synchronization retries
required by posix-subset.md. A fatal signal action MUST interrupt all classes
safely; cleanup MUST wait for pinned issued I/O before freeing its memory. No
operation MUST block with interrupts disabled. Entry validation order MUST be structure/
flags, descriptor identity/type, copied paths/ranges, resource reservation, then
commit. Where several errors apply in the same stage their precedence is
unspecified; tests MUST arrange single-error cases.

### Complete syscall table

**[F2]** Each row's errors MUST be the complete allowed set, after expanding
`PATH` as `EFAULT, EINVAL, ENAMETOOLONG, EILSEQ, ENOENT, ENOTDIR, EIO, ENOMEM`.
PATH MUST apply to each path argument and represent lookup/copy errors only.
ENOMEM MUST include bounded scratch/metadata allocation failure. Error lists
MUST NOT imply that no side effects are possible after a D-class commit.
`none` MUST mean no returning error. Calls 0–5 MUST keep the earlier table
exactly; process exit MUST use call 0 and MUST NOT receive a duplicate number.

| No. | Call and register signature | Success / semantics | Block | Complete errors |
| --- | --- | --- | --- | --- |
| 16 | `spawn(B=args*)` | New PID; atomic preparation and inheritance below. | D | PATH, E2BIG, ENOEXEC, EACCES, EBADF, EPERM, EAGAIN, EMFILE, EINTR |
| 17 | `waitpid(B=pid,C=status*,D=options)` | Reaped PID; 0 for WNOHANG without an exited matching child. Optional status pointer. | I | EINVAL, EFAULT, ECHILD, EINTR |
| 18 | `getpid()` | Calling process PID. | N | none |
| 19 | `getppid()` | Parent PID, or 1 after adoption. | N | none |
| 20 | `thread_create(B=args*)` | New TID; same process, kernel-owned stack/TLS. | N | EFAULT, EINVAL, EAGAIN, ENOMEM |
| 21 | `thread_exit(B=value)` | End calling thread; save opaque u32 join value. Last live thread MUST terminate process with normal status 0. | X | none |
| 22 | `tls_set(B=base,C=bytes)` | 0; set calling thread's GS base to a writable 4096-byte TLS page. | N | EFAULT, EINVAL, EBUSY |
| 23 | `wait_word(B=word*,C=expected,D=deadline*,S=clock)` | 0 on wake, including spurious wake; optional absolute deadline. | I | EFAULT, EINVAL, EAGAIN, ETIMEDOUT, EINTR, ECANCELED |
| 24 | `wake_word(B=word*,C=count)` | Number woken, at most count; zero count allowed. | N | EFAULT, EINVAL |
| 25 | `mmap(B=args*)` | Base address of zero-filled private anonymous pages. | N | EFAULT, EINVAL, EOPNOTSUPP, EACCES, ENOMEM |
| 26 | `munmap(B=base,C=length)` | 0; unmap whole pages in mmap arena, including holes. | N | EINVAL, EBUSY, ENOMEM |
| 27 | `mprotect(B=base,C=length,D=prot)` | 0; atomic protection change on fully mapped pages. | N | EINVAL, EACCES, ENOMEM, EBUSY |
| 28 | `brk(B=end)` | Current break for end=0; new break on success. | N | EINVAL, ENOMEM, EBUSY |
| 29 | `open(B=path*,C=flags,D=mode)` | Lowest free fd; mode considered only with O_CREAT. | D | PATH, EACCES, EEXIST, EISDIR, EROFS, ENOSPC, EMFILE, ENFILE, EINTR |
| 30 | `read(B=fd,C=buffer*,D=bytes)` | Bytes read or 0 at EOF; at most 1 MiB. | I | EBADF, EFAULT, EINVAL, EISDIR, EIO, EAGAIN, EINTR, ENOMEM |
| 31 | `write(B=fd,C=buffer*,D=bytes)` | Bytes written; at most 1 MiB, atomic append placement. | D | EBADF, EFAULT, EINVAL, EISDIR, EIO, EAGAIN, EINTR, EFBIG, ENOSPC, EROFS, ENOMEM |
| 32 | `pread(B=fd,C=buffer*,D=bytes,S=off_lo,I=off_hi)` | Positioned read; description offset unchanged. | I | EBADF, EFAULT, EINVAL, EISDIR, EIO, ESPIPE, EINTR, ENOMEM |
| 33 | `pwrite(B=fd,C=buffer*,D=bytes,S=off_lo,I=off_hi)` | Positioned write; ignores append status, offset unchanged. | D | EBADF, EFAULT, EINVAL, EISDIR, EIO, ESPIPE, EINTR, EFBIG, ENOSPC, EROFS, ENOMEM |
| 34 | `lseek64(B=fd,C=off_lo,D=off_hi,S=whence,I=result*)` | 0 with resulting i64 offset; no file allocation. | N | EBADF, EFAULT, EINVAL, ESPIPE, EOVERFLOW |
| 35 | `close(B=fd)` | 0; closes files and all object descriptors; fd released even on EIO; caught signals deferred throughout. | D | EBADF, EIO |
| 36 | `fstat(B=fd,C=stat*)` | 0; coherent regular/directory/stream metadata. | D | EBADF, EFAULT, EIO, ENOMEM, EINTR, EOPNOTSUPP |
| 37 | `stat(B=path*,C=stat*)` | 0; path lookup and same metadata layout. | D | PATH, EINTR |
| 38 | `getdents(B=fd,C=records*,D=bytes)` | Bytes of complete dirent records; 0 at end. | I | EBADF, EFAULT, EINVAL, ENOTDIR, EIO, ENOMEM, EINTR |
| 39 | `mkdir(B=path*,C=mode)` | 0; create one directory. | D | PATH, EACCES, EEXIST, EROFS, ENOSPC, EINTR |
| 40 | `rmdir(B=path*)` | 0; remove empty, unpinned directory. | D | PATH, EACCES, ENOTEMPTY, EBUSY, EROFS, EINTR |
| 41 | `rename(B=old*,C=new*)` | 0; replace/rename on same volume; identical-entry no-op before replacement/pinning checks, except case-only spelling update. | D | PATH, EACCES, EXDEV, EISDIR (file over directory), ENOTDIR (directory over file), EINVAL (directory into own descendant), ENOTEMPTY, EBUSY, EROFS, ENOSPC, EINTR |
| 42 | `unlink(B=path*)` | 0; remove regular-file name, defer chain release if open. | D | PATH, EACCES, EISDIR, EBUSY, EROFS, EINTR |
| 43 | `dup(B=fd)` | Lowest unused fd referring to same description; CLOEXEC clear. | N | EBADF, EMFILE |
| 44 | `dup2(B=oldfd,C=newfd)` | newfd after atomic replacement; oldfd==newfd MUST be a no-op on a valid fd; caught signals deferred throughout. | D | EBADF |
| 45 | `fcntl(B=fd,C=cmd,D=arg)` | FD/status flags or new fd for supported commands; setters return 0. | N | EBADF, EINVAL, EMFILE |
| 46 | `fsync(B=fd)` | 0 after data/metadata and device durability barrier. | D | EBADF, EINVAL, EIO, EROFS, EINTR |
| 47 | `ftruncate(B=fd,C=len_lo,D=len_hi)` | 0; change regular-file size, preserving position. | D | EBADF, EINVAL, EISDIR, EFBIG, ENOSPC, EROFS, EIO, ENOMEM, EINTR |
| 48 | `getcwd(B=buffer*,C=capacity)` | Bytes including NUL; libc MUST return buffer. | N | EFAULT, EINVAL, ERANGE, ENOMEM |
| 49 | `chdir(B=path*)` | 0; replace process cwd node reference. | D | PATH, EINTR |
| 50 | `clock_gettime(B=clock,C=timespec*)` | 0; REALTIME, MONOTONIC or calling process CPU time. | N | EFAULT, EINVAL |
| 51 | `nanosleep(B=request*,C=remaining*)` | 0 after relative MONOTONIC interval; remaining optional. | I | EFAULT, EINVAL, EOVERFLOW, EINTR |
| 52 | `sigaction(B=signal,C=new*,D=old*)` | 0; either pointer MAY be null; process-wide disposition. | N | EFAULT, EINVAL |
| 53 | `sigprocmask(B=how,C=set*,D=old*)` | 0; calling thread's mask; null set queries only. | N | EFAULT, EINVAL |
| 54 | `kill(B=pid,C=signal)` | 0; signal/probe targets in caller's process group only. | N | EINVAL, ESRCH, EPERM |
| 55 | `sigreturn(B=frame*)` | Restore validated active signal frame; no ordinary return. | X | none; malformed frame MUST terminate caller with SIGSEGV |
| 56 | `uname(B=utsname*)` | 0; sysname CiukiOS, nodename ciuki, machine i686, `release` the recorded build ID, `version` the CiukiOS version string (`config/version.json`, e.g. `0.9.0 pre-alpha`). | N | EFAULT |
| 57 | `surface_create(B=width,C=height,D=format)` | New CLOEXEC fd for zeroed shared surface; format=1 XRGB8888. | N | EINVAL, EOVERFLOW, ENOMEM, EMFILE, ENFILE |
| 58 | `surface_map(B=fd,C=prot)` | Base address; whole shared object, READ or READ plus WRITE. | N | EBADF, EINVAL, EACCES, ENOMEM |
| 59 | `surface_info(B=fd,C=info*)` | 0; immutable geometry and allocation length. | N | EBADF, EFAULT |
| 60 | `present(B=display_fd,C=surface_fd,D=rect*)` | 0 after synchronous, clipped copy to qualified F1 framebuffer. | D | EBADF, EFAULT, EINVAL, EACCES, ENODEV, EIO, EINTR |
| 61 | `input_read(B=input_fd,C=events*,D=capacity)` | Event count; capacity in records, 1–64. | I | EBADF, EFAULT, EINVAL, EACCES, ENODEV, EIO, EAGAIN, EINTR |
| 62 | `channel_pair(B=two_fds*)` | 0; two CLOEXEC bidirectional bounded message endpoints. | N | EFAULT, ENOMEM, EMFILE, ENFILE |
| 63 | `channel_send(B=fd,C=message*,D=flags)` | 0 after atomic enqueue; flags=0 or DONTWAIT=1. | I | EBADF, EFAULT, EINVAL, EMSGSIZE, EAGAIN, EPIPE, EINTR, ENOMEM |
| 64 | `channel_recv(B=fd,C=message*,D=flags)` | 1 for one message, 0 for peer-closed and drained. | I | EBADF, EFAULT, EINVAL, EAGAIN, EMFILE, EINTR |
| 65 | `thread_join(B=tid,C=value*)` | 0 and optional opaque u32 exit value; reap joinable thread. | I | ESRCH, EINVAL, EDEADLK, EFAULT, EINTR |
| 66 | `thread_detach(B=tid)` | 0; release retained result automatically at exit. | N | ESRCH, EINVAL |
| 67 | `display_info(B=display_fd,C=info*)` | 0; current geometry, masks and generation, without physical address. | N | EBADF, EFAULT, EACCES, ENODEV |
| 68 | `thread_kill(B=tid,C=signal)` | 0; signal/probe one thread of the calling process. | N | EINVAL, ESRCH |

There are **53 new calls (16–68)** and **59 assigned calls including 0–5**.
**[F2]** The count MUST NOT include the ten reserved numbers or library-only
wrappers. Fstat on surface/channel/grant objects MUST return EOPNOTSUPP;
other wrong-kind descriptor uses MUST return EBADF unless the row specifies
EISDIR, ENOTDIR, ESPIPE or EINVAL. Dup2 MUST discard an implicit-close error
while keeping that delayed error on the underlying filesystem for fsync; its
replacement MUST never be half-completed. Getdents capacity MUST be 792–65536
bytes; the unused suffix MUST remain untouched. Fsync MUST accept regular files
and directories and return EINVAL for non-filesystem objects; a read-only file
fd on a writable volume MUST still be accepted.

### ELF loading and initial state

**[F2]** Native files MUST be ELFCLASS32, ELFDATA2LSB, EV_CURRENT, ET_EXEC,
EM_386, e_flags=0, ELFOSABI_NONE, ABI version zero. The loader MUST validate
the 52-byte ELF header, e_phentsize=32, 1–16 program headers, checked file/table
extents, p_filesz<=p_memsz and bounded additions before allocating. It MUST load
only PT_LOAD segments. PT_NULL and bounded PT_NOTE/PT_PHDR metadata MAY be
ignored; PT_GNU_STACK MUST lack PF_X. Other program-header types, PT_INTERP,
PT_DYNAMIC and PT_TLS MUST cause ENOEXEC. A section table MAY be absent; when
present its bounds MUST be checked and SHT_DYNAMIC MUST cause ENOEXEC. Section
names MUST NOT determine mappings. This is a deliberately restricted profile of
the [ELF program-loading specification](https://refspecs.linuxfoundation.org/elf/elf.pdf).

**[F2]** Every nonempty LOAD MUST have p_align=4096, page-aligned p_vaddr and
p_offset, PF_R set and only PF_R/PF_W/PF_X bits. Rounded memory extents MUST
not overlap, wrap, or leave `0x00400000..0x0fffffff`; this is F2's loader limit
within the frozen user-image range. Both file size and the sum of rounded LOAD
memory sizes MUST be at most 32 MiB. The entry MUST lie in the initialized
file-backed portion of an executable, non-writable LOAD. The loader MUST copy
exactly p_filesz and zero BSS and final-page padding. W+X, executable GNU_STACK,
PIE and unresolved dynamic relocation requirements MUST be rejected. Loading
MUST hold a consistent file snapshot against concurrent writes/truncation;
failure MUST free all provisional resources without publishing a child.

**[F2]** Main stack reservation MUST remain `0xbfc00000..0xbfffffff`. Its
usable default range MUST be `0xbff00000..0xbfffffff` (1 MiB); the guard at
`0xbfeff000..0xbfefffff` and remaining reservation MUST stay unmapped. Backing
credits MUST be reserved before successful spawn; pages MAY be committed on
first access. ESP MUST be four-byte aligned and point to this consecutive
u32-word sequence:

```text
argc, argv[0] ... argv[argc-1], 0,
envp[0] ... envp[envc-1], 0,
AT_PAGESZ=6, 4096,
AT_ENTRY=9, entry,
AT_CIUKI_TLS=0x60000001, tls_base,
AT_CIUKI_TLS_SIZE=0x60000002, 4096,
AT_CIUKI_ABI=0x60000003, 1,
AT_NULL=0, 0
```

**[F2]** Strings MUST be copied above these arrays in the stack, with no kernel
pointers or parent aliases. Combined strings (including NUL), pointers,
terminators, argc, auxv and alignment padding MUST fit ARG_MAX=65536 bytes;
argv MUST contain 1–256 strings, envp 0–256. A null envp MUST mean empty;
null argv MUST fail EINVAL. EAX..EDI and EBP MUST start zero, EFLAGS=0x202,
CS/SS/DS/ES/FS MUST be ordinary DPL3 flat selectors and GS the TLS selector.
The startup arrangement follows
[SysV process initialization](https://www.sco.com/developers/devspecs/abi386-4.pdf)
with explicitly named CiukiOS auxv extensions. No function return address MUST
precede argc. Initial x87 state MUST follow the frozen reset policy.

### Processes, inheritance and teardown

**[F2]** PID 1 MUST be a supervisor/reaper with no public way to acquire its
authority. Positive process and thread IDs MUST be monotonically allocated
through INT32_MAX and MUST NOT be reused within one boot; exhaustion MUST
return EAGAIN. Limits MUST be 64 processes including zombies, 256 live/retained
threads system-wide, 16 threads per process, 128 fds per process and 1024 open
descriptions/object endpoints system-wide. These are bounds on metadata, not
reservations of all corresponding physical memory. A process MUST own its
address space, fd table, cwd, signal dispositions and process group; threads
MUST share these and own masks, registers, TLS, stacks and x87 state.

**[F2]** Spawn MUST resolve the copied POSIX path through the production VFS,
without PATH search, script interpreter or PE fallback. It MUST copy the caller's
cwd reference, snapshot fd sources and signal state, validate/load everything,
then publish one runnable child and return its PID. Failed preparation MUST
create no visible PID/child and leave parent state unchanged. Signals caught
by the parent MUST reset to default; ignored dispositions MUST remain ignored;
the calling thread mask MUST be inherited and pending signals MUST be empty.
The child MUST inherit its parent's group unless NEW_GROUP is set, in which
case its PGID MUST equal its PID. No setpgid/setsid operation exists in F2.

**[F2]** Fds 0–2 MUST be inherited at the same numbers if open and not CLOEXEC;
closed or CLOEXEC slots MUST remain closed, with no automatic device opening.
Every other fd MUST be absent unless named in the explicit list. The list MUST
contain 0–128 entries with distinct targets in 0–127. Entries MUST apply
simultaneously to the parent snapshot, so swapping two fds is well-defined;
they MUST override the default 0–2 inheritance. Source=-1 MUST explicitly close
a target. A CLOEXEC source explicitly named MUST fail EBADF, not override its
close-on-spawn policy. Successful inherited descriptions MUST share position
and status flags; child descriptor CLOEXEC MUST be clear. Fd ownership MUST NOT
be consumed in the parent. Display/input grants MUST be non-inheritable even
through the explicit list (EPERM). The bootstrap supervisor MAY install them
directly in the trusted desktop, outside the public spawn interface.

**[F2]** Call 0 MUST terminate the entire native process from any thread;
the F0 single-thread probe semantics MUST stay identical. Libc normal exit
MUST expose only the low eight status bits to waitpid. Kernel diagnostics MUST
retain the full raw exit code and fault vector; F0's `0x100+vector` code MUST
remain available to its probes. Wait status MUST be `code<<8` for normal exit
and the signal number in bits 0–6 for signal termination; the core bit MUST
stay zero. WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG MUST decode this layout.
Stopped/continued children MUST NOT be reported because job control is excluded.

**[F2]** Waitpid MUST accept a positive child PID or -1 (any direct child)
and options 0/WNOHANG only. Pid zero or less than -1 MUST fail EINVAL. With
multiple exited children, -1 MUST choose the lowest PID. Status copyout and
reaping MUST be one transaction; invalid status pointers MUST leave the zombie
waitable. Only one waiter MUST reap a child; remaining waiters MUST recheck
their predicate. Orphans MUST be reparented to PID 1, which MUST reap them.
Default SIGCHLD MUST be ignored without suppressing zombies; explicit SIG_IGN
MUST auto-reap children and make later waits fail ECHILD when none remain.

**[F2]** Process termination MUST first prevent every thread from running,
then cancel waits/timers and drain or quarantine outstanding requests, close
handles, release device claims/DMA, release FPU ownership, unmap and free page
tables, and free thread stacks only after switching away. This MUST preserve
the frozen teardown order. Last-handle surface backing MUST remain alive while
another process mapping or a queued message holds it. Only a small zombie
record (PID, parent, status, diagnostic reason) MUST survive until reaping;
exit publication, SIGCHLD and waiter wakeup MUST occur after resource release.
Every step MUST be idempotent. A user fault MUST NOT panic the kernel or tear
down an unrelated process or the desktop.

**[F2]** The supervisor MUST close/reap its own completed children and MUST
never exit through the public ABI. Default program scheduling MUST be normal;
only the bootstrap supervisor MUST give the desktop interactive priority.
Public spawn/thread calls MUST NOT accept priority elevation. No process MUST
be published before all of its minimum stack, TLS, page-table and diagnostic
resources have been reserved. A pending termination during creation MUST cancel
publication and reclaim the provisional child.

### Threads, TLS and private wait queues

**[F2]** F2 MUST append one DPL3 writable TLS data descriptor at GDT index 7,
selector 0x3b, without changing existing flat selectors or TSS descriptors.
The existing flat-segment rule MUST continue to apply to ordinary CS/DS/ES/SS;
this additional GS-only descriptor MUST have the current thread's base and
byte limit 4095. Every switch MUST reload its descriptor and GS; no LDT or
user-supplied descriptor flags MUST be accepted. The kernel MUST use its own
saved thread identity, never user TCB values, for authorization. This follows
the descriptor and privilege rules in the
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

The current F0 protection probe checks a seven-entry GDT limit in
`src/kernel/probes/probes.c`. **[F2]** That structural check MUST be extended to
verify the added TLS descriptor, its DPL/limit and per-switch base isolation;
all earlier protection checks and calls 0–5 MUST continue to pass. The reserved
future DOS LDT entries MUST NOT reuse index 7.

**[F2]** A TLS page MUST initially be zero except its 64-byte TCB header:
u32 self at 0, tid at 4, reent pointer at 8, pthread-private pointer at 12,
stack base at 16, stack bytes at 20, flags at 24 and nine reserved u32 at
28–63. Flags MUST be zero in F2. The kernel MUST initialize self/tid/stack;
crt0 or the pthread trampoline MUST initialize the two library pointers before
using libc. `tls_set` MUST validate a four-byte-aligned writable 4096-byte
private anonymous range, set self/tid/stack fields, pin it for the caller and
unpin the former page atomically. It MUST fail EBUSY inside an active signal
handler or if another thread already owns that TLS range. Extra compiler TLS
and PT_TLS templates MUST remain excluded; pthread keys and `_reent` are the
supported F2 thread-local interfaces.

**[F2]** Thread-create entry and return trampoline MUST name executable image
addresses. Stack_bytes=0 MUST select 1 MiB; other sizes MUST be page multiples
from 64 KiB through 4 MiB. The kernel MUST allocate stack plus a guard page
and TLS in the mmap arena, initialize a cdecl stack `(return_trampoline,arg)`,
inherit the creating thread's mask, reset x87 and zero other registers, then
publish the thread. Libc's entry trampoline MUST allocate/init `_reent` before
the user routine and call thread_exit after destructors. Failure there MUST
terminate the process with SIGABRT rather than run with shared errno.

**[F2]** Kernel-owned stack/TLS mappings MUST be protected from munmap while
their thread is live; detached threads MUST release them after switching away.
Joinable exited threads MUST retain only their TID/value until joined; their
stack/TLS MUST already be released. Join MUST reject self with EDEADLK,
detached or already-claimed joins with EINVAL, and another process's/unknown
TID with ESRCH. An interrupted join MUST release its claim. Detach MUST reject
already-detached/claimed joins with EINVAL and unknown TIDs with ESRCH.
Thread exit MUST NOT close process fds or release other threads' mappings.

**[F2]** Wait_word MUST require a four-byte-aligned readable u32 in private
user RAM (ELF data/BSS, heap, stack or private anonymous mmap). Static pthread
objects in BSS MUST therefore work. The key MUST be
`(process identity, mapping generation, virtual address)`,
never physical address alone. Shared surface words MUST be rejected with
EINVAL. Compare and enqueue MUST be atomic with respect to wake/unmap; mismatch
MUST return EAGAIN without sleeping. Deadline MUST be null (infinite) or an
absolute timespec on REALTIME/MONOTONIC; an expired matching wait MUST return
ETIMEDOUT. Wake MUST choose FIFO waiters up to count without saving wake credits.
Unmap MUST remove such waiters with ECANCELED before the virtual address can
be reused; these waits MUST NOT pin their page indefinitely. A queued wait MUST
not prevent normal page reclamation. A kernel wait/wake operation already bound
to a mapping generation MUST NOT affect a later mapping at the same address;
the kernel MUST bind the key while validating the mapping and MUST NOT rebind
an in-flight operation after unmap. Wake_word has no userspace generation token:
a delayed userspace call made after address reuse MUST resolve the current
mapping and MAY wake its waiters. Applications MUST coordinate object lifetime,
including outstanding userspace wakers, before destroying or reusing its storage.

### Memory and clocks

**[F2]** The heap MUST start at page-rounded highest LOAD end and grow below
0x20000000. The mmap arena MUST be `0x20000000..0xbfbfffff`, disjoint from the
frozen main-stack reservation. Mmap, surfaces and additional thread resources
MUST use this arena with at most 256 mapping extents per process. Mmap MUST
support exactly MAP_PRIVATE|MAP_ANONYMOUS, fd=-1 and offset=0. It MUST treat
hint as an optional page-rounded placement preference, use first-fit from the
arena bottom otherwise, and never replace existing mappings. MAP_FIXED,
MAP_SHARED and file-backed requests MUST fail EOPNOTSUPP. Bad combinations,
zero lengths, wraps and out-of-range results MUST fail EINVAL.

**[F2]** Private anonymous pages MUST be zero-filled, with all backing pages
or credits reserved before success; demand commitment MUST NOT permit later
physical overcommit. PROT_NONE MUST reserve VA and retain existing backing
contents if reached through mprotect. New PROT_NONE ranges MAY omit backing
until access is enabled; mprotect MUST reserve it transactionally then. Allowed
protections MUST be NONE, READ, READ|WRITE and READ|EXEC. W+X and WRITE/EXEC
without READ MUST fail EACCES. Every private anonymous mapping MUST have maximum
rights READ|WRITE|EXEC independently of its initial protection. Those maximum
rights MUST permit later mprotect transitions between any of the four allowed
combinations, including NONE to READ|WRITE and READ|WRITE to READ|EXEC and back;
they MUST NOT permit simultaneous W+X. Lowering protection MUST NOT lower these
maximum rights or discard contents. This is an intentional subset of
[POSIX mmap](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html).
Non-PAE i686 lacks NX: write/read protection MUST be enforced, but F2 MUST NOT
claim hardware prevention of instruction fetch from readable data pages.

**[F2]** Munmap/mprotect MUST require aligned starts and round positive lengths
up to pages with overflow checks. Munmap MUST support partial anonymous/surface
unmaps and harmless holes, but only within the arena. Mprotect MUST reject holes
with ENOMEM and requests beyond an object's maximum rights with EACCES; it MAY
restrict ELF data/image mappings but MUST NOT make original RX/R image pages
writable or change main-stack/TLS protections. Reserved/live stack/TLS/pinned
I/O ranges MUST fail EBUSY. Extent splitting MUST be prepared before mutation;
metadata exhaustion MUST leave all mappings unchanged. Brk(end) MUST be byte
granular, preserve a partial page, zero newly exposed bytes, release complete
pages on shrink and reject outside-heap addresses with EINVAL. Failed growth
MUST leave the old break unchanged. Libc brk MUST convert successful pointer
returns to 0 and sbrk MUST return the previous break.

**[F2]** CLOCK_MONOTONIC MUST start at zero at kernel timer initialization and
never decrease. CLOCK_REALTIME MUST use a valid UTC seed from the F1 read-only
RTC provider when that provider qualified under the
[device contract's RTC rules](device-firmware-ownership.md), with owned ports,
serialized index/data access, NMI-mask preservation, bounded UIP/seconds checks
and BCD/binary and 12/24-hour handling. Uname MUST then report realtime_source=1.
The kernel MUST record the RTC seed and its corresponding MONOTONIC sample;
REALTIME MUST advance by elapsed MONOTONIC time from that sample. If no qualified
valid RTC seed is available, REALTIME MUST instead use the canonical build's
recorded UTC epoch plus MONOTONIC, and uname MUST report realtime_source=0.
That fallback MUST NOT be presented as measured wall time. F2 MUST consume the
provider result without adding any port access or CMOS writes; a clock-setting
interface remains excluded. The provider's read discipline follows the device
contract's [MC146818 reference](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/rtc/rtc-mc146818-lib.c).
CLOCK_PROCESS_CPUTIME_ID MUST accumulate scheduled execution time of all the
caller's threads, including kernel work on their behalf, with 1 ms accounting.
Clock_getres MUST report 1 ms for all three clocks; clock() MUST convert CPU time
to CLOCKS_PER_SEC=1000000. UTC MUST be the sole timezone in F2; gettimeofday
MUST derive REALTIME and truncate nanoseconds to microseconds.

**[F2]** Nanosleep MUST round deadlines upward to the 1 ms timer quantum and
never return success early; a zero request MUST return immediately. On EINTR
it MUST write the nonnegative remainder when requested; on success it MUST
write zero there. Negative seconds/bad nanoseconds MUST fail EINVAL and an
unrepresentable absolute deadline EOVERFLOW. Tick conversion MUST use checked
64-bit arithmetic. Timing evidence MUST distinguish QEMU icount time from
host elapsed time as required by F0.

### Signals, faults and signal return

**[F2]** Only the signal numbers listed above MUST be supported; kill signal
zero MUST test target existence/authority without delivery. SIGKILL MUST be
uncatchable and unmaskable. Sigaction MUST reject attempts to set its action;
mask operations MUST silently clear its bit. Other unsupported bits/signals
MUST fail EINVAL. A signal bit MUST be `1ULL << (signo-1)`. Dispositions MUST
be process-wide, masks per thread. Async process-directed signals MUST select
the lowest-TID eligible live thread or stay process-pending; thread-directed
signals MUST remain pending for that thread. Repeated standard signals MUST
coalesce. SIGCHLD's default action MUST be ignore; all other supported defaults
MUST terminate the process without a core file.

**[F2]** A pending signal with a fatal default action MUST be eligible when
unmasked for the target thread, even if that thread has an active handler.
It MUST terminate the process through safe I/O quiescence and teardown, without
entering a catcher or returning EINTR. A masked fatal default signal MUST remain
pending; SIGKILL MUST bypass both the mask and the active-handler restriction.

**[F2]** Sigaction MUST accept only flags zero or SA_SIGINFO. When installing
a catcher (handler other than DFL/IGN), both handler and restorer MUST point
into mapped executable image code, or installation MUST fail EINVAL. DFL/IGN
actions MUST ignore the restorer. A null new-action pointer MUST leave
the action unchanged. Sigprocmask with null set MUST ignore how and query only;
otherwise it MUST accept exactly the three mask operations above. Pending
signals with SIG_IGN disposition MUST be discarded, except synchronous fault
signals, whose fatal rule below MUST apply. Of several eligible pending
signals, the lowest signal number MUST be delivered first; SIGKILL MUST take
precedence regardless of mask or active handler.

**[F2]** Kill MUST accept pid>0 for one process, pid=0 for the caller's group,
or pid=-pgid for that same group; -1 MUST fail EINVAL. An existing positive PID
in another group, or any other group target, MUST fail EPERM. An absent positive
PID MUST fail ESRCH. Supervisor PID 1 MUST be excluded from public kill targets
with EPERM. Thread_kill MUST target only a live thread of the calling process.
SIGPIPE MUST be generated for channel sends to a closed peer, remaining pending
when blocked and discarded when ignored; the syscall result MUST remain EPIPE
when the process survives.

**[F2]** User exceptions MUST map as follows: #DE and #MF to SIGFPE; #UD to
SIGILL; invalid/protection #PF and #GP to SIGSEGV; #AC to SIGBUS. Kernel fixup
faults MUST remain EFAULT, and lazy #NM MUST remain an FPU ownership event.
F2 MUST set CR0.AM while ordinary startup EFLAGS.AC remains clear, permitting
controlled user alignment faults without changing normal access behavior.
Other user exceptions MUST terminate with SIGSEGV and retain their vector in
diagnostics. An executable mapping is a software label without NX enforcement.
Blocked/ignored synchronous fault signals MUST terminate immediately; no thread
MUST loop indefinitely on an undeliverable fault. This is the F2 handler
extension anticipated by the earlier FPU/fault sections; F0 probe payloads
MUST retain their terminate-and-report path without installing handlers.

**[F2]** Delivery MUST occur on return to user mode, or immediately for the
faulting user thread, with no kernel lock held. SA_SIGINFO handlers MUST receive
`(signo,siginfo*,ucontext*)`; other handlers MUST receive signo only. The signal
and action mask MUST be blocked until return. F2 MUST support one active handler
per thread: async signals with catchers MUST remain pending for that thread
while it runs, including during blocking syscalls inside the handler. After
successful sigreturn their eligibility MUST depend on the restored mask.
Fatal default actions MUST obey the separate rule above. Any synchronous fault
in a handler MUST terminate the process with the new signal. An unwritable or
exhausted user stack MUST terminate with SIGSEGV; F2 has no alternate stack.
These are explicit restrictions on
[POSIX signal actions](https://pubs.opengroup.org/onlinepubs/9799919799/functions/sigaction.html).

**[F2]** Handler entry MUST clear live DF, TF and AC while preserving their
interrupted values in the context. After saving the interrupted x87 image the
kernel MUST give the handler a clean reset x87 environment and data state;
otherwise a pending unmasked x87 exception could immediately fault the handler.
Sigreturn MUST restore the saved, validated x87 image, including deliberate
handler repairs, through the existing lazy ownership machinery. No handler
MUST inherit another thread's FPU state. Siginfo fault_addr MUST be CR2 for #PF,
the faulting EIP for #UD/#DE/#MF/#GP, and zero for #AC (whose operand address
the processor does not report). Vector/trap_error MUST be zero for explicit
signals and SIGCHLD; unused fields MUST be zero. SIGCHLD code MUST be 0 in
this subset; waitpid MUST supply the exit status.

**[F2]** `ciuki_ucontext` MUST occupy 256 bytes: u32 size=256 at 0, flags=0 at
4, link=0 at 8, stack_base at 12, stack_bytes at 16, stack_flags=0 at 20;
saved u64 mask at 24; nineteen u32 general slots at 32 in the order
`gs,fs,es,ds,edi,esi,ebp,esp,ebx,edx,ecx,eax,vector,error,eip,cs,eflags,user_esp,ss`;
u32 cr2 at 108, fp_format=1 at 112, 108-byte 32-bit FNSAVE-format x87 image
at 116, and 32 zero padding bytes at 224. Both ESP slots MUST contain the
interrupted user ESP. FPU reserved bytes MUST be zero; selectors MUST refer
only to user segments. A thread without previous FPU use MUST expose initialized
reset state, not another thread's registers. The kernel MUST save the actual
lazy owner first and translate its FXSR representation if needed. User SSE
MUST stay disabled, so no XMM state is part of version 1.

**[F2]** The signal frame MUST occupy 320 bytes at a four-byte-aligned address
below interrupted ESP: offset 0 restorer address, 4 signo, 8 pointer to siginfo
at frame+32, 12 pointer to context at frame+64, 16 frame size=320, 20 version=1,
24:u64 kernel-generated per-delivery token, then siginfo and ucontext. Entry
ESP MUST equal frame base. On handler return the restorer MUST derive that
base from ESP-4 and issue sigreturn. The kernel MUST retain the active address,
token, restorer and original immutable context fields in supervisor memory.
`siginfo.code` MUST be 0 for kill/thread_kill, 1 for SEGV_MAPERR/ILL_ILLOPC/
BUS_ADRALN/FPE_INTDIV as applicable, 2 for SEGV_ACCERR, and 7 for x87 faults
(the saved x87 status supplies finer diagnosis). Sender_pid MUST be zero for
CPU faults, caller PID for explicit signals and child PID for SIGCHLD.

**[F2]** Sigreturn MUST accept only the current thread's active frame and
matching token, size and version; it MUST validate and copy the entire frame
before restoring. The caller MAY edit GPRs, EIP, both equal ESP values, permitted
arithmetic flags, signal mask and x87 environment/registers. EIP MUST name a
mapped executable region and ESP a writable user stack location. CS/SS and
data selectors MUST remain their original user values; vector/error/CR2,
stack description, TLS base and reserved fields MUST remain unchanged. IF and
reserved EFLAGS bit 1 MUST remain set; IOPL/NT/VM/VIF/VIP MUST remain clear;
only CF/PF/AF/ZF/SF/TF/DF/OF/AC MAY change. FPU reserved fields and user pointer/
selector fields MUST be sanitized before hardware restore. A bad frame MUST
terminate only this process with SIGSEGV. A successful sigreturn MUST replace
the saved syscall frame entirely, never return as an ordinary EAX result.
Longjmp out of a signal handler MUST remain unsupported; a handler MUST return
normally or terminate through _exit.

### Desktop surfaces, input and channels

**[F2]** The desktop/compositor MUST be an ordinary ring-3 process. Only the
bootstrap supervisor MUST install its exclusive display/input grants; clients
MUST receive a channel endpoint through explicit spawn inheritance and submit
shared surfaces to the desktop. The desktop MUST composite into its own output
surface and call present. No syscall MUST expose a framebuffer physical address,
device MMIO or controller ports. Panic/diagnostic console ownership MUST remain
serialized with F1 presentation; losing the desktop MUST leave a kernel console
fallback and MUST NOT corrupt unrelated processes.

**[F2]** Surface format 1 MUST be XRGB8888, little-endian B,G,R,ignored-X,
four bytes per pixel; width/height MUST each be 1–2048, stride=width*4, and
allocation length=page-rounded stride*height, at most 16 MiB. Geometry and
allocation arithmetic MUST be checked before allocation. Surface_create MUST
return a read/write description; transferred surface descriptions MUST have
read-only mapping rights. Surface_map MUST map the complete object in the mmap
arena, retaining an independent backing reference, and MUST reject executable
or private/COW requests. Closing a surface fd MUST NOT invalidate mappings;
munmap or process death MUST release them. A mapping MUST never grant access
to bytes outside the zero-initialized rounded allocation. A client MAY race
its own drawing, but MUST NOT be able to change geometry through shared pixels.

**[F2]** `ciuki_surface_info` MUST be 24 bytes: six u32, in order size=24,
width, height, stride, format=1, allocation_bytes. `ciuki_framebuffer_info` (syscall 67) MUST be
48 bytes: twelve u32, size=48, width, height, pitch, bpp, red_size, red_pos,
green_size, green_pos, blue_size, blue_pos, generation. Display generation MUST
identify the F1 active lease and MUST NOT authorize runtime modesetting.
`ciuki_rect` MUST be 24 bytes: i32 src_x, src_y, dst_x, dst_y, then u32 width,
height. The presenter MUST intersect source and destination bounds preserving
their translation, accept empty/clipped-away rectangles as no-ops, reject
signed endpoint overflow with EINVAL, and convert XRGB8888 into F1's qualified
pixel masks/pitch. It MUST synchronously pin backing and wait for copy completion;
there is no vsync, fence, DMA, blending or GPU acceleration promise.

**[F2]** The exclusive input grant MUST read the F1 queue without touching
devices. An event MUST be 40 bytes: u32 sequence at 0, source at 4,
generation at 8, type at 12; u64 monotonic_ns at 16; i32 code at 24,
value at 28, value2 at 32; u32 lost_count at 36. Types MUST be KEY=1
(code=PS/2 set-1 base scan code, E0 extended codes OR 0x100, Pause=0x200;
value 1=down, 0=up, 2=repeat), TEXT=2 (code=Unicode scalar), MOTION=3
(value=dx,value2=dy, positive right/down), BUTTON=4 (code 1=left,2=right,
3=middle; value down/up), RESYNC=5. **[F1-42]** WHEEL=6 adds signed
detents in value (positive away from the user), without pointer motion;
source=3 identifies USB. Existing source 1=native PS/2 and 2=firmware
retain their meanings. Unused fields MUST be zero. Both F1
backends MUST normalize to these values; missing physical transitions MUST
remain an F1 qualification failure.

The current F1 native `i8042.c:keyboard_byte` stores set-2 positions, and
`input.h:INPUT_KEY_A`/`input_unshifted` use that space (A=0x1c). **[F2]** The
native decoder MUST translate set 2 to the public set-1 code space at decode
time, before updating key state or enqueueing an event (A=0x1e, E0 keys with
0x100, Pause=0x200). The firmware backend already yields set 1; it MUST preserve
that code space. Both backends, their text/digest helpers and the F1 input probe
MUST share one public key-code table, without backend-specific public codes.
The upstream reference for firmware scan-code handling is
[SeaBIOS keyboard decoding](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c).

**[F2]** Input queue capacity MUST be 256 events. Overflow MUST clear stale
queued events, emit one RESYNC with cumulative lost_count and current button
bitmap in value (bits 0–2), and permit new transitions; the desktop MUST release
its remembered keys on RESYNC. Empty blocking reads MUST wait interruptibly;
O_NONBLOCK MUST produce EAGAIN. EOF MUST NOT be fabricated on device failure;
revoked/quarantined sources MUST return EIO and absent input ENODEV. Present
and display_info MUST return ENODEV if no LFB was qualified; the no-LFB text/
serial fallback MUST still support console/test operation.

**[F2]** Each channel direction MUST queue at most 64 messages. A
`ciuki_message` MUST occupy 288 bytes: u32 length at 0, fd_count at 4,
i32 fds[4] at 8, u32 sender_pid at 24, reserved=0 at 28, u8 data[256]
at 32. Send MUST require length<=256 and fd_count<=4, sender_pid=0 on input,
zero unused fd slots/payload bytes, and only owned surface fds as attachments.
Invalid attachment kinds MUST fail EBADF. The kernel MUST overwrite sender_pid
with the true sender and create read-only references to attachments. Receipt
MUST allocate fresh CLOEXEC fds in the receiver. It MUST copy bytes/fds and
dequeue as one transaction; bad buffers or insufficient fd slots MUST leave
the head message queued. Oversize length/count MUST return EMSGSIZE.

**[F2]** Send MUST copy/pin the entire message before waiting and MUST retain
attachment references while queued; closing the sender's fd MUST NOT invalidate
them. O_NONBLOCK or DONTWAIT MUST select EAGAIN on full/empty queues. Closing
the last reference to an endpoint MUST release messages destined to it and wake
peers; already-queued messages from that endpoint MUST remain readable until
drained, then receive MUST return zero. Endpoint transfer inside messages MUST
be excluded, preventing reference cycles. Endpoint duplication and inheritance
MUST count as ordinary references. A client fault MUST close its endpoints and
release all its surfaces without terminating the desktop or another client.

**[F2]** Bootstrap MUST give the desktop a separate process group and SHOULD
spawn each application in a new group, so the explicit own-group signal policy
does not grant clients authority over the desktop. Client protocol, window
management, Ciuki assets and UI remain desktop code directives; these syscalls
MUST NOT encode window titles, theme details or application policy.

**[F2]** Surface mappings MUST inherit maximum rights from the description
that created them; a later close or duplicate MUST NOT increase these rights.
Transferred descriptions MUST report O_RDONLY through F_GETFL; created surfaces
MUST report O_RDWR. Surface, channel and device-grant objects MUST reject
O_APPEND via F_SETFL with EINVAL. Input grants MUST report O_RDONLY, display
grants O_WRONLY and channel endpoints O_RDWR. Only O_NONBLOCK MUST be mutable
on these objects; byte read/write on them MUST fail EBADF. Unknown/stale fd
numbers MUST fail EBADF before grant authorization; an owned descriptor of the
correct kind without the required grant rights MUST fail EACCES. A valid
display/input grant MAY be duplicated within the same process, but MUST NOT
escape that process through inheritance or channel attachment.


## F4-01 amendment: copied audio device control (frozen 2026-10-11)

Syscall **69**, `CIUKI_SYS_DEVCTL`, uses B=fd, C=operation, D=buffer,
S=byte size, returns 0 or a negative errno. No F0/F2 number changes.
All records are little endian, four-byte aligned, version=1 at byte 0 and
size at byte 4. Size must equal both the record size and S. Reserved words
must be zero. Unknown operation: ENOTTY; unknown version: EOPNOTSUPP;
wrong size/reserved/range: EINVAL. The entire record is copied and its writable
user range pinned before mutation or waiting; getters also take a header and
reserved zeros. No hardware address or user DMA mapping is returned.

| Operation | Number | Record / bytes |
| --- | --- | --- |
| AUDIO_GET_CAPS | 0x4101 | ciuki_audio_caps / 80 |
| AUDIO_SET_FORMAT | 0x4102 | ciuki_audio_format / 32 |
| AUDIO_GET_STATUS | 0x4103 | ciuki_audio_status / 112 |
| AUDIO_SET_GAIN | 0x4104 | ciuki_audio_gain / 24 |
| AUDIO_DRAIN | 0x4105 | ciuki_audio_command / 16 |
| AUDIO_RESET | 0x4106 | ciuki_audio_command / 16 |
| MIXER_GET_MASTER | 0x4201 | ciuki_mixer_master / 32 |
| MIXER_SET_MASTER | 0x4202 | ciuki_mixer_master / 32 |

The exact fields below are consecutive u32 unless marked u64/i32. All records
start with version,size. U8=1, S16LE=2; caps formats uses bits 0/1.

| Record | Remaining fields, in byte order |
| --- | --- |
| caps | formats, min_rate, max_rate, output_rate, max_streams, ring_frames, period_frames, published_periods, backend (see transport IDs below), generation, vendor, device, subsystem (sub_vendor low 16), revision, codec_id, drain_ms, reserved[2] |
| format | rate, format, channels, output_rate (input zero), reserved[2] |
| gain | left, right, reserved[2] |
| command | reserved[2] |
| status | queued_frames, state (0=idle,1=started,2=failed), output_rate, generation, u64 accepted_frames, consumed_frames, output_frames, fetched_frames, source_starvation, fifo_errors, dma_halts, deadline_misses; i32 last_error; stream_generation, max_service_us, max_irq_us, transport_frames, latency_frames |

| master | left, right, mute (0/1), save_state (0=defaults,1=saved,2=applied_unsaved), i32 save_error, reserved |

f4-31 assigns the former final reserved status word (offset 108) to output
`latency_frames`, retaining v1/112-byte layout. Status has no reserved input
words and can be reused for queries. Latency is the current source queue
converted upward to output frames, plus outstanding DMA frames and the caps
drain allowance, saturated at UINT32_MAX. It estimates the delay to the last
accepted frame from the latest worker cursor; it is not a measured analog
DAC position. `transport_frames` keeps its prior meaning. Native write on a
stopped, healthy device establishes RUN before returning successfully.
When its source queue is empty, latency uses that stream's last mixed output
position instead of subsequent sources/idle silence. A stream with no accepted
data has zero latency. Music position and EOF use this stream-specific delay.

F4-11 transport IDs (record layout and existing commands unchanged): 1 is the
Intel-layout AC'97 engine (Intel ICH, SiS 7012, nForce/nForce2), 2 ES1370,
3 ES1371/ES1373/CT5880, 4 ISA SB16, 5 VIA VT82C686, 6 VIA VT8233/VT8235.
`output_rate` is the effective physical rate: 44,100 Hz on ES1370/SB16,
48,000 Hz on the AC'97 transports. The default *input* remains 48,000 Hz;
SET_FORMAT reports both rates and never substitutes the requested source rate.
Generated/fetched/transport frame counters use physical output frames. DOS
DSP/event/OPL time and their source taps remain at their existing 48 kHz clock.
For write-only AK4531, `codec_id=414b4531h` is a software model tag derived
from the ES1370 identity; it is not a codec register read. SB16 reports the
observed DSP major/minor version in that field. The diagnostics cursor fields
retain native transport units: ICH PICB in words, SiS in bytes, cyclic engine
remaining counts in stereo frames; no field means audible completion. See
[transport decisions and sources](f4-audio-second-set.md).

SET_MASTER input save_state/save_error must be zero. Volumes/gains are 0..100.
A successful SET_MASTER applies immediately and reports an unsuccessful save
in save_state/save_error; a codec failure returns EIO. Public mixer descriptions
are read-only, and SET_MASTER returns EACCES. Only the bootstrap supervisor may
install a non-inheritable O_RDWR control description into a trusted process;
dup retains that authority in that process. Ordinary spawn and attachments
cannot acquire it. Audio dup/inheritance share one stream, including status flags.
Status separately exposes the unconsumed source queue and the current published
transport queue in frames; lifetime generated/fetched counters include earlier
runs and cannot be subtracted to infer the current queue after a stop.

Complete devctl errors: EBADF, EFAULT, ENOTTY, EOPNOTSUPP, EINVAL, EACCES,
EBUSY, ENODEV, ENOMEM, EINTR, EIO. DRAIN is I-class and waits for the snapshot
of accepted frames and its converted/published tail plus drain_ms. RESET is
nonblocking, discards uncommitted frames and changes stream generation; published
periods remain owned. SET_FORMAT requires reset after the first queued frame.
AUDIO open additionally permits ENODEV/ENOMEM; byte writes retain F2 positive
partial-count precedence over EINTR/EIO. Device seeks/positioned I/O: ESPIPE.
No ioctl, poll/select, recording or master authority through public opens.

### F4-08 amendment: hardware observations

`MIXER_GET_DIAGNOSTICS` (4203h) is a read-only operation on `/dev/mixer`,
including when audio activation failed or the device is absent. It follows
the copied, pinned version/size/reserved rules above and needs no master grant.
The 64-byte `ciuki_audio_diagnostics` record has version/size at 0/4;
codec_id/controller at 8/12 (controller = PCI vendor << 16 | device);
flags/irq_line/civ/picb at 16/20/24/28; u64 fetched_frames/irqs at 32/40;
i32 last_error at 48; u16 power/master/pcm/rate at 52/54/56/58; reserved u32
at 60. Flags are ready=1, running=2, quarantined=4. Fields are cached kernel
observations, not physical addresses or an assertion of audible sound.
Fetch and owned-IRQ counters are monotonic over transport stops. Getters
accept a returned record again with its reserved word zero. No existing
record size or operation number changes.

## F2-33 amendment: process enumeration and termination requests (2026-10-11)

Directive f2-33 (Task Manager). Implementer draft, lead review pending. No
F0/F2/F4 number changes; both calls are unadvertised to the F2 gate programs
(no probe or SDK test issues them) and every other unassigned number keeps
returning ENOSYS.

Research: the Linux [`/proc/PID/stat` fields](https://docs.kernel.org/filesystems/proc.html)
(state, ppid, pgrp, utime in jiffies, num_threads, start time after boot, rss)
fix the per-process set; the [POSIX kill permission rule](https://pubs.opengroup.org/onlinepubs/9799919799/functions/kill.html)
(EPERM without permission, ESRCH without a target) the error vocabulary;
Windows separates [EndTask](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-endtask)
(WM_CLOSE first, then forced) from [TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)
(unconditional, needs PROCESS_TERMINATE on the handle). Ciuki keeps the close
path in the desktop (f2-30 CLOSE_REQUEST, SIGTERM after 2 s) and models the
access right as a delegated descriptor, as the F4-01 mixer control handle does.

| No. | Call and register signature | Success / semantics | Block | Complete errors |
| --- | --- | --- | --- | --- |
| 70 | `proc_list(B=buffer*,C=bytes)` | Number of process records written after one system record; readable by every process. | N | EINVAL, EFAULT |
| 71 | `proc_terminate(B=pid,C=signal,D=authority_fd)` | 0 after posting SIGTERM or SIGKILL; D=0xffffffff means no authority. | N | EINVAL, EBADF, EPERM, ESRCH |

**Call 70.** `bytes` must be 64..65536 (`CIUKI_PROC_LIST_MIN/MAX`), otherwise
EINVAL. The whole `[buffer, buffer+bytes)` range must be mapped writable
(EFAULT, checked before any side effect). The kernel takes a consistent
snapshot with interrupts held off into one static 5184-byte buffer (the
kernel heap stops at 2 KiB objects; nothing between snapshot and copy-out
waits and kernel code is not preempted, so calls never share it), then stores
the 64-byte system record and
`min(64, (bytes-64)/80)` process records; the unused suffix stays untouched.
Records follow in ascending PID order and include zombies.

| Record | Size | Fields in byte-offset order |
| --- | --- | --- |
| `ciuki_proc_system` | 64 | 0:u32 version=1, 4:u32 size=64, 8:u32 tick_hz=1000, 12:u32 records (written), 16:u64 total_ticks, 24:u64 idle_ticks (timer ticks taken by the idle task), 32:u32 usable_pages, 36:u32 free_pages, 40:u32 kheap_pages, 44:u32 kheap_bytes (in use), 48:u32 kheap_high_water (bytes), 52:u32 processes (published, may exceed records), 56:u32 reserved[2]=0 |
| `ciuki_proc_record` | 80 | 0:u32 version=1, 4:u32 size=80, 8:u32 pid, 12:u32 ppid (0 for PID 1), 16:u32 pgid, 20:u32 state (1 live, 2 stopping, 3 zombie), 24:u32 threads (live threads; PID 1 reports its one reaper task), 28:u32 resident_pages (owned backing pages incl. mapped shared surfaces), 32:u64 cpu_ticks (ticks charged while a thread of the process ran), 40:u64 start_tick (g_ticks at publication), 48:char name[32] (image basename, printable ASCII, NUL-terminated, zero-filled; empty for kernel controller owners) |

**Call 71.** Validation order: signal must be SIGTERM or SIGKILL and pid > 0
and not the caller (EINVAL); a D other than 0xffffffff must name an open
authority descriptor of the caller (EBADF, also for any other descriptor
kind); PID 1 gives EPERM; a missing, unpublished, stopping or zombie target
gives ESRCH; then permission: a target in the group of the live desktop that
PID 1 bootstrapped is refused (EPERM) unless the caller is that desktop (its
group ends only through the desktop's close path); with an authority descriptor, a target in the
group of the descriptor's owner is refused (EPERM) unless the caller is the
owner, so a delegate never ends the desktop or its group even where PID 1
recorded no desktop; a target without live user
threads (a kernel controller owner) is refused (EPERM); a descendant of the
caller is allowed; any other target requires the authority, honoured only
when the caller is the descriptor's owner or the owner's direct child (EPERM
otherwise). Success posts the signal exactly like `kill`; a caught or ignored
SIGTERM is the target's choice, SIGKILL is not. `kill` (54) keeps its
group-local rule unchanged.

**Authority descriptor.** `supervisor_spawn(..., desktop=true)` installs one
authority object at the lowest free fd >= 3 of the trusted desktop, FD_CLOEXEC
clear, and passes its number as `CIUKI_TASKS_FD`. Default inheritance covers
only fds 0-2, so it never travels implicitly; the desktop names it in the
explicit spawn list (target fd 4) only for `/Programs/tasks`. Close, dup, dup2 and
fcntl treat it as a generic object; fstat gives EOPNOTSUPP, byte I/O EBADF.
The object is freed with its last descriptor. The F0/F2 ledgers count it as a
handle of its holder; no device, description or grant counter changes.

## F3-02 amendment: DOS window virtual machine (2026-10-11, pending lead review)

Syscall **72**, `CIUKI_SYS_DOSVM`, uses B=operation, C=record, D=byte size
and returns 0 or a negative errno. No F0, F2 or F4 number changes. The
record `ciuki_dosvm_command` is 136 bytes, little endian, four-byte aligned,
version=1 at byte 0 and size at byte 4; size must equal both 136 and D.
Every field an operation does not take, outputs included, must be zero on
input. The record is copied, then its user range is pinned writable before
any reservation or sleep; results are copied back only on success.

| Bytes | Field | Use |
| --- | --- | --- |
| 0, 4 | version, size | 1, 136 |
| 8, 12 | id, generation | VM key from CREATE; input of every other operation |
| 16 | i32 surface_fd | CREATE: owned writable 640x480 XRGB8888 surface (stride 2560) |
| 20, 24 | state, reason | STATUS/CREATE output: 0 booting, 1 running, 2 stopped; reason when stopped (0 exit, 1 terminate, 2 fault, 3 reset, 4 timeout); STATUS launch failure: DOS EXEC error when known |
| 28 | focused | FOCUS input 0/1; STATUS output |
| 32 | changed | PRESENT/PRESENT_BANDS output: 1 when rows were written; STATUS: VIDEO_READY=1, LAUNCH_FAILED=2 |
| 36 | video_mode | output: guest BIOS mode (BDA 0049h) |
| 40 | name[32] | CREATE: 1-31 printable ASCII bytes, NUL, zero padding; STATUS failure: last console context, at most 31 bytes and NUL |
| 72 | ciuki_input_event / presentation | INPUT: one desktop event with the F2 validation rules; PRESENT_BANDS: low/high u32 destination band mask at 72/76, remaining 32 bytes zero |
| 112 | ciuki_rect | PRESENT output: damage rows, src=dst, full 640-pixel width |

| Operation | Number | Semantics |
| --- | --- | --- |
| CREATE | 1 | Admit, create and boot one DOS VM bound to the caller and the surface; one VM per process. |
| PRESENT | 2 | Render changed 8-source-line bands into the bound surface; completion writes or programmed refresh for direct chain-4, integer scale, centred, black borders. |
| INPUT | 3 | Requires focus. KEY events reach the guest keyboard (US layout BIOS words until the f3-01 device path); TEXT, MOTION and BUTTON are accepted and have no consumer yet. A full guest queue returns EAGAIN. |
| FOCUS | 4 | Set focus; losing it releases held modifiers. |
| CLOSE | 5 | Destroy the VM and drop the surface reference; a running VM is forcibly terminated. |
| STATUS | 6 | State, reason, focus and video mode. |
| LAUNCH | 7 | Once DOS is ready, arm command visibility immediately before typing its final Enter. EAGAIN before ready, EBUSY if already armed. The prompt does not arm it. |
| PRESENT_BANDS | 8 | PRESENT plus an exact mask of destination 8-line bands written by this call; bits 60-63 are zero. |

F3-13 additions preserve version 1, size 136 and every existing offset. All
outputs must still be zero on input. An armed launch reports VIDEO_READY only
after observing DOS EXEC and a supported video-mode set, BIOS drawing or VGA
store. Its presentation is suppressed until then. An EXEC error or a return to
keyboard input before any frame sets LAUNCH_FAILED; the native client retains
an error panel. A direct CPU store is observed through the page dirty bit.
Presentation retains the chain-4 window; BIOS/model mutations, latch semantics,
teardown and digest/model consumers still close and harvest it.

Window VM workers keep the normal run queue and starvation guard. Their
quantum is capped at 4 ms without focus and 8 ms with focus (or the configured
normal quantum, if shorter). This doubles their share among runnable peers
when focused while bounding the delay before a native client can poll/present.
Probe workers and all other tasks retain their ordinary quantum and priority.

Authority is not a descriptor. The kernel marks a process at spawn only when
its exact path is `/Programs/dosbox` and its parent is the bootstrap supervisor or
owns both a display and an input grant (the desktop); grants never leave
their owner, so no other launcher can confer it. Keys belong to the owner:
another process, a stale generation or a closed VM gets ESRCH. Owner death
(exit, signal, fault) makes the supervisor's collection pass destroy the VM
through the idempotent f3-00 cleanup and release the surface. The kernel
writes the shared surface pages directly; no framebuffer address, VM memory
or device is exposed, and the desktop composites the surface like any other
client window.

Complete dosvm errors: EPERM (no authority), EINVAL, EFAULT, EOPNOTSUPP
(version), EBADF (surface descriptor), EACCES (read-only surface; input
without focus), EBUSY (caller already owns or is creating a VM; operation in
progress), ESRCH, ENOMEM (admission or allocation), ENOSPC (VM or creation
slots), EAGAIN, EINTR (owner stopping during creation), EIO (private disk or
boot failure, presenter write failure).
## F3-04 amendment: DPMI client entry path (2026-10-11)

**[F3]** GDT index 8 (selector 40h) is the per-VM LDT descriptor. It is
loaded with LLDT by `dosvm_task_switch` (called from the scheduler after
`ESP0` is set) when a DOS VM worker with an active DPMI client is switched
in, and LDTR is null for every other task; F2's TLS descriptor stays at
index 7. The F0 protection probe now expects a nine-entry GDT, LDTR null and
index 8 not present outside a DPMI worker.

**[F3]** DPMI clients run at CPL 3, IOPL 0, EFLAGS.VM=0, in the VM's address
space and LDT. `INT n` through the DPL 0 gates arrives as #GP(n*8+2) and is
decoded by the VM monitor (INT 31h services, virtual interrupts); `INT3`
and `INT 80h` use their DPL 3 gates and are delivered to the client's
protected-mode vectors 3 and 80h. `trap.c` routes every trap of a worker
whose saved frame is a client frame (`dosvm_client`) to `dosvm_trap` before
the native fault and syscall paths, after acknowledging hardware IRQs; a
DPMI client never reaches `syscall_dispatch`. Kernel-mode debug traps caused
by a VM's DR0–DR3 watchpoints are dismissed with RF; the debug registers are
loaded per VM by the same switch hook.

**[F3]** `v86_enter` copies a 92-byte V86 frame when EFLAGS.VM is set and a
76-byte ring-3 frame otherwise; the VM worker re-enters after every mode
change. Every client frame passes `dpmi_frame_check` before the kernel IRET.

## Power control amendment: system call 73 (2026-10-12)

Syscall **73**, `CIUKI_SYS_POWER`, takes B=action and returns a
non-negative value or a negative errno. No other number changes. Constants
are in `ciuki/abi.h`; the SDK wrapper is `int ciuki_power(uint32_t action)`
in `ciuki/power.h` (`-1` with errno on failure).

| Action | Value | Semantics |
| --- | --- | --- |
| `CIUKI_POWER_REBOOT` | 1 | Flush storage, then `kernel_reboot()` (8042 reset pulse, then triple fault). Does not return on success. |
| `CIUKI_POWER_OFF` | 2 | Flush storage, then ACPI S5 soft-off. Returns `ENOTSUP` when no soft-off method was found and `EIO` when the S5 write left the machine running after 2 s; in both cases storage is already stopped and the caller shows "It is now safe to turn off your computer". |
| `CIUKI_POWER_QUERY` | 3 | Returns `CIUKI_POWER_CAN_REBOOT` (1) and, when ACPI soft-off was found, `CIUKI_POWER_CAN_OFF` (2). No side effect except the one-time table parse. |

**Authority.** Only the process whose PID is the supervisor's live desktop
(`supervisor_desktop_pid()`, the `/Programs/desktop` spawned by PID 1 with
`desktop=true`) may call it; every other caller gets `EPERM`, including
queries. Unknown actions give `EINVAL`.

**Storage.** REBOOT and OFF call `storage_sync()` (boot log shutdown, then
`storage_shutdown`) when writable storage is mounted and running. A failure
is returned unchanged before any power action: `EBUSY` while any process
still has a file open on a FAT volume (the desktop closes its applications
and DOS windows first), or the flush error. A FAT volume left dirty mounts
read-only on the next boot (`FAT_RO_DIRTY`), so no power action follows a
failed flush.

**ACPI soft-off.** Once per boot (on a normal boot right after the desktop
bootstrap, otherwise at the first QUERY or OFF) the kernel reads, through
cached `vmm_map_mmio` windows, the RSDP the loader recorded
(`acpi_rsdp_phys`), the RSDT (or, when absent and the RSDP revision is 2 or
more, the XSDT with entries below 4 GiB), the FADT (`FACP`, at least 116
bytes) and the DSDT, verifying each signature, length (DSDT at most 256 KiB)
and checksum. From the FADT it takes PM1a_CNT_BLK (offset 64) and
PM1b_CNT_BLK (68), or X_PM1a/X_PM1b_CNT_BLK (172/184) in SystemIO space when
the legacy field is zero, SMI_CMD (48), ACPI_ENABLE (52) and DSDT (40, or
X_DSDT at 140). In the DSDT AML it looks for `Name(_S5, Package)` or
`Name(\_S5, Package)` (NameOp 08h, optional RootChar 5Ch, PackageOp 12h) and
reads the first two elements as ZeroOp, OneOp or Byte/Word/DWord constants
of at most 7. OFF then, if SCI_EN (PM1 control bit 0) is clear and the FADT
names SMI_CMD and ACPI_ENABLE, writes ACPI_ENABLE to SMI_CMD and waits up to
3 s for SCI_EN; writes SLP_TYPx (bits 10-12) with SLP_EN clear, then
SLP_TYPx | SLP_EN (bit 13), to PM1a and, if present, PM1b. No AML method
(`_PTS`, `_GTS`) is executed. The parse result is logged once on serial and
in the boot log (`[power] acpi soft-off ...` or `... unavailable: step=`).
APM is not used.

Sources checked on 2026-10-12: FADT offsets, Linux
[`include/acpi/actbl.h`](https://raw.githubusercontent.com/torvalds/linux/master/include/acpi/actbl.h);
PM1 bits, Linux
[`drivers/acpi/acpica/aclocal.h`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/acpi/acpica/aclocal.h);
two-write sequence, Linux
[`drivers/acpi/acpica/hwsleep.c`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/acpi/acpica/hwsleep.c)
`acpi_hw_legacy_sleep`; AML opcodes, Linux
[`drivers/acpi/acpica/amlcode.h`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/acpi/acpica/amlcode.h);
QEMU [`hw/acpi/core.c`](https://raw.githubusercontent.com/qemu/qemu/master/hw/acpi/core.c)
`acpi_pm_cnt_write` (SLP_EN with SLP_TYP 0 requests a guest shutdown; SCI_EN
is not tested) and
[`hw/i386/acpi-build.c`](https://raw.githubusercontent.com/qemu/qemu/master/hw/i386/acpi-build.c)
(`_S5` = Package(4){0, 0, 0, 0}); SeaBIOS
[`src/fw/paravirt.c`](https://raw.githubusercontent.com/coreboot/seabios/master/src/fw/paravirt.c)
sets the PM base to 0x600 when QEMU provides `etc/table-loader`, so the
QEMU FADT reports PM1a_CNT 0x604. The T23 tables were not read: its values
come from its own FADT/DSDT at run time and are recorded by the log line
above. Host test: `tests/host/power_test.c` (QEMU-shaped and ICH-shaped
fixtures, refusals).


### f1-23 display ABI amendment

Syscall **74**, `CIUKI_SYS_DISPLAY`: EBX=CIUKI_DISPLAY_QUERY (0), ECX points to
`ciuki_display_info` (612 bytes, version=1 and size=612 supplied by caller),
or EBX=CIUKI_DISPLAY_SET (1), ECX is a uint16 mode id encoded in u32. EDX,
ESI, EDI and EBP are zero. The mode entry is 12 bytes. Query is public; set
requires the live supervisor desktop PID and exclusive display descriptor.
The `<ciuki/display.h>` wrappers return 0 or negative errno directly.
The original syscall-67 layout/function remain binary compatible under the
C record name `ciuki_framebuffer_info`; framebuffer lease generation conveys
no mode-setting authority. Reading exactly four bytes from the owner's display
fd returns its current nonblocking u32 mode-change counter. Complete successful
presents with the active lease generation acknowledge the 10 s switch watchdog.
See the [f1-23 acceptance amendment](f1-acceptance.md#amendment-f1-23--display-inventory-and-runtime-vbe-modes-2026-10-12)
for the precise structure, validation, firmware transaction and qualification.

## F2-39 amendment — launch diagnostics (2026-10-12)

Additive syscall 75, `LAUNCH_MARK`: `ebx=event`, `ecx=pid`,
`edx=click_us_low`, `esi=click_us_high`, `edi=ebp=0`. No wire record,
no change to desktop messages or process-list records. The F1-23 integration
keeps its specified display call at 74 and moves the new launch diagnostics
call from main's provisional 74 to 75; rebuild the SDK and diagnostic clients
with this merged header.
The SDK wrapper is `ciuki_launch_mark(event, pid, click_us)`.

Events: STATUS (0), BEGIN (1), CONFIGURE (2), DAMAGE (3), FRAME (4). STATUS
requires pid/timestamp zero and returns 1 when the console is verbose, 0 when
quiet. BEGIN requires pid zero and a CLOCK_MONOTONIC timestamp in microseconds
no later than the current clock. It records the click for the next spawn; even
a rejected or cancelled spawn consumes it. The other events require timestamp
zero and the PID of a direct child of the supervisor's desktop. Only that
desktop may issue markers (EPERM otherwise); unknown or unrelated PIDs give
ESRCH, invalid arguments/stage order EINVAL. Quiet mode accepts authorized
markers without collecting or emitting diagnostics.

CONFIGURE is marked after the initial committed configuration is queued. DAMAGE
is marked after the first valid initial damage message. FRAME is marked after
the desktop successfully presents all dirty regions containing that frame.
Duplicate stages do not reset times or emit duplicate launch records. A failed
present leaves FRAME pending. Minimized/collapsed windows wait for presentation
after restoration. Native surface mapping is timestamped by the kernel.

Kernel timestamps use an atomic PIT tick snapshot, expressed in microseconds
with 1000 us resolution. Wall-clock corrections cannot change them. Verbose
records identify the name and PID: `[launch-stage]` on ELF completion, `[launch]`
on first presentation, `[launch-detail]` for surface/configuration/damage/#PF,
`[launch-io]` for volume payload and physical cache-miss requests, and
`[launch-image]` for immutable ELF snapshot reads. All times are absolute
CLOCK_MONOTONIC timestamps; total_us is first_frame_us minus click_us.
Volume read_bytes counts returned payload bytes, including the entire immutable
ELF snapshot once, and excludes RAM snapshot copies. Physical disk_bytes includes
metadata and cache misses only. read_max/disk_max/image_max are requested bytes
per operation at the respective layer, not throughput. Successful physical
requests are counted; a failed request does not claim bytes transferred.

This is diagnostic N-class work: markers never wait or allocate. Serial emission
uses the existing klog sinks; launch timestamps are captured before emitting
the record. Counters stop at FRAME. No launch output is emitted in quiet boot.
Sources, host evidence and pending T23 qualification:
`docs/validation/f2-39-app-launch-latency.md`, `build/codex/f2-39-report.md`.


## F2-41 amendment — read-only devices (2026-10-12)

Call 76 `DEVICES`: `ebx=output`, `ecx=34696`, other argument registers
ignored. The SDK `ciuki_devices(struct ciuki_devices_info *)` supplies the
extent. The first two caller words must be version 1 and size 34696.
Returns 0 on success, EINVAL for an unsupported extent/version/size,
EFAULT for an inaccessible/overflowing full readable and writable range,
or EAGAIN if resource quarantine changes the generation during both bounded
snapshot attempts. The output is a pointer-free pack(4) record. Counts never
exceed 64 devices, 16 drivers and 64 leases; resource lists stop at 8.
Unfilled array slots, unused bytes and string tails are zero. The target
layout has literal expectations in `tests/host/abi_layout_test.c`.

Device states are ready (0), absent/inactive (1), quarantined (2),
firmware-first (3). PCI identity alone never proves a native binding. Lease
states preserve the registry's firmware (0), claimed (1), active (2),
quiescing (3), released (4), quarantined (5), discovered (6); released
leases are omitted. Resource types are I/O (0), IRQ (1), ISA DMA (2), memory
(3), with start inclusive/end exclusive. UNKNOWN_SIZE (bit 0) has end=start
and conveys a PCI BAR base without an extent. TRUNCATED (bit 0) in the
snapshot means device/PCI discovery reached a bound; in a device record
it means additional resources are omitted. CPU vendor/brand/family/model,
uptime milliseconds and current boot policy are read-only metadata.

N-class: no allocation, waits, hardware I/O, BAR sizing, probing, activation,
lease changes or quarantine recovery. A private static buffer follows call
70's UP non-preemptible-kernel rule. Registry capture holds a short IRQ lock;
CPUID and formatting run after restoring interrupts. Static snapshot storage
is approximately 38 KiB including its private registry copy. See
[device-manager.md](device-manager.md) for sources and Control Panel behavior.

### F3-20 amendment — DOS window full-screen command (2026-10-14)

Call 72 keeps its 136-byte version-1 command and existing owner delegation.
Operation 9 (`CIUKI_DOSVM_FULLSCREEN`) takes `focused=1` to enter, or zero to
return; entry requires the focused DOS window and a restorable desktop grant.
Operation 10 (`CIUKI_DOSVM_BACKGROUND`) takes `focused=1` for pause on Alt+Tab,
or zero (default) for continued windowed execution. All other input/output
fields must be zero. STATUS.changed gains bits 4 (full screen), 8 (suspended)
and 16 (entry refused, positive errno in reason). STOPPED and launch-failure
reasons retain precedence. There is no physical-address, arbitrary-port or
firmware entry in the native ABI. The kernel retains the window's surface and
binding across ownership changes. Physical access ends before VM destruction.

The kernel input filter consumes Alt+Enter and full-screen Alt+Tab make,
repeat and break edges. It routes full-screen keys and relative PS/2/USB mouse
reports directly to the existing virtual 8042/INT33h devices. A returned session
resynchronizes desktop input; Alt+Tab additionally supplies a balanced desktop
switch chord. Re-focusing or requesting full screen resumes a paused session.
The dated F3-20 amendment in `device-firmware-ownership.md` defines the precise
VGA/firmware authority and the hardware state readback limits.
## f5-03a — Win32 kernel transport freeze (2026-10-14)

The additive Win32 family is global call **96**, `CIUKI_SYS_W32`; native
0–5/16–77 retain their numbers and all existing ELF semantics; the number is
declared in a separate `enum ciuki_w32_syscall`, so `enum ciuki_syscall` is unchanged. 78–95 remain
unassigned here for F6. `EBX=record`, `ECX=exact size`, other argument registers
zero. The 16-byte version/size/operation/reserved header and fixed records in
`abi.h` are frozen by the dated f5-03a amendment of
[the personality contract](win32-personality-contract.md#f5-03a--kernel-record-freeze-and-research-2026-10-14).
Malformed transport uses -errno; authorized operations return NTSTATUS.
Core rows require Win32 personality; native service/display authority must be
assigned explicitly by later amendments. Fixed diagnostic size is 240 bytes;
i386 CONTEXT is 716 bytes, EXCEPTION_RECORD 80 bytes, x87 save area 112 bytes.
Win32 FS is GDT slot 9/selector 0x4B/base TEB/limit 8191. Native GS slot 7,
DOS LDT slot 8 and all previous selectors remain unchanged. SSE is enabled
only for Win32 with CPU FXSR+SSE; native ELF still observes #UD. Lazy state
transitions temporarily enable OSFXSR for XMM preservation. No native signal
record is extended. See the contract amendment for rounding, errors, safe
context restoration, loader handoff and emergency exception-stack semantics.
