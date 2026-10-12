# f5-01 native development: implementation basis

This work follows `docs/directives/f5-01-native-ide-and-compiler.md`. Guest and
hardware qualification belong to the lead; host execution does not establish
CiukiOS runtime qualification.

## Research and repository comparison (2026-10-12)

- [TinyCC 0.9.27 manual](https://bellard.org/tcc/tcc-doc.html): TCC has its own
  C99 compiler, assembler and linker. Its GNU linker-script support covers
  library input commands, not the SDK's `SECTIONS`/`PHDRS` script.
- [Upstream release](https://github.com/TinyCC/tinycc/tree/release_0_9_27),
  immutable commit `d348a9a51d32cece842b7885d27a411436d7887b`: `tccelf.c`
  lays out two LOADs with executable read-only data and an unaligned writable
  LOAD. `libtcc.c` loads Linux CRT files by default. Neither matches
  `sdk/ciuki.ld`, `sdk/crt/start.c` or `sdk/tests/check_sdk.py`.
- [newlib libc manual](https://sourceware.org/newlib/libc.html): OS hooks
  provide file/process services and reentrancy. This port uses the existing
  SDK hooks and TLS CRT; it does not substitute Linux system calls.
- [POSIX pipe](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pipe.html)
  and [waitpid](https://pubs.opengroup.org/onlinepubs/9699919799/functions/wait.html):
  drain compiler output while the child runs, close the parent's write ends,
  distinguish EAGAIN, EOF, EINTR and child exit. The existing SDK supplies
  fcntl, waitpid and ciuki_spawn. Contrary to the directive's assumption,
  `sdk/libciuki/excluded.c` implements pipe as ENOSYS and the public ABI has
  no pipe syscall. `channel.c` supplies bounded message queues, blocking
  sends, DONTWAIT receives and EOF after peer close and queue exhaustion.

## Implementation decisions

Keep upstream source immutable and apply an explicitly recorded Ciuki patch
in the build directory. Both the Linux-hosted cross compiler and the target
compiler use that same patch. Emit static i386 ELF with page-aligned RX, R and
RW LOADs and a non-executable GNU_STACK. Retain the SDK crt0, newlib,
libciuki, libpthread and libdeskui; use upstream libtcc1 for TCC arithmetic
helpers. Refuse dynamic output, in-process `-run` and compiler TLS. Install
the host linker script for documentation/host SDK use; TCC implements the
equivalent layout directly.

CiukIDE uses CiukNote's textarea and file boundary. Project configuration is
bounded data, never a shell command. Build invokes /Programs/tcc with argv and
explicit stdout/stderr channel mappings. A TCC-only `_write_r` link wrapper
frames stdio bytes in the existing channel message payloads, and falls back
to the ordinary newlib write hook for console/files. CiukIDE drains those
messages as two output streams. This is an application port adaptation, not
a new POSIX pipe implementation. Run requests the existing desktop
LAUNCH path, with a desktop-owned CiukIDE identity authorizing project
executables under /Documents. No kernel ABI or privilege change is needed.

Vendored source is a release-tag snapshot of upstream compiler, headers and
runtime files, with per-file upstream Git blob identities and SHA-256 pins.
The source snapshot is intentionally smaller than the full release archive
(platform examples/tests and Windows headers are omitted). The original
LGPL notice and runtime licence exception remain present.

Host commands and their results, limitations and lead follow-up are recorded
in `build/codex/f5-01-report.md`.
