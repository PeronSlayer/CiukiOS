# f3-57 — Unified DOS local IFS channel and unchanged conformance closure

- **Phase/model:** F3/DF1–DF6, Codex gpt-6.1-sol, xhigh, primary-source research.
- **Status/worktree:** complete f3-54 directive; lead creates wt/f3-57 after
  f3-55/f3-56 interfaces are integrated and reviewed.
- **Authority:** FreeDOS fork's callout/bookkeeping hooks, Ciuki control driver,
  existing native dos_file/VFS adapters, activation/teardown, host harnesses,
  conformance runtime adapter and durable validation. No new disk/cache policy.
<<<<<<< HEAD
- **Dependencies:** lead confirms the fork-local 11xx seam versus public
  redirector conflict documented by f3-54. The base battery must remain unchanged.
  Use the f3-56 reserved capability/gate; lead reserves any additional selector
  or event before implementation. No ABI number is chosen here.
=======
- **Dependencies:** the lead confirmed fork-local IFS interception at the seven
  marshalled sites specified in f3-56; public 1100h returns AL=0 and the base
  battery/assertion stays unchanged. Use the lead-reserved DFV2 layout/gates and
  `kernel=unified` (per-game choice 2), preserving v1 and old/reference profiles.
  Lifecycle uses existing 0–8 as below; request no speculative extra selectors.
>>>>>>> main
- **Size:** full native/guest state and I/O integration, not only INT2F dispatch.

## Sources

