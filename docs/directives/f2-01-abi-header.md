# Directive f2-01: the single-source ABI header and its layout tests

- **Step:** F2. **Contracts:** `execution-abi.md` (F2 extension: types,
  errno values, constants, records, syscall numbers 16–68, auxv, TCB,
  signal frame, `ciuki_ucontext`, surface/display/input/channel records),
  `posix-subset.md` (sysconf values, pthread object sizes), `f2-acceptance.md`
  (T0 ABI layout tests).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f2-abi`. Files: new `src/kernel/include/ciuki/abi.h`
  (the only source of every public constant and record), new
  `tests/host/abi_layout_test.c`, new `scripts/test/abi_layout_dump.py`,
  `scripts/test/host_kernel_tests.sh` (add the test). Nothing else: no
  kernel behaviour changes, no SDK files yet.

## What to build

1. **`ciuki/abi.h`**: C17, freestanding, usable by the kernel and by the
   future SDK/newlib port without other project headers (`<stdint.h>` only;
   no kernel types). Contents, each exactly as the contract states and in
   this order: ABI version macro; fixed-width typedefs for the public types
   (`ciuki_off_t` i64, `ciuki_time_t` i64, … — prefixed names, the SDK maps
   POSIX names onto them); every errno value; `O_*`, `F_*`, `FD_CLOEXEC`,
   `SEEK_*`, `S_IF*`, `PROT_*`, `MAP_*`, `CLOCK_*`, `WNOHANG`,
   `CIUKI_SPAWN_*`, `CIUKI_THREAD_*`, `_SC_*` ids and their values, signal
   numbers and `SIG_*`/`SA_SIGINFO`, `DT_*`, auxv tags incl. the three
   `AT_CIUKI_*`; the syscall numbers 0–68 as an enum with the contract's
   names; the records `ciuki_timespec`, `ciuki_stat`, `ciuki_dirent`,
   `ciuki_spawn_fd`, `ciuki_spawn_args`, `ciuki_thread_args`,
   `ciuki_mmap_args`, `ciuki_sigaction`, `ciuki_siginfo`, `ciuki_utsname`,
   `ciuki_ucontext` (256 bytes, FNSAVE image), the 320-byte signal frame,
   the 64-byte TCB header, `ciuki_surface_info`, `ciuki_display_info`,
   `ciuki_rect`, `ciuki_input_event` (40 bytes), `ciuki_message` (288
   bytes), plus `_Static_assert`s for every size and offset the contract
   lists (`offsetof` on each field). Limits: `CIUKI_PATH_MAX 1040`,
   `CIUKI_NAME_MAX 765`, `CIUKI_ARG_MAX 65536`, `CIUKI_OPEN_MAX 128`, the
   process/thread/fd/mapping ceilings, the 1 MiB I/O cap, the 16 MiB
   surface cap, the 256 B message payload, 64 messages per direction, 256
   input events. Key code space: set-1 base codes, E0 → `| 0x100`, Pause
   `0x200`, with the public key code list the contract implies (letters,
   digits, modifiers, arrows, function keys, editing keys) as macros.
2. **Layout dump and test**: `abi_layout_dump.py` emits, from the header,
   a JSON of sizes/offsets by compiling a tiny target program with the
   kernel's clang flags (`--target=i686-unknown-elf`) and reading the values
   back from the object (for example through `_Static_assert` failures or
   a generated table in `.rodata` read with `llvm-objcopy`); the host test
   compiles the header natively with `-m32` where available and otherwise
   checks the target-generated JSON against the contract's tables encoded
   in the test. Every public struct size and every listed field offset is
   asserted; `sizeof(ciuki_off_t) == 8` and `sizeof(ciuki_time_t) == 8`.

## Acceptance by the lead

Diff review; host tests; kernel build unchanged (the header is not yet
included by kernel code). Reply with: the header's symbol count, the test
output, and any contract value that could not be expressed or that
contradicts another contract statement.
