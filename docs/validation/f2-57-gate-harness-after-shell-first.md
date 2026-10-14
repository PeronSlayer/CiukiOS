# F2-57 desktop gate descriptor model

2026-10-14. Host harness validation; no QEMU qualification.

## Research and repository comparison

- [glibc Linux open implementation](https://raw.githubusercontent.com/bminor/glibc/master/sysdeps/unix/sysv/linux/open.c)
  returns a Linux descriptor through the openat syscall.
- [glibc Linux close implementation](https://raw.githubusercontent.com/bminor/glibc/master/sysdeps/unix/sysv/linux/close.c)
  closes the descriptor in that same Linux table.
- [glibc closedir implementation](https://raw.githubusercontent.com/bminor/glibc/master/sysdeps/unix/sysv/linux/closedir.c)
  uses its internal close implementation for the DIR's descriptor.

The native gate fixture linked Linux `open()` with a public `close()` fake
that closes entries in `spawn_host_server->fds`. Since f2-56, the production
main loop enables the shell before the gate render loop; its optional device
queries use open/devctl/close. Successful Linux opens could therefore close
an unrelated guest object with the same numeric fd. This is a possible
host-dependent explanation of the directive's EBADF evidence, not a reproduced
cause: the unmodified gate suite passed all 10 tests on this worktree, whose
/dev/net, /dev/media and /dev/mixer paths were absent.

Separately, the fixture explicitly pinned display 3, input 4, authority 5
and output 6. Production `grants_install()` and `surface_create()` allocate
free slots, and the desktop accepts explicit display/input descriptor arguments.
These production allocation and ABI rules remain the authority.

## Harness decision

Look up each unique display/input grant by `desktop_description.kind` in the
modelled server table; pass the returned fds through the desktop's existing
arguments. Check the fd passed to the real `grant_input_read()` against the
looked-up input grant; do not remap a bad caller fd or weaken EAGAIN checks.
Track the output returned by `surface_create()` and find termination authority
by its object predicate.

Model optional mixer/network/media opens as non-inheritable fixture objects
in the same guest table as grants and channel pairs. Their devctl operations
retain ENOSYS; other direct file opens return ENOENT. Linux stdio/DIR handles
remain host fixtures. This does not emulate device service contents, icon
store I/O or a complete VFS.

A second test layout relocates the grants to display 11/input 13 through
production desktop_dup/desktop_close, leaving holes below them. The output
then uses slot 3. Both layouts exercise successful shell node open/close
cycles, missing-device EAGAIN progress for six seconds, USB key delivery
after five seconds, and exact approved-portrait first-frame checks in normal
and gate modes. Existing fault, CLOEXEC, explicit child-fd 3, group, IPC,
resource cleanup and burst checks remain in place.

## Verification

- Desktop gate: 10 tests PASS, ASan/UBSan, including both descriptor layouts.
- Desktop performance: 2 tests PASS, ASan/UBSan.
- Desktop collection: 92 discovered, 87 passed, 5 skipped because the native
  desktop payload is not built in this worktree.
- `git diff --check`: PASS.

All suites ran sequentially with `nice -n 19`. Generated review frames and
persistent host binaries were removed. See `build/codex/f2-57-report.md` for
commands, timings and limitations.
No production source, assets, image or build configuration changed.