Read f3-54 exact callout/gap tables, f3-49 with all amendments, f3-50 wire note,
f3-51 amendments, current provider/cache/deadline/sharing/lock APIs and K2043
<<<<<<< HEAD
dosfns/task/fcb/inthndlr/int2f in bounded ranges. Verify
=======
dosfns/task/fcb/inthndlr/int2f plus main/config/fatfs/ioctl/blockio in bounded
ranges. Verify
>>>>>>> main
[1108](https://fd.lod.bz/rbil/interrup/network/2f1108.html),
[1116](https://fd.lod.bz/rbil/interrup/network/2f1116.html),
[1121](https://fd.lod.bz/rbil/interrup/network/2f1121.html),
[112E](https://fd.lod.bz/rbil/interrup/network/2f112e.html) and Resource Kit
chapter28 IFS Manager. Cite actual source version and decisions before code.

## Required implementation

<<<<<<< HEAD
1. Implement the local backend at FreeDOS's remote-wrapper/call_int2f seam,
   preserving its 11xx marshalling for owned C:/native views and original INT2F
   chain for unowned requests. Do not globally claim public1100 installed: the
=======
1. Implement the local backend immediately before the original INT2F at all
   seven sites (`int2f.asm:355,364,407,432,443,454,471`), with the frame variants
   and conversion paths enumerated in the contract/H56-03. There is no single
   marshalled INT2F instruction at call_int2f. Preserve owned C:/native-view
   marshalling and each original INT2F site for unowned requests, executed once.
   Do not globally claim public1100 installed: the
>>>>>>> main
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
<<<<<<< HEAD
   timestamp get/set, flush, drive/DTA/CDS/cwd, JFT expansion, persistent extended
   errors, file/drive IOCTL and every lifecycle transition. All reference counts
   reserve/commit/rollback against the native registry once. FreeDOS's open
   fallback count increment and remote-close decrement cannot double-count.
4. Implement the activation barrier before shell/AUTOEXEC/application load:
=======
   timestamp get/set, **checked FCB-close metadata**, flush, drive/DTA/CDS/cwd,
   JFT expansion, persistent extended errors, file/drive IOCTL and every lifecycle
   transition. Implement the contract's explicit INT21/10h metadata hook before
   fcbfns.c:609–613: caller FCB size/date/time is validated input to the native
   operation, not ordinary 3Eh close or SFT mirror tampering. Preserve pristine-
   SHARE merge order, attribute-read-only metadata behavior without data-write
   authority, and mounted read-only protection. Precheck publication; serialized
   metadata plus close commits once, precommit failure keeps metadata/open state/
   refs intact. Cover failed publication, retry/reopen and provider faults.
   All reference counts reserve/commit/rollback against the native registry
   once. FreeDOS's open
   fallback count increment and remote-close decrement cannot double-count.
4. Join f3-56's late BIND **after init_kernel() returns and before DoInstall()**
   (`main.c:121–128`), so even CONFIG.SYS INSTALL programs see the native owner.
   INIT only discovers/registers the resident driver. PostConfig extends SFT
   and replaces CDS (`config.c:459–465`), final FsConfig runs (`main.c:348`),
   DOS=LOW can relocate code (`config.c:506`); resolve all final ranges anew.
   At that boundary implement the activation barrier:
>>>>>>> main
   drain/close bootstrap regular files, flush and disable local C: filesystem
   ownership, register real DOS structures, mount one logical private FAT16 view
   through f3-47 and publish all views/binding atomically. Fault each step and
   unwind to a consistent isolated state. No post-activation fallback to local FAT
<<<<<<< HEAD
   on an owned drive. Internal loaders/FCBs/1226 use the same native descriptions.
=======
   on an owned drive. `kernel=unified` fails locally before INSTALL/shell on an
   activation error; isolated fixtures may stay local explicitly. Internal loaders/
   FCBs/1226 use the same native descriptions. Event 0 BIND owns this transaction;
   no earlier kernel pointer from driver INIT may be used after relocation.
>>>>>>> main
5. Preserve C: private overlay, D: read-only system, E: policy-controlled removable
   media, persistent aliases and native node/share identity. BIOS raw/file access
   serializes through the same provider/generation; invalidate mapping on raw
   mutation. No new data cache/parser/window or raw native volume write.
<<<<<<< HEAD
=======
   Implement every cell of the contract's redirected-drive classification matrix
   for C:/D:/E:, not a blanket DPB fallback: 32h AL=FF/CF preserved; 4400h/09h/
   0Ah checked native information; 4408h/0Eh and 7302h/04h CF=1 AX=000F;
   7305h valid packet CF=1 AX=020C; INT25h/26h CF=1 AX=0201 with their extra
   stack FLAGS word. 4409h returns AX=0300/DX=1000 on the managed unsubstituted
   drive. Treat 7303h as a separate checked extended-space native adapter
   (`dosfns.c:885–887` -> 110Ch), not the legacy 1Bh/1Ch/36h layout/narrowing.
   Preserve exact invalid-drive/packet/buffer results and validation precedence
   in the matrix. These DOS absolute-disk failures do not prohibit BIOS INT13
   access to the virtual provider; that path keeps generation/serialization/
   cache invalidation and read-only policy, never physical-host-disk access.
>>>>>>> main
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

<<<<<<< HEAD
=======
Use process event selectors only with their existing meanings: 0 late BIND,
1 PSP_CREATE, 2 PSP_SELECT, 3 EXEC_BEGIN, 4 EXEC_COMMIT, 5 EXEC_ROLLBACK,
6 PROCESS_EXIT, 7 TSR, 8 VM_DESTROY. Dup/clone/forced-dup and FCB-close metadata
use original DOS operation identities on the checked file path and internal
native reserve/commit/rollback; they are not new lifecycle events or uses of
EXEC transaction numbers. No additional selector is requested. If a specific
transition cannot be represented, report it to the lead before adding an ABI.

>>>>>>> main
## Acceptance and tests

| ID | Required closure |
| --- | --- |
<<<<<<< HEAD
| H57-01 activation | Fault every drain/register/mount/publication step, stale generation and stop; no dual active FAT writer and no leaked descriptions/mount/cache allocation. |
| H57-02 marshalling | Enumerate every inventory callout and omission through production CPU+VFS; stack/FLAGS/register guards, negative errors/CX bytes, direct/FCB/EXEC/0300 exactly once. |
| H57-03 ownership | Genuine SFT chain/JFT/SDA/CDS direct reads, native/guest reference parity under dup/inherit/forced dup/exit/TSR/rollback and all admission faults. |
| H57-04 semantics | All H49-02 through09 and H50-02 through05 matrices, including mutation fault points, 15x15 sharing, lock lifetime, copied DTAs, NLS/FCB edges and raw/file coherence. |
| H57-05 bounded work | Provider/namespace/cache/transport held independently; fake-time deadline/cancel/INT24/late completion, FIFO successor progress, all ledgers baseline. |
| H57-06 conformance | Host probes deterministically assembled; lead gets two complete same-image pristine/unified comparisons at 100% for all original cases and all required appended cases, no lost records or cleanup failures. |
| H57-07 performance | Host counters prove zero post-map guest INT13 and bounded loops; lead measures T23 DF6 latency/distributions and contention before runtime closure, never host timing substituted for hardware. |
=======
| H57-01 activation | Execute INIT, final FILES/SFT allocation, LASTDRIVE/CDS replacement, FsConfig and DOS=LOW relocation; poison provisional pointers/callbacks. Activate only after init_kernel/before DoInstall; INSTALL's first file operation sees native ownership. Fault every late drain/register/mount/publication step, stale generation and stop; no dual FAT writer, stale binding or leaked description/mount/cache allocation. |
| H57-02 marshalling | Enumerate every inventory callout and omission through production CPU+VFS; stack/FLAGS/register guards, negative errors/CX bytes, direct/FCB/EXEC/0300 exactly once. |
| H57-03 ownership | Genuine SFT chain/JFT/SDA/CDS direct reads, native/guest reference parity under dup/inherit/forced dup/exit/TSR/rollback and all admission faults. |
| H57-04 semantics | All H49-02 through09 and H50-02 through05, mutation faults, 15x15 sharing, lock lifetime, copied DTAs, NLS/FCB edges and raw/file coherence. Checked FCB-close metadata: changed size/date, concurrent handle/FCB change and both close orders, attribute-read-only/mounted read-only, failed publication, provider fault/cancel/stop, retry and reopened bytes/size/date/AL/59h/refs. Match the frozen pristine-SHARE oracle; forged SFT changes alone never mutate native state. |
| H57-05 bounded work | Provider/namespace/cache/transport held independently; fake-time deadline/cancel/INT24/late completion, FIFO successor progress, all ledgers baseline. |
| H57-06 conformance | Host probes deterministically assembled; lead gets two complete same-image pristine/unified comparisons at 100% for all original cases and all required appended cases, no lost records or cleanup failures. |
| H57-07 performance | Host counters prove zero post-map guest INT13 and bounded loops; lead measures T23 DF6 latency/distributions and contention before runtime closure, never host timing substituted for hardware. |
| H57-08 drive classification | Production fork CPU+VFS fixtures execute every contract matrix cell: C:, D:, E: present/absent/read-only/writable, default-drive encodings, invalid handles/drives, devices, both initial CF states, 7302–05 malformed/short/wrapped buffers and INT25/26 FLAGS-stack sentinels. Exact native success fields/deliberate errors, no local DPB/cache/format/write side effects; separate coherent BIOS raw mutation tests. |
>>>>>>> main

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
