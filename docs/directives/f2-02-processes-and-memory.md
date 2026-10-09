# Directive f2-02: processes, ELF loader, memory, threads and wait words

- **Step:** F2. **Contracts:** `execution-abi.md` F2 extension (types,
  register rules, validation order, blocking classes; syscalls 16–28,
  65–66: `spawn`, `waitpid`, `getpid`, `getppid`, `thread_create`,
  `thread_exit`, `tls_set`, `wait_word`, `wake_word`, `mmap`, `munmap`,
  `mprotect`, `brk`, `thread_join`, `thread_detach`; ELF loading and
  initial state; processes, inheritance and teardown; threads, TLS and
  private wait queues; memory), `posix-subset.md` (limits), `f2-acceptance.md`
  (probes `elf-load`, `spawn-wait`, `mmap`, `threads-wait`; T0 tests).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`.
- **Prerequisite on `main`:** f2-01 (`ciuki/abi.h`), f1-01 (VFS, for the
  ELF file read path; until f1-09 mounts volumes, the loader reads through
  a `struct ciuki_file_ops` boundary that the probes feed from an in-image
  payload, so the probes run before storage bring-up).
- **Worktree:** `wt/f2-process`. Files: new `src/kernel/proc/` (`process.c`,
  `elf.c`, `spawn.c`, `thread.c`, `uaddr.c` (mappings arena, brk, mprotect),
  `waitword.c`, `syscalls_proc.c`), headers under `src/kernel/include/ciuki/`
  (`process.h`, `elf.h`, `uaddr.h`, `waitword.h`), `src/kernel/core/syscall.c`
  (dispatch table extension only, 0–5 unchanged), `src/kernel/core/task.c`,
  `src/kernel/arch/desc.c` and `isr.asm` only where the TLS descriptor
  (GDT index 7, selector 0x3B, per-switch base reload) and the user frame
  require them, `src/kernel/probes/f2_probes_process.c`,
  `scripts/build_kernel.py` (add `proc`), `tests/host/proc/*`,
  `scripts/test/host_kernel_tests.sh`. Keep every F0 probe and syscall
  behaviour identical.

## What to build

1. **Process object** separate from `struct task`: address space, fd table
   (128 slots, filled by later directives; here only the table and the
   0–2/explicit inheritance snapshot logic), cwd reference (opaque until
   f2-03), signal disposition table (storage only; semantics in f2-04),
   process group, PID/PPID, zombie record, PID 1 supervisor task created
   at boot. Monotonic PIDs/TIDs through INT32_MAX, limits 64 processes,
   256 threads, 16 per process. Teardown in the contract's order; every
   step idempotent; a user fault never affects another process.
2. **ELF loader** per the restricted profile (header and program-header
   validation, PT_LOAD only, page-aligned, no W+X, 32 MiB bounds, entry in
   RX file-backed range, BSS and padding zeroing, snapshot read through the
   file boundary, full rollback on failure). Initial stack layout
   (argc/argv/envp/auxv with the three `AT_CIUKI_*` tags), registers, EFLAGS
   0x202, GS = TLS selector, x87 reset policy.
3. **Memory**: heap `brk` below 0x20000000; mmap arena 0x20000000–0xBFBFFFFF
   with ≤256 extents, first-fit, hints, private anonymous only, zero-fill,
   backing credits reserved before success, PROT_NONE semantics, maximum
   rights per extent, `munmap` partial/holes, `mprotect` transactional with
   EBUSY on pinned/stack/TLS ranges; main stack reservation and guard as
   specified.
4. **Threads**: `thread_create` with kernel-allocated stack+guard and TLS
   page in the arena, cdecl entry frame `(return_trampoline, arg)`, mask
   inheritance, x87 reset; `thread_exit` with join value retention;
   `thread_join`/`thread_detach` rules; `tls_set`; last-thread exit ends
   the process with status 0; the TLS descriptor reloaded on every switch.
5. **Wait words**: `wait_word`/`wake_word` keyed by (process, mapping
   generation, VA); compare-and-enqueue atomic against wake/unmap; FIFO
   wake; ECANCELED on unmap; absolute deadlines on REALTIME/MONOTONIC
   (REALTIME = MONOTONIC + the seed offset, provided by a small `clock.c`
   hook that f2-03 completes); EAGAIN on mismatch; EINTR plumbing point for
   f2-04.
6. **Syscall layer**: `copy_from_user`/`copy_to_user` with an exception
   fixup table (contract: a fault inside returns EFAULT), argument
   validation order, closed error sets per row, 64-bit arguments as lo/hi
   halves, results via checked pointers.
7. **Probes** (`f2_probes_process.c`, registered with `CIUKI_F1_PROBE`-style
   macro extended to F2 — add `CIUKI_F2_PROBE` and a `.f2probes` section in
   the same way; the `f2:` selector phase is wired by the runner directive):
   `elf-load`, `spawn-wait`, `mmap`, `threads-wait` exactly as the
   acceptance table, using ring-3 payloads built by the lead's SDK later;
   until the SDK exists, the payloads are small NASM programs assembled into
   the kernel image like the F0 payloads (`payload.asm`), covering the
   contract conditions that do not need libc.

## Host tests (mandatory)

`tests/host/proc/`: the ELF parser against valid and malformed fixtures
(every rejection rule, overflow, boundary sizes), the arena allocator
(first-fit, hints, splits, holes, limits, rollback), the wait-word queue
model with fake scheduling (compare/enqueue/wake/unmap races, FIFO,
deadlines), the stack/auxv builder (ARG_MAX, counts, alignment), and the
PID/zombie/reaping state machine (adoption by PID 1, double-reap refusal).

## Acceptance by the lead

Diff review; host tests; kernel build with audit; F0 suites unchanged;
the four probes pass on QEMU (`qemu-t23`, `qemu-e500`, `qemu-min128`) with
the interim NASM payloads; a report of the resource ledgers after 100
cycles. Reply with: files, interfaces (signatures), test output, and any
contract problem found.
