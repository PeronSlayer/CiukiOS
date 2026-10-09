# Directive f2-00: the F2 contract — POSIX subset, processes, SDK, acceptance

- **Step:** F2 (contract before code, as `foundations-transition.md`
  requires). **Inputs:** `execution-abi.md` ("Decision for F2", the frozen
  F0 syscalls, address space, ELF32, threads, FPU policy),
  `vfs-storage-contract.md` (paths, handles, share modes),
  `f0-acceptance.md` and `f1-acceptance.md` (evidence grammar, runner,
  tiers), `foundations-transition.md` row F2 (gate: an application crash
  leaves the desktop and other processes running; an identified upstream
  application builds against the CiukiOS SDK and passes named runtime
  tests), scope in `dev_diary/2026-10-09-10` and `README.md`.
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`, web search on.
- **Worktree:** `wt/f2-contract`. Files: `docs/design/execution-abi.md`
  (extend; keep every frozen statement), new `docs/design/posix-subset.md`,
  new `docs/design/f2-acceptance.md`, `docs/README.md` (index lines).
  Nothing else; no code.

## What to write

Plain, precise English; no marketing prose. Every normative statement uses
MUST/SHOULD/MAY. Cite primary sources inline (POSIX.1-2024 at
pubs.opengroup.org, the i386 System V ABI, the ELF specification, newlib
and musl documentation, the libc of your choice's porting guide).

1. **libc decision** (`posix-subset.md`): compare newlib (BSD-style
   licences, board-support porting model, reentrancy via `_reent`), picolibc
   and musl (MIT, Linux-oriented syscall layer, integrated pthreads) for a
   GPLv2 kernel with a statically linked userland: porting effort for our
   own syscall numbers, thread support, footprint on a 128–512 MiB machine,
   what SDL2, lwIP, ioquake3 and later Wine need from the libc. Decide, and
   state what the decision costs.
2. **Syscall table**: numbers from 16 upward (0–5 stay frozen), each with
   register signature, semantics, blocking behaviour and the complete error
   list. Groups: process (`spawn` with path, argv, envp and an explicit fd
   inheritance list as the baseline; `fork` marked excluded or supported
   with its cost; `exit`, `wait`/`waitpid` subset, `getpid`, `getppid`),
   threads (`thread_create`, `thread_exit`, TLS base via a GDT segment,
   the CiukiOS wait/wake extension that pthreads mutexes and condition
   variables are built on, named as an extension and not as futex), memory
   (`mmap` anonymous and file-backed or anonymous only — decide, `munmap`,
   `mprotect`, `brk`), files (`open` flag subset, `read`, `write`, `pread`,
   `pwrite`, 64-bit `lseek`, `close`, `fstat`/`stat`, directory reading,
   `mkdir`, `rmdir`, `rename`, `unlink`, `dup`, `dup2`, `fcntl` subset,
   `fsync`, `ftruncate`, `getcwd`, `chdir`), time (`clock_gettime`
   MONOTONIC and REALTIME, `nanosleep`), signals (`sigaction` subset,
   `sigprocmask`, `kill` of own process group only, `sigreturn`; delivery
   of `SIGSEGV`, `SIGFPE`, `SIGILL`, `SIGBUS` with a machine context on the
   user stack; default actions; what happens on a fault inside a handler),
   information (`uname`-like). State what is excluded in F2: users and
   permissions, terminals and job control, sockets (F6), dynamic linking,
   `fork` if excluded.
3. **Types and ABI**: ILP32, 64-bit `off_t` and `time_t`, `errno` values
   (recommend the Linux i386 numbering so ported code's tables match; say
   so explicitly), `struct stat`, `struct dirent`, `O_*`/`PROT_*`/`MAP_*`
   constants, the process entry stack (argc, argv, envp, a minimal auxv
   with page size and the TLS block), stack size and guard, alignment, the
   `ucontext`-like structure for signals.
4. **Process model**: static ELF32 loading rules (PT_LOAD only, page
   alignment, no PIE in F2, entry, bss zeroing, maximum image size), the
   address space from `execution-abi.md`, `spawn` semantics (path
   resolution through the VFS, 0–2 inherited plus the explicit list, close-
   on-exec equivalent), exit codes, zombies and reaping, teardown ordering
   (threads, mappings, handles), and the crash-isolation requirement (a
   fault in one process never affects another or the desktop).
5. **Path mapping**: the VFS is drive-letter based and case-insensitive;
   define the POSIX view (one root, e.g. `/c/...` per volume or `/` on the
   boot volume with `/mnt/<letter>` — decide and justify against what SDL,
   Quake and Wine expect), separators, case handling, the 259-character
   limit, and per-process cwd (POSIX) versus per-drive cwd (DOS bridge).
6. **Desktop and compositor as a ring-3 process**: the minimal kernel
   interfaces F2 needs — shared-memory surface objects, a present call that
   hands a rectangle of a surface to the framebuffer device of F1, an input
   event queue fed by the F1 input driver, and an inter-process channel for
   the desktop's clients — each as a syscall with errors, in the same table.
   The desktop itself is F2 code; only the interfaces belong here.
7. **SDK**: toolchain target and flags (the pinned clang/ld.lld), sysroot
   layout (`include/`, `lib/`, `crt0`, linker script), how the libc is built
   and pinned, a `ciuki-cc` wrapper or an explicit flag set, and the
   **gate application**: propose one upstream program with its own test
   suite that exercises files, memory, time and the console without
   networking (for example Lua 5.4 with its official test suite; justify
   your choice, its licence and how its tests become evidence), plus a
   second, graphical program using the surface interfaces as a stretch.
8. **`f2-acceptance.md`**: probes with exact pass conditions and evidence
   fields, in the style of `f1-acceptance.md`: `elf-load`, `spawn-wait`,
   `fd-table`, `mmap`, `signals-fault`, `threads-wait`, `crash-isolation`
   (desktop or a stand-in server survives a client fault), `libc-smoke`,
   `app-gate` (the upstream test suite run inside the guest, result as a
   record); selector `f2:<probe>` with the existing grammar; suites,
   deadlines, T0 host tests (ELF parser, path mapping, argument validation),
   T3 on the icount profiles, T4 on the laptops.

## Acceptance by the lead

Lead review, then a cross-review by `gpt-6.1-sol`. The contract is
approved when every F2 directive can be written from it without open
questions on numbering, types or process semantics. Reply with: files,
the libc decision in two sentences, the syscall count, the gate
application, and the open questions you could not close.
