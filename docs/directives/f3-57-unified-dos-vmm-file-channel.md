# f3-57 — Unified DOS local IFS channel and unchanged conformance closure

- **Phase/model:** F3/DF1–DF6, Codex gpt-6.1-sol, xhigh, primary-source research.
- **Status/worktree:** complete f3-54 directive; lead creates wt/f3-57 after
  f3-55/f3-56 interfaces are integrated and reviewed.
- **Authority:** FreeDOS fork's callout/bookkeeping hooks, Ciuki control driver,
  existing native dos_file/VFS adapters, activation/teardown, host harnesses,
  conformance runtime adapter and durable validation. No new disk/cache policy.
- **Dependencies:** lead confirms the fork-local 11xx seam versus public
  redirector conflict documented by f3-54. The base battery must remain unchanged.
  Use the f3-56 reserved capability/gate; lead reserves any additional selector
  or event before implementation. No ABI number is chosen here.
- **Size:** full native/guest state and I/O integration, not only INT2F dispatch.

## Sources

Read f3-54 exact callout/gap tables, f3-49 with all amendments, f3-50 wire note,
f3-51 amendments, current provider/cache/deadline/sharing/lock APIs and K2043
dosfns/task/fcb/inthndlr/int2f in bounded ranges. Verify
[1108](https://fd.lod.bz/rbil/interrup/network/2f1108.html),
[1116](https://fd.lod.bz/rbil/interrup/network/2f1116.html),
[1121](https://fd.lod.bz/rbil/interrup/network/2f1121.html),
[112E](https://fd.lod.bz/rbil/interrup/network/2f112e.html) and Resource Kit
chapter28 IFS Manager. Cite actual source version and decisions before code.

## Required implementation

1. Implement the local backend at FreeDOS's remote-wrapper/call_int2f seam,
   preserving its 11xx marshalling for owned C:/native views and original INT2F
   chain for unowned requests. Do not globally claim public1100 installed: the
   unchanged `multiplex` case requires absence. DFVM advertises the local backend.
   Kernel-origin authentication prevents applications from forging privileged
   callouts. Unsupported owned functions return real DOS errors.
2. Cover every actual row of the source-backed inventory: directory mutation,
   open/create/new/extended/temp, close/commit, read/write/count0, attrs/rename/
   delete, search, allocation, FCB and internal EXEC reads, locks and exit. Respect
   CX/AX return conversion and per-call buffer/stack/SFT/SDA rules. Lock and
   unlock both use 110A's actual parameter convention; don't infer sites from
   REM constants alone.
3. Implement the gap hooks: all SftSeek modes, CloneHandle/dup/forced dup,
   timestamp get/set, flush, drive/DTA/CDS/cwd, JFT expansion, persistent extended
   errors, file/drive IOCTL and every lifecycle transition. All reference counts
   reserve/commit/rollback against the native registry once. FreeDOS's open
   fallback count increment and remote-close decrement cannot double-count.
4. Implement the activation barrier before shell/AUTOEXEC/application load:
   drain/close bootstrap regular files, flush and disable local C: filesystem
   ownership, register real DOS structures, mount one logical private FAT16 view
   through f3-47 and publish all views/binding atomically. Fault each step and
   unwind to a consistent isolated state. No post-activation fallback to local FAT
   on an owned drive. Internal loaders/FCBs/1226 use the same native descriptions.
5. Preserve C: private overlay, D: read-only system, E: policy-controlled removable
   media, persistent aliases and native node/share identity. BIOS raw/file access
   serializes through the same provider/generation; invalidate mapping on raw
   mutation. No new data cache/parser/window or raw native volume write.
6. Retain full DF4 sharing/locks, native/DOS contention, copied search state,
   attributes/dates and all DF5 errors/critical callbacks. Reuse cancellable
   request transport and 2000ms total deadline, including retry/INT24; release
   storage locks before callbacks. Stop/removal invalidates generations and
   suppresses late guest writes. A synchronous wait followed by a clock check
   does not satisfy cancellation.
7. Preserve entry/actual-return trace pairing for original calls and internal
   operations without double records; generation/sequence/drop accounting and
   monitor/wall time remain meaningful. Warm mapped file reads have zero guest
   INT13; bootstrap/cold/raw traffic is separately counted.
8. Run all existing f3-48 programs, unchanged in tags/assertions/binary hashes,
   against pristine reference (bridge off, SHARE on) and unified candidate
   (local IFS on). Add new programs for new behavior without editing old cases.
   No identical unsupported errors, normalization, skip or synthetic PASS.

## Acceptance and tests

| ID | Required closure |
| --- | --- |
| H57-01 activation | Fault every drain/register/mount/publication step, stale generation and stop; no dual active FAT writer and no leaked descriptions/mount/cache allocation. |
| H57-02 marshalling | Enumerate every inventory callout and omission through production CPU+VFS; stack/FLAGS/register guards, negative errors/CX bytes, direct/FCB/EXEC/0300 exactly once. |
| H57-03 ownership | Genuine SFT chain/JFT/SDA/CDS direct reads, native/guest reference parity under dup/inherit/forced dup/exit/TSR/rollback and all admission faults. |
| H57-04 semantics | All H49-02 through09 and H50-02 through05 matrices, including mutation fault points, 15x15 sharing, lock lifetime, copied DTAs, NLS/FCB edges and raw/file coherence. |
| H57-05 bounded work | Provider/namespace/cache/transport held independently; fake-time deadline/cancel/INT24/late completion, FIFO successor progress, all ledgers baseline. |
| H57-06 conformance | Host probes deterministically assembled; lead gets two complete same-image pristine/unified comparisons at 100% for all original cases and all required appended cases, no lost records or cleanup failures. |
| H57-07 performance | Host counters prove zero post-map guest INT13 and bounded loops; lead measures T23 DF6 latency/distributions and contention before runtime closure, never host timing substituted for hardware. |

Add a focused `tests/host/dos/run_unified_file.sh` exercising the real fork's
request path, native bridge/VFS/provider and fault fixtures. Integrate it into
the normal kernel host script. Run one at a time with nice19: that script,
run_unified_binding.sh, DOS unittest discovery, run_file_api.sh, run_hook_cpu.sh,
host_fs_tests.sh and host_kernel_tests.sh. Preserve script-defined sanitizer
settings; add no external detect_leaks override. Regenerate docs/build apps
after a public ABI edit. New host scripts get exact commands in the DOS README.

## Evidence and constraints

Report build/codex/f3-57-report.md with every H57/H49/H50 outcome, source/image
identity separation, callout coverage, activation proof, trace examples,
counter/ledger results and open lead-owned runtime gates. Lead builds the one
canonical image and runs the authorized differential battery. No default switch
or retirement here. Codex runs no QEMU, scopes, OS image build, git mutation or
diary edit. No new panic, game patch, memory-cap increase, global redirector
advertisement or weakened original battery. Escalate a real contract conflict
with evidence; never conceal it in profile-specific expected results.
