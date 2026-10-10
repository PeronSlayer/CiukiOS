# Directive f2-06: the CiukiOS SDK — newlib port, crt0, libpthread, libciuki, ciuki-cc

- **Step:** F2. **Contracts:** `posix-subset.md` (libc decision, surface
  and exclusions, pthreads layouts and error rules, paths, SDK build and
  reproducibility, crt0 duties), `execution-abi.md` F2 extension (types,
  errno, constants, records, syscall numbers and register signatures,
  entry stack, TCB, signal frame), `f2-acceptance.md` (`libc-smoke`, T1
  static checks).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Prerequisite on `main`:** f2-01 (`ciuki/abi.h`). The kernel side
  (f2-02…f2-05) is developed in parallel: the SDK must build and pass its
  host-side checks without a running kernel.
- **Worktree:** `wt/f2-sdk`. Files: new `sdk/` tree (`sdk/README.md`,
  `sdk/build_sdk.py`, `sdk/newlib/` patches and configure recipe,
  `sdk/crt/crt0.S`, `sdk/libciuki/` (syscall stubs, `ciuki_spawn`, surface
  and channel wrappers, `sigreturn` trampoline), `sdk/libpthread/`,
  `sdk/sysroot-overlay/include/` (`sys/`, `pthread.h`, `ciuki/*.h` that
  include `ciuki/abi.h` by copy at build time), `sdk/ciuki.ld`,
  `sdk/bin/ciuki-cc`, `sdk/tests/` (host-side link/inspect tests and the
  `libc-smoke` program source), `scripts/build_sdk.sh`, `Makefile`
  (`sdk` target), `config/sdk-pins.json` (newlib archive SHA-256, upstream
  commit, tool versions), `docs/sdk.md` (how to build a program),
  `build/downloads/` for the pinned archive (gitignored). Nothing in
  `src/kernel/`.

## What to build

1. **Pinned newlib import**: `newlib-4.5.0.20241231.tar.gz` fetched into
   `build/downloads/` with the SHA-256 recorded in `config/sdk-pins.json`
   (refuse mismatch); the per-file licence inventory generated into the SDK
   manifest; patches as files under `sdk/newlib/patches/` with their own
   hashes. Build out of tree with the pinned clang/ld.lld through wrappers
   (`--target=i686-unknown-elf -march=pentiumpro`, the contract's user flags:
   no SSE/MMX, `-fno-stack-protector`, `-mstack-alignment=4`), full stdio,
   long long, multithread locks, 64-bit `off_t`/`time_t`, dynamic
   reentrancy via `__getreent()` reading `GS:8`; nano/compiler-TLS/
   small-reent off; no libgloss, libnosys or upstream syscall numbers.
2. **CiukiOS system port** (newlib `libc/sys/ciukios` or the equivalent
   configuration): the reentrant syscall adapters mapping newlib's hooks to
   the kernel ABI with the contract's errno translation and 64-bit types;
   stdio on fds 0–2; environ; `sbrk` serialized with the allocator lock;
   locks implemented on the pthread library's mutexes (recursive where
   newlib requires); `_exit` → call 0; excluded interfaces returning
   ENOSYS visibly (never silently succeeding).
3. **crt0** (`crt0.S`): consume the entry stack (argc/argv/envp/auxv), read
   the TCB from `AT_CIUKI_TLS`, initialize `_reent` for the main thread,
   environ, stdio, pthread main-thread state, run init-array, call
   `main(argc, argv, environ)`, `exit`; `_Exit`/`_exit` immediate.
4. **libpthread**: the layouts of `posix-subset.md` (mutex 8 u32, cond 4,
   once 1, attrs), create/exit/join/detach/self/equal, mutex normal and
   recursive with EDEADLK/EPERM/EBUSY rules, condition variables with the
   sequence protocol over `wait_word`/`wake_word` and EINTR retry, timed
   waits (REALTIME/MONOTONIC attribute), `pthread_once`, keys (64, four
   destructor passes), `pthread_sigmask`, `pthread_kill`; positive error
   returns; entry trampoline allocating `_reent` and calling `thread_exit`.
5. **libciuki**: raw syscall stubs for 16–68 generated from `abi.h`,
   `ciuki_spawn`, surface/channel/input/display wrappers, the `sigreturn`
   restorer trampoline, `uname`, `sysconf` values.
6. **Linker script and wrapper**: `ciuki.ld` (0x00400000, separate RX/R/RW
   page-aligned LOADs, non-executable GNU_STACK, init/fini arrays, no
   dynamic/TLS segments); `ciuki-cc` applying sysroot, flags, `-nostdlib`,
   `-static`, the object order and the archive group; a self-test that
   compiles, links and inspects a program (`llvm-readelf`: segments, flags,
   no PT_INTERP/DYNAMIC/TLS, no undefined symbols, no SSE/MMX opcodes via
   the existing audit classifier) without running host binaries as target
   probes.
7. **`libc-smoke` source** (`sdk/tests/libc_smoke.c`): the acceptance row's
   coverage (stdio, malloc/realloc/alignment, strings/conversions, x87 libm,
   setjmp/longjmp, per-thread errno, files/dirs, environment, constructors/
   atexit/destructors order, clocks, excluded entry points behaving as
   documented), emitting `CIUKI_TEST` records through call 3 with the
   kernel-owned sequence rule (the program reports counts; the kernel
   controller frames them — follow `f2-acceptance.md`).
8. **Manifest** (`build/tools/ciuki-sdk/manifest.json`): tool hashes, newlib
   archive/commit/patch hashes, configure arguments, flags, header/archive
   hashes, linked-file licence inventory.

## Host tests (mandatory)

`scripts/build_sdk.sh` builds everything from a clean `build/tools/ciuki-sdk/`
under the memory-capped scope the lead runs (document the command); the
wrapper self-test; `llvm-readelf`/`llvm-objdump` checks on `libc_smoke`
and on a hello-world; a host-side unit test of the libpthread layouts and
of the errno/types mapping compiled natively with `-m32` where available
(fallback: target compile + static asserts).

## Acceptance by the lead

Diff review; SDK build from a clean clone with the pinned tools (lead runs
it in a scope); T1 static checks pass; later, `libc-smoke` passes on QEMU
once f2-02…f2-05 are integrated. Reply with: files, the manifest, build
time and size figures (text/data/bss of `libc_smoke` and hello-world),
test output, and any contract problem found (including newlib build
adjustments for clang).
