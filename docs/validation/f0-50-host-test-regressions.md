# f0-50 host harness regressions

Scope: the 2026-10-21 directive, host execution only. The original discovery
log describes `main` aed8765d; the implementation starts at 6b701d75.

Research and implementation decisions:

- Amendment 1 adds licence-inventory and FAT-fixture sizing regressions.
  [dosfstools mkfs.fat manual](https://github.com/dosfstools/dosfstools/blob/master/manpages/mkfs.fat.8.in)
  documents separate reserved sectors, allocation tables, root-directory space
  and cluster allocation. Size the fixture from its payload bytes plus 25%
  (at least 16 MiB), and 64 KiB per file/directory for allocation slack, rounded
  up to MiB with the existing 64 MiB minimum. Preserve the formatter's automatic
  FAT selection and complete payload readback. Extract the build's desktop
  licence mapping for reuse by the build and expected-inventory checks.
- [GCC include search rules](https://gcc.gnu.org/onlinedocs/cpp/Search-Path.html):
  quoted includes start at the including file's directory. The firmware harness
  relocates `i8042_test.c` into a temporary directory; relocate its test-header
  references just as it already relocates production-header references.
- [glibc scheduling definitions](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/bits/sched.h):
  GNU `SCHED_NORMAL` denotes Linux policy 0. Ciuki's normal priority level is 7;
  give the kernel constant its own namespace rather than suppressing diagnostics
  or letting either header's include order select its meaning.
- [glibc socket enumerators](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/bits/socket_type.h):
  the host headers declare socket names as enumerators. The replay adapter must
  remove guest macros before parsing host socket declarations, as it already
  does for host signal, directory and sysconf enumerators.
- [Clang UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html):
  array-bound instrumentation detects the old applet filename-table overrun.
  This checkout already has `programs` and a table-size static assertion; retain
  both and run the complete device module with its existing sanitizers.
- [Clang LeakSanitizer](https://clang.llvm.org/docs/LeakSanitizer.html):
  preserve each repository script's existing leak-detection settings. A sandbox
  LeakSanitizer failure must remain visible and needs an outside-sandbox rerun.

The storage fixture must substitute the current cache-writer wait boundary and
check writeback on every active loop iteration. The compositor fixture must
link the real toolkit units now called by folder transfer dialogs. The stack
allocator fixture must extract exactly its three production entry points;
the intervening global page sampler is outside that fixture's fake-PTE model.
The FAT16 source guard must require the explicit conformance directory argument
and its construction, retaining its existing malformed-parent runtime check.
The read-only log-reader fixture retains its fail-fast program-management
boundary: settings-format and writer callbacks captured by ASan's global
instrumentation get unreachable assertion stubs alongside the existing stubs.
No settings write is authorized by that fixture.

Build-provenance failures remain lead-owned. Detailed commands, results,
prerequisite provenance and remaining work are recorded in
`build/codex/f0-50-report.md`.

Final host evidence (2026-10-21): the ten other directive modules pass all
37 tests. The desktop module passes 25 of 28, with three lead-owned SDK/build
precondition errors; its production integrity predicate separately accepts the
copied output snapshot and rejects a tampered portrait. The 14 scheduler/latency
tests pass, and `core/task.c` and `proc/priority.c` compile with canonical i686
flags. No new `panic()` call was introduced.

Amendment 1 is implemented: the build and expected payload set share
`desktop_license_payloads()`, all licence hashes retain source verification,
and expected record paths/cardinality grow with that inventory. The FAT
readback fixture uses the documented payload-derived sizing. A separate real
mtools regression passes full size/SHA-256 readback of all twelve licence
files and one payload larger than the old 64 MiB volume; it also checks the
minimum and MiB alignment. The full canonical desktop readback still requires
source-matched SDK/desktop prerequisites.

The copied SDK's networking headers permit both native-newlib modules to pass
without SDK source changes. This is not evidence of complete SDK provenance:
the lead's artifacts describe newer GL/Win32 sources and a different exported
desktop protocol header than this worktree. The lead must rebuild the canonical
prerequisites after integration. In the original discovery log, the stale-output
test fails at its initial `current_build(inputs())` assertion, before tampering;
it is a stale prerequisite, not evidence of accepted stale output.

`nice -n 19 bash scripts/test/host_kernel_tests.sh` runs through its earlier
host checks and both SuperSavage fixtures, then stops at `record_runtime_test`
with LeakSanitizer's sandbox/ptrace fatal error. Leak detection was left enabled
there, and the remaining script requires the lead's outside-sandbox run.
