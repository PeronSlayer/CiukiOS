# f1-61 DOS window disk selection and launch diagnostics

Research checked before implementation, 2026-10-14:

- [POSIX error definitions](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html)
  distinguish missing files, busy resources and physical I/O failures. An I/O
  failure on one optional pathname does not establish that another pathname
  is unreadable.
- [POSIX open](https://pubs.opengroup.org/onlinepubs/9799919799/functions/open.html)
  reports errors for the file being opened. Preserve the VFS error for the
  selected base, and keep the base read-only with deny-write sharing.

The supplied T23 evidence for ce2e6ce9 records DOOM/Quake I/O error windows,
an E: volume error of -5 and no dosbox launch records. It establishes missing
diagnostics and a concurrent foreign-volume failure, not their causal link.
In this worktree, `base_lookup` already ignores every failed fixture `stat`;
it also ignores bundled-image `stat` errors. `proc_launch_elf` logs only when
the console is verbose. Thus absence of that record cannot prove that the
dosbox process was never spawned or that a fixture error aborted selection.

Decision under the f1-61 directive: fixture lookup on D: through Z: keeps
scanning after every error or invalid entry. The window's bundled base is
required when no fixture qualifies: preserve its lookup error (including
ENOENT), reject invalid geometry/directories with EINVAL, and report the exact
base path. Do not silently replace a missing/broken bundled games image with
DOSBOOT.IMG. Direct kernel VMs with no configured base still use DOSBOOT.IMG;
workload probes still require an attached fixture. Global storage/table setup
errors remain fatal and are logged with reason=probe. Bundled lookup/open
errors use reason=bundled; selected fixture/default-base open errors use
reason=probe. Logging occurs outside the lookup gate and VFS critical sections.

The native DOS-window CREATE boundary emits an unconditional, bounded
`[launch-stage] name=dosbox pid=<pid> stage=vm-create` before disk selection
and VM creation, and `[launch] name=dosbox pid=<pid> error=<n> stage=vm-create`
on failure. It also names the requested window. This boundary covers desktop
game launches in quiet boot without enabling global latency accounting or
changing the syscall/wire ABI. Record the public returned error after existing
error translation; disk records retain the underlying VFS error.

Host evidence and outstanding QEMU/hardware checks are recorded after testing
in `build/codex/f1-61-report.md`. No QEMU or hardware acceptance is claimed.

Host validation: the full kernel suite passed. The DOS VM harness was rebuilt
and rerun on the final source: 5,821,240 checks passed, including foreign-drive
errors before the bundled base, a later valid fixture, required-base absence,
lookup/open failures, diagnostic ordering and cleanup/retry. The desktop suite
passed 23 tests with three canonical-build prerequisite skips, including the
Games menu, autorun/loading and launch timing cases. Both changed C units
compile under the pinned i686 flags and frame limits. All builds/tests used
`nice -n 19`, sequentially. ASan/UBSan were enabled; leak detection was disabled
because the sandbox blocks LeakSanitizer's process tracing. The optional DOS
guest-image runner was skipped for missing prerequisites. Lead acceptance
still requires `vm-window-qemu-desktop-2002`, `vm-starve-qemu-t23` and DOOM/Quake
on the T23 with the stick inserted, tied to the canonical image hash.
