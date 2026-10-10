# F2 acceptance: native processes, libc and applications

Author: Codex. Directive: f2-00. Date: 2026-10-10.
Status: revised after cross-review; lead approval pending.
This document specifies evidence, not a claim that any F2 test has passed.

## Decision and prerequisites

**[F2]** The gate MUST establish static ELF32 loading through the production
VFS, the complete [F2 ABI](execution-abi.md#f2-extension-native-abi-version-1),
the selected [libc and SDK](posix-subset.md), a ring-3 desktop surviving client
crashes, and the named upstream application running its tests inside the guest.
Every MUST below is mandatory except a row explicitly labelled stretch.
SHOULD is a recommendation and MAY an option. Table conditions and evidence
fields MUST be treated as normative requirements.

**[F2]** Lead approval of f2-00 followed by its cross-review MUST precede
implementation directives. T0/T1 MUST precede QEMU. F0 and F1 QEMU gates MUST
keep passing on each integrated image, including safe mode, storage durability,
fault containment and FPU isolation. F1 read/write qualification MUST precede
F2 filesystem mutation. A prerequisite failure MUST stop dependent tests and
record them as `not_run`, never PASS.

**[F2]** QEMU qualification MAY permit integration on main under the owner
decision recorded in [foundations-transition.md](foundations-transition.md).
Full phase completion MUST wait for one combined F0/F1/F2 qualification on the
same image on both ThinkPad T23 and Armada E500 after F2 closes on QEMU. Earlier
hardware evidence MUST NOT be relabelled for that image. F2 MUST NOT resume
publication or qualify the Windows bundle; those retain the existing release
policy and lead-owned requirements.

Primary references are the
[ELF loading specification](https://refspecs.linuxfoundation.org/elf/elf.pdf),
[i386 ABI](https://www.sco.com/developers/devspecs/abi386-4.pdf),
[POSIX.1-2024 interfaces](https://pubs.opengroup.org/onlinepubs/9799919799/idx/functions.html),
[newlib OS/reentrancy interfaces](https://sourceware.org/newlib/libc.html#Syscalls),
and [Lua's official test procedure](https://www.lua.org/tests/).
POSIX supplies the semantics being adapted; the exclusions, counts, workloads
and deadlines here are CiukiOS requirements. The repository's F0 dispatcher and
per-task address spaces are documented in posix-subset.md; they do not already
implement this gate.

## Selection and evidence

**[F2]** Loader and kernel MUST accept exactly this extension of the F0/F1
selector grammar, at most 64 ASCII bytes:

```text
f2:<probe-id> run=<8-hex-digit-id> [platform=e500] [safe=1] [server=desktop|standin]
```

**[F2, f2-12 amendment]** `server` MUST apply only to `crash-isolation`,
occur last, appear at most once and require validated QEMU fw_cfg provenance. Automatic selection MUST use the actual
desktop when a qualified LFB and `/bin/desktop` are available. Safe mode and
no-LFB operation MUST retain the stand-in, including with an explicit desktop
request. An explicit desktop request with an available LFB but missing payload
MUST fail launch rather than silently qualify the stand-in. The existing
64-byte bound remains: the 67-byte combination of `platform`, `safe` and
`server` is invalid; safe E500 selectors MUST omit `server` and use automatic
fallback selection. The combined E500 + safe + server request is never
needed because safe mode always runs the stand-in. This suffix also requires boot-loader validation; adding
it only to the kernel cannot make an explicit selector bootable.

The post-fault host observation uses the ordered key, relative-motion and
button unions documented by the [QEMU input-send-event reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
and binary PPM from [screendump](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-screendump).
The runner MUST retain before/after PPM SHA-256, compare the upper-right 64x96
unoccluded portrait region with `apps/desktop/convert_portrait.py` (the same
pinned conversion as the portrait host test), and require at least 16 changed
pixels in the original cursor's 8x16 footprint. A clock-only change MUST fail.
It MUST delete passing captures after retaining these measurements. This is
external observation, additional to guest presents/input counters and the LFB
digest; it does not establish guest qualification by itself.

The native controller MUST use production supervisor launch, grants and a new
process group. Test-only desktop pacing cycles `bad-pointer`, `closed-peer`,
`forged-fd`, `grant-fd`, `handler-fault` twenty times each. Expected signals are
SIGPIPE for closed-peer and SIGSEGV for the other four. Rejection assertions
precede the deliberate faults; an assertion exit cannot qualify a cycle. This
matches [POSIX signal meanings](https://pubs.opengroup.org/onlinepubs/7908799/xsh/signal.h.html)
and [wait status semantics](https://pubs.opengroup.org/onlinepubs/9699919799/functions/wait.html).

Call 3's existing bounded ASCII summaries MUST identify the emitting process
through the kernel task, never through a claimed PID. The desktop publishes
an aligned anonymous control page explicitly; the controller validates its
whole writable extent and writes only the declared command/generation words.
The demo counts only matching PONGs for its pending serial. After 100 turns it
pauses; a snapshot barrier follows the desktop's flushed replies and focus
messages, and the demo reports its consumed generation. Only then are both
object and process ledgers compared. Test loops sleep 1 ms (ordinary loops
remain 10 ms), without lowering the 100 turns/100 ticks requirements.
The desktop MUST publish each new victim PID before sending CONFIGURE, which
can schedule that child and deliver its armed summary. Interaction summaries
also MUST show both key edges, motion and both button edges consumed.

| Record | Stand-in | Native desktop |
| --- | --- | --- |
| `payload` | Embedded ELF SHA-256 | Production `/bin/desktop` launch |
| `identity`, `spaces` | PIDs and CR3 in identity | PIDs/pgids in identity, CR3 in spaces |
| `victim` x100 | Existing modes 2–5, signal/status | Five named faults, PID/CR3/pgid, signal/status |
| `progress` x100 | Result-page turns/ticks | PID-bound summary turns/ticks |
| `restored` x100 | Object ledger equality | Object and process ledger equality |
| `cycles`, `ledger` | 100, final equality | 100, final equality |
| `display` | Existing before/after fallback counters | Interaction counters below |
| `ARM action=post_fault_input` | Absent | Presents/input count/pixel digest |
| `interaction stage=before/after` | Absent | Presents/input count/pixel digest; after `changed=1` |

The runner MUST delay its pre-input capture until the test desktop has had
an opportunity to repaint after ARM, then deliver the ordered stimulus. Its
post-input capture MUST happen while the desktop is still live, following the
interaction record and completed stimulus; a terminal without that observation
MUST fail. The controller retains the desktop for 3000 ticks before teardown.
The runner independently MUST verify the before/after guest counters and digest
against ARM. Normal cases on T23, E500, min128, desktop-1998 and desktop-2002
require native evidence; no-LFB and safe cases require stand-in evidence.

Boot-time planning estimate (not QEMU evidence): 10,000 acknowledged turns
with two 1 ms polling loops cost approximately 20 s of guest pacing, plus 100
launch/fault/reap cycles, summary framing, full repaints and a 3 s observation
window. Budget 45–120 s of host time under TCG `-icount shift=1,sleep=on`; the
300 s boot deadline remains. The lead MUST measure this estimate on the
canonical image; no hardware or TCG throughput is inferred from host fakes.

**[F2]** The fixed probe registry MUST contain `elf-load`, `spawn-wait`,
`fd-table`, `mmap`, `signals-fault`, `threads-wait`, `crash-isolation`,
`libc-smoke`, `app-gate`. Unknown IDs, duplicate/out-of-order keys, extra keys,
oversized or malformed requests MUST report selection error and run no probe.
F2 MUST have no kernel `all`/`core` alias; a runner alias MUST expand into
individual boots in prerequisite order. Absent selection MUST boot the ordinary
desktop without fault injection. F0 and F1 selectors MUST remain unchanged.
`platform=e500` and selector `safe=1` MUST remain validated-QEMU-only, through
`opt/it.alcybercloud.ciukios/test`; physical selection MUST use the menu/bounded
serial path and menu/BOOT.CFG safe mode. This retains the bounded
[QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html) interface.

**[F2]** Evidence MUST retain `CIUKI_TEST v=1`, at most 240 bytes per ASCII
record, unique keys, monotonic sequence numbers per run, decimal counters and
fixed-width hexadecimal addresses. Each selected probe MUST emit exactly one
BEGIN and one terminal END with status PASS/FAIL; READY/ARM MUST precede external
stimuli. Missing, duplicate, contradictory or late terminal records MUST fail
even if the emulator or application exits zero. Long evidence MUST be split
into records, never truncated. Available serial and screen sinks MUST contain
the same records under the existing fallback/paging rules.

```text
CIUKI_TEST v=1 run=12ab34cd seq=000001 probe=spawn-wait event=BEGIN
CIUKI_TEST v=1 run=12ab34cd seq=000002 probe=spawn-wait event=DATA case=inherit pid=7 fd=8 offset=17
CIUKI_TEST v=1 run=12ab34cd seq=000003 probe=spawn-wait event=END status=PASS

CIUKI_TEST v=1 run=12ab34ce seq=000001 probe=app-gate event=BEGIN
CIUKI_TEST v=1 run=12ab34ce seq=000002 probe=app-gate event=DATA case=lua-basic exit=0 final_ok=1 assertion_failures=0
CIUKI_TEST v=1 run=12ab34ce seq=000003 probe=app-gate event=END status=PASS
```

**[F2]** Every case MUST identify `case`, process/thread IDs where relevant,
expected and observed result/error, and counters that establish its condition.
The kernel test controller MUST own record sequence numbers. Untrusted stdout
MUST be escaped or framed as application output and MUST NOT become a test
record just because it contains `CIUKI_TEST`. Call 3 MUST keep its frozen
format/limit and attach the emitting task identity through the controller.
Application success text alone MUST NOT override a nonzero/crashed process
status. Logs MUST contain enough measured data for host predicates to check
the result rather than accepting the kernel's PASS string alone.

**[F2]** Result.json MUST retain all F0/F1 image, manifest, source, dirty state,
tool, selector, device, firmware, profile, timing, stimulus and cleanup fields.
It MUST add `abi_version`, `sdk_manifest_sha256`, newlib source/patch hashes,
application and test-source hashes, ELF hashes, argv/env/cwd/fd setup, application
wait status, declared exclusions, maximum resident/committed pages and resource
ledgers. SHA-256 strings MUST be split across DATA records if necessary and
retained whole in result.json. Host-manifest hashes MUST be compared to actual
guest-loaded payload hashes. Image SHA-256 MUST remain externally calculated;
build IDs MUST NOT substitute for it.

## Probe matrix

**[F2]** All negative cases MUST arrange one principal invalid condition, so
the ABI's allowed same-stage error precedence does not create false failures.
`U` means static SDK-built ring-3 ELF on the canonical filesystem; `K` means a
supervisor controller observing production paths. K MUST NOT implement a second
loader, allocator, scheduler or syscall path. Expected user faults MUST be
isolated in fresh processes. Test-only injection MUST remain supervisor-only
and inactive without a validated selector.

| Probe / tiers / payload | Exact pass conditions | Required DATA evidence |
| --- | --- | --- |
| `elf-load` / T0,T1,T3,T4 / K,U | A valid static ELF MUST run at 0x00400000 with expected initialized data, zero BSS/padding, entry registers, argc/argv/envp/auxv, GS TCB and x87 reset state. Truncated headers/tables, integer overflow, wrong class/endian/machine/version, ET_DYN, INTERP/DYNAMIC/TLS, invalid alignment, page overlap, W+X, executable stack, entry outside initialized RX, oversized image and unmapped pointer vectors MUST produce the specified ENOEXEC/E2BIG/EFAULT without publishing a child or leaking allocations. The maximum accepted boundary and one-over-boundary MUST both be tested. | `case`, ELF SHA-256, file/LOAD byte totals, entry/ESP/GS, auxv page/TLS/ABI values, rejection errno, children/pages/maps before/after, BSS/register errors=0. |
| `spawn-wait` / T0,T3,T4 / K,U | Parent and child MUST have distinct writable backing at the same VA. Inheritance MUST cover argv/env snapshots, relative cwd, default 0–2, absent fd 3+, explicit mapping/swap/close, CLOEXEC rejection and failure rollback. Child normal exit 37 MUST yield status 9472; fault exit MUST yield the named signal plus raw vector diagnostics. WNOHANG MUST return 0 while live; wait MUST reap once; invalid status pointer MUST leave a zombie; competing waits MUST not double-reap. Parent death MUST adopt/reap through PID 1. 100 create/exit/fault/reap cycles MUST restore ledgers after each cycle. | PIDs/PPIDs/PGIDs, fd map and flags, independent sentinels/PFNs, wait statuses/errors, child publication count, orphan counts, cycles=100, pages/threads/zombies/handles/maps baseline and final, leaked=0. |
| `fd-table` / T0,T3,T4 / U,K | A fixed file workload MUST verify O_EXCL, append atomicity from two descriptions, shared dup/spawn offsets, dup2 replacement/self case, CLOEXEC, fcntl flags, pread/pwrite position preservation, 64-bit seek above 4 GiB without allocation, FAT EFBIG refusal, zero-filled truncate growth, fsync/reopen data, open-unlink lifetime, replacement/case-only/cross-directory rename, EXDEV, cwd reconstruction, pinned rmdir EBUSY and directory cookies/rewind. Rename MUST return EISDIR for file over directory, ENOTDIR for directory over file and EINVAL for a move into its own descendant; identical-entry rename MUST succeed without mutation even for a pinned nonempty directory, while case-only rename MUST update spelling. Fill 128 fd slots, observe EMFILE, close/reuse slots without leaks. Bad buffers/flags/encoding/lengths MUST match errors with zero validation side effects. Read-only/share-conflict and post-commit I/O failures MUST preserve F1 rules. | Operation IDs, expected/actual offsets/sizes, hashes, file identity/link counts, normalized paths, fd/status flags, directory digest, errno, side-effect counts, handle ledger; checker digest/results for durable workload. |
| `mmap` / T0,T3,T4 / U,K | Anonymous mappings MUST be zero, private across processes, correctly rounded and never replace a hinted collision. Partial unmap/split/protect MUST preserve unaffected bytes. All 16 transitions between NONE/R/RW/RX MUST succeed independently of initial protection, including initial NONE to RW and RW to RX and back; contents MUST survive temporary NONE/R/RX restrictions. PROT_NONE/read-only accesses MUST fault locally; W+X, MAP_FIXED, MAP_SHARED, file-backed and invalid/wrapped requests MUST fail as specified. Brk query/grow/shrink/regrow MUST preserve old bytes and zero newly exposed bytes. Exhaustion and split-allocation failure MUST leave mappings/credits unchanged. Active stack/TLS/pinned I/O changes MUST return EBUSY. Surface mapping references MUST survive fd close and die after final unmap. | Range/protection/physical-credit ledger, initial/current/maximum rights, transition IDs/results, sentinels/digests, errno, fault address, split count, promised/free/reserved pages before/after; `nx=0` explicitly. |
| `signals-fault` / T0,T3,T4 / U,K | Real #PF (unmapped and read-only), #UD, integer divide, unmasked x87 and #AC injections MUST deliver SIGSEGV/SIGILL/SIGFPE/SIGBUS with exact vector/error/address/EIP and caller-local context. A handler MUST repair its saved EIP or x87 state and resume the intended instruction path with GPRs, flags, masks, TLS and x87 intact. Mask/unmask MUST defer and coalesce SIGUSR1; own-group kill MUST succeed, cross-group kill MUST fail EPERM; thread_kill MUST reach the selected thread. Handler-blocking and syscall-interruption subcases below MUST pass. SIGKILL, default fault, blocked/ignored fault, bad stack, handler fault and forged sigreturn MUST terminate only the offender. | Signal/vector/code/EIP/CR2, sender/target PID/TID, handler entry/return counts, saved/restored mask and x87 digest, nested-fault result, forged field, wait status, survivor progress, corruption=0; blocking/interruption evidence below. |
| `threads-wait` / T0,T3,T4 / U,K | Eight threads MUST each keep distinct errno, TCB, pthread key and x87 state through at least 1000 preemptive switches. Normal/recursive mutex tests MUST reach exactly 80000 shared increments. An interrupted mutex wait MUST retry and acquire only after unlock, without exposing EINTR. Condition producer/consumer MUST transfer 10000 numbered tokens with zero loss/duplicates, including signal-before-enqueue and timeout/wake races. Wait mismatch MUST return EAGAIN; timed wait MUST reach ETIMEDOUT no earlier than its deadline; unmap/reuse MUST return ECANCELED for the old mapping's queued wait, and kernel operations already bound to its generation MUST NOT affect the new mapping's wait. Userspace wakers MUST be coordinated before reuse; a delayed userspace wake after reuse MUST NOT be treated as covered by kernel generation isolation. Once MUST execute once; key destruction MUST obey four-pass limit; join/detach and last-thread exit MUST reclaim stacks/TLS. | Per-thread IDs/errno/TCB/state digests, dispatches, increments=80000, tokens=10000, once_count=1, timeout-domain/deadline/observed time, mapping generations, wait/wake/cancel outcomes, userspace-waker coordination, mutex interruption/retry/acquisition results, destructor passes, resources before/after. |
| `crash-isolation` / T0,T3,T4 / K,U | Desktop (or labelled early stand-in), one survivor client and a disposable client MUST be separate address spaces. The victim MUST fault after sharing a surface and while a channel transaction is pending; repeat for bad pointers, closed peers, forged/grant fds and handler fault. The desktop and survivor MUST keep their PIDs, handle at least 100 request/reply turns and make progress for at least 100 timer ticks after every fault. An actual desktop MUST redraw and consume a post-fault key and mouse/button sequence without reboot/restart. 100 victim cycles MUST release messages, surfaces, mappings and fds; no client MAY map the LFB or acquire grants. | `server=desktop` or `server=standin`, PIDs/CR3 identities, fault/status, before/after present/input counts and pixel digest, replies>=100, ticks>=100, cycles=100, resource ledgers, unauthorized_access=0, desktop_restarts=0. |
| `libc-smoke` / T0,T1,T3,T4 / U | The installed SDK MUST compile/link a standalone program using stdio, malloc/realloc/alignment, strings/conversions, x87/libm, setjmp/longjmp, per-thread errno, file/directory wrappers and environment. Constructors/atexit/destructors MUST execute in specified order. Real console I/O/redirection MUST work. MONOTONIC/REALTIME/CPU clocks and sleeps MUST satisfy the clock cases below. Excluded entry points/features MUST fail or remain unadvertised exactly as documented. Every syscall 16–68 MUST have at least one successful and applicable negative case in this matrix. | SDK/newlib/ELF hashes, test counts/failures, bytes/hash for stdout/stderr, allocation high-water, exit callback order, clock sources/resolutions/deltas, supported/excluded API list, coverage bitmap, missing_cases=0. |
| `app-gate` / T1,T3,T4 / U,K | Lua 5.4.8 MUST run the unmodified official portable suite and the separately identified CiukiOS supplement below entirely in the guest. Both MUST exit zero, with no failed assertions, unexpected fault, timeout or extra skip. The desktop/survivor MUST remain alive after completion. A host Lua run or successful compilation MUST NOT satisfy this row. | Application/version, source/tests/SDK/ELF hashes, `case=lua-basic` and supplement case results, argv/cwd, final_ok=1, assertion_failures=0, wait status, declared exclusions, console digest, heap/stack/resident high-water, survivor progress. |

### Required subcases and measurement details

**[F2/T0]** Host tests MUST exercise the production ELF parser, path mapper
and argument validator with valid and malformed fixtures, including truncated
pages, multiply/add overflow, read-only destinations, page-straddling buffers,
oversized argv/envp, duplicate inheritance targets and concurrent unmap
interleavings. The path matrix MUST cover `/`, `/mnt`, `/mnt/d`, `/dev/null`,
relative cwd, `..` over a mount root, `missing/../x`, trailing slash, repeated
slashes, forbidden backslash/colon, invalid UTF-8/non-BMP input, case collisions,
259/260-character canonical paths and UTF-8 byte limits. Host ABI tests MUST
assert every public structure size/offset and constant against target-generated
layout data, including sizeof(off_t)=sizeof(time_t)=8. They MUST NOT substitute
the host's native stat, timespec or pointers.

**[F2/T0]** Scheduler/wait models MUST force the compare/enqueue/wake and
mapping-generation races through production queue logic. Signal-frame validator
tests MUST enumerate privileged selectors, altered IOPL/VM/NT flags, wrong
token/thread/frame address, kernel EIP/ESP, invalid x87 metadata and bad masks;
none MUST reach hardware restore. Object tests MUST cover every channel queue
limit, fd-attachment limit, partial allocation rollback, endpoint close and
surface final-reference ordering. Renderer tests MUST compare clipped rectangles
and pixel conversions against independent reference pixels with untouched
padding/canaries. Full filesystem fixtures/checkers MUST reuse F1's production
FAT/VFS/cache harness and error-injection model.

**[F2/T1]** Static checks MUST reject an SDK/native binary with host library
dependencies, missing pinned hashes/licenses, undefined symbols, wrong target,
unsupported instructions or rejected ELF segments. All payloads, upstream test
files and negative ELF fixtures MUST be present in the canonical image and
listed by path/hash in build-manifest.json. The canonical build MUST include
both ordinary desktop operation and runtime-selected tests, without test-time
build flags. Existing kernel helper/opcode audits MUST remain unchanged.

**[F2]** The clock cases MUST measure 10000 successive MONOTONIC reads with
zero decreases, REALTIME minus MONOTONIC equal to the recorded UTC seed minus
its MONOTONIC sample within one timer tick, and process CPU time increasing
while busy and not increasing during a 100 ms sleep except for at most two
accounting ticks.
A 20 ms nanosleep MUST last at least 20 ms guest time; interruption by SIGUSR1
at approximately 10 ms MUST return EINTR with a remainder bounded by 0–20 ms.
Scheduling delay MUST be recorded, not confused with clock resolution. Invalid
clock IDs/timespecs and 64-bit deadlines MUST use the ABI errors. Evidence MUST
record `realtime_source=1`/`source=rtc` for a valid seed from the qualified F1
read-only RTC provider, otherwise `realtime_source=0`/`source=build` with the
canonical build's recorded UTC epoch and seed MONOTONIC sample zero. It MUST
include provider qualification/result, seed UTC, seed MONOTONIC sample and
observed offset, and MUST agree with uname. T0 MUST exercise both source-selection
paths; guest runs MUST verify the available qualified source or fallback without
adding F2 port access. The build fallback MUST NOT claim calendar-clock accuracy.

**[F2]** The signals-fault `case=handler-blocking` MUST enter a SIGUSR1 handler
that performs a 20 ms nanosleep, then direct an unmasked, caught SIGUSR2 to that
same thread while it sleeps. The sleep MUST finish successfully no earlier than
its deadline, with no EINTR or nested catcher; SIGUSR2 MUST run only after the
outer handler's sigreturn. Evidence MUST include target TID, signal/mask state,
handler entry/return ordering, maximum active depth=1, sleep result and elapsed
time. Separate runs in this subcase MUST send unmasked default-action SIGTERM
and SIGKILL while the handler blocks; each MUST terminate only the offender
without waiting for sigreturn, with wait status and survivor progress recorded.

**[F2]** The signals-fault `case=syscall-interruption` MUST inject an eligible
caught signal before command issuance for read/pread and before D-class commit,
except close/dup2, observing EINTR with no command/mutation. It MUST also
interrupt read/pread after issuance before any byte: a successful drain MUST
yield EINTR with the buffer/offset unchanged and no further command; failed/timed-out drain MUST
yield EIO and F1 quarantine. Interruption after a positive partial transfer
MUST preserve that count. D-class operations interrupted after commit MUST
complete with their success/I/O result. Close and dup2 MUST defer catchers
through both precommit waits and completion, with no EINTR, one fd release or
atomic replacement, and their documented error handling. Each returning case
MUST verify that output writes and saved context EAX already contain the final
result at handler entry and that sigreturn resumes with that result. Evidence
MUST include syscall/stage, command-issued and drained counts, commit/progress
counts, buffer/offset digests, raw result/saved-EAX/resumed-EAX, handler-entry
order, backend ownership and pinned-resource ledgers; teardown MUST leak nothing.

**[F2]** The threads-wait `case=mutex-interrupted` MUST hold a mutex in one
thread, block another on it and deliver a caught signal to that waiter. The
handler MUST return without changing the mutex. The waiter's raw wait_word
MUST report EINTR internally; pthread_mutex_lock MUST recheck/retry, remain
blocked while ownership is unchanged, then return zero with ownership only
after unlock. Evidence MUST include TIDs, handler count, raw EINTR/retry counts,
ownership/unlock/acquisition order and public return=0, with exposed_eintr=0.
The same interrupted-wait schedule MUST exercise pthread_once waiters and
newlib internal locks, preserving once_count=1 and lock ownership with no
exposed EINTR or premature completion.

**[F2]** SIGBUS testing MUST execute a ring-3 unaligned access with CR0.AM and
user EFLAGS.AC set under the controlled test. The handler trampoline MUST clear
AC for its own accesses; restoration MUST use the saved user flags. This MUST
not alter normal process startup. TCG's known x87 precision-control deviations
MUST remain recorded and MUST NOT be used to relax hardware comparisons of
control words, register preservation or process isolation. The exception
expectations follow the
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

**[F2]** Resource-cycle baselines MUST be taken after deliberate shared-cache
warmup. Final comparisons MUST include pages, backing credits, kernel allocations,
processes/zombies, live/retained threads, mappings, fds, open descriptions,
surface references, queued messages and waiters. Persistent shared cache changes
MUST be explicitly accounted for; unexplained growth MUST fail. Physical RAM,
reserved/free totals and application commitments MUST reconcile with boot-memory's
ledger on every target. No allocation probe MUST write into reserved memory.

**[F2, f2-19 ledger rule]** For `app-gate`, processes, zombies, all thread
counts, fds, mappings, backing credits, open descriptions, surfaces, messages,
grants, channels, page tables and namespace pins/waiters MUST return to their
baseline. Live volume-backed named identities MAY survive close: report their
before/after counts and allocator-class bytes (including the heap header),
and subtract only this measured byte delta from kernel in-use growth. Report
storage blocks before/after, their configured baseline bound and workspace
pages; the current `cache_init()` preallocates the entire pool and runtime I/O
reuses its blocks, so its physical-page and heap-byte deltas MUST be zero.
`cache_accounted=1` and `restored=1` require zero unexplained physical-page and
kernel-byte remainders and no other namespace-node growth. This follows the
[VFS lifetime distinction between last close, cached identity deallocation
and unmount](https://docs.kernel.org/filesystems/vfs.html), checked against
`fs/cache.c`, `core/kheap.c` and `proc/posixpath.c`. In this source state,
`vfs_detach()` expires identities; quiescent `vfs_destroy()` calls
`files_detach()` to free them and the namespace, as the host teardown check
proves. Report `heap_pages_before/after`, their delta, heap bytes in use
and peak before/after from the read-only `kheap_snapshot()` ledger: class-pool
pages are counted only on successful `refill()` allocations and retained by
`kfree()` for reuse. Subtract only this measured pool-page delta from
`pages_delta`; it does not excuse kernel bytes in use beyond retained named
identities. This separates backing pages from live objects, consistent with
[slab allocation](https://kernel.org/doc/html/latest/mm/slab.html) and
[kernel memory accounting](https://www.kernel.org/doc/html/v6.8/filesystems/proc.html#meminfo),
checked against `core/kheap.c`. The supplied 32-page growth passes only when
the heap pool independently grows by 32 pages; any remainder MUST fail.
Process tables/backing MUST restore: `release_threads()` calls `ua_destroy()`
and `as_destroy()` frees the page directory and user tables. ELF snapshots
are temporary and closed after load; the storage pool remains preallocated.
Ledger metadata arrays `storage`, `identities` and `heap` are respectively
`[blocks, workspace_pages]`, `[live_named_nodes, class_bytes]` and
`[pool_pages, bytes_in_use, peak_bytes_in_use]`.

### Lua application evidence

**[F2]** `app-gate` MUST first run the SDK-built Lua executable with argv
`["lua","-e","_U=true","all.lua"]`, cwd `/system/tests/lua-5.4.8-tests`,
`LC_ALL=C`, `TZ=UTC0`, `HOME=/home`, `TMPDIR=/tmp`, and only fd 0–2 inherited.
Stdin MUST be a finite fixture or `/dev/null`; stdout/stderr MUST be bounded
capture streams/files. The complete test directory MUST be writable in the
overlay where upstream tests require it, with test-created files limited to
that directory and `/tmp`. The observer MUST associate output with that PID,
require the upstream final success indication, zero failed assertions and
normal exit status zero. It MUST keep the suite archive hash and the fixed
`_U` exclusions in result.json. No test file MAY be edited to pass.

**[F2]** The separately hashed `ciuki-f2.lua` supplement MUST run these named
cases with its own terminal result: `file-roundtrip` (65536 bytes whose byte i
is i modulo 251, seek/read/rewrite bytes 8192–12287 as 255 minus their original
values, close/reopen and compare every byte before rename/remove),
`allocation` (100 cycles of 4096 live strings of length 128 followed by full
collection, preserving live reference values), `time-utc` (UTC round trips for
2000-02-29 and 2040-01-01 through os.time/os.date plus nonnegative os.clock),
and `console` (known ordered stdout/stderr lines and exit 0). Project records
MUST distinguish these from upstream `lua-basic`. The supplement MUST NOT serve
as a replacement if the upstream suite fails.

**[F2]** Application output MUST remain separate from controller records and
within the runner's 4 MiB log limits. If upstream verbosity exceeds that limit,
the observer MAY retain a streaming digest and bounded head/tail while scanning
the complete stream for assertions/final status; it MUST record total bytes and
capture truncation. It MUST NOT discard controller evidence. The runner MUST
mark the complete/internal upstream modes `excluded_by_contract`, never passed.
The two mandatory runs MUST use the same Lua executable and SDK as ordinary
native execution, without special allocator, parser or scheduler variants.

### Desktop qualification and stretch

**[F2]** An early stand-in server MAY establish isolation during implementation,
but `server=standin` MUST NOT close the final F2 gate. On LFB-equipped evidence
profiles and both laptops the final run MUST use the actual ring-3 desktop,
with the approved Ciuki identity preserved. A no-LFB boot MUST retain console
operation, return ENODEV for display interfaces and run stand-in isolation as
an additional fallback case. Graphical/input interaction on normal profiles
MUST be demonstrated by both guest counters and external observation.

**[F2]** Input evidence MUST compare the same physical keys from native set-2
decoding and firmware set-1 delivery against the shared public set-1 table.
It MUST include A=0x1e, an E0 key and Pause=0x200, with make/break/repeat
semantics as applicable; native set-2 positions MUST NOT escape into public
events. The F1 input probe and both backends' text/digest helpers MUST use that
same table. T0 MUST replay both encodings; qualified guest backends MUST report
backend, raw encoding, expected/observed public codes and event digest, with
the earlier F1 input counts and physical-transition requirements preserved.

**[F2 stretch]** The doomgeneric surface port described in posix-subset.md MAY
add a named `case=doomgeneric` to crash-isolation. Its test MUST show 300 frames,
controlled movement/menu input, clean exit and reference release, with engine/
data/license hashes. Failure or absence MUST be recorded as stretch coverage
and MUST NOT replace or weaken any mandatory row. Commercial data MUST NOT be
included in the canonical image.

## Suites, deadlines and retained evidence

**[F2]** Versioned suite definitions under `tests/suites/` MUST use the existing
runner and `image=full`, with these groupings and per-boot host-monotonic limits:

| Suite | Probes in dependency order | Deadline from QEMU launch |
| --- | --- | --- |
| `f2-process` | elf-load, spawn-wait, fd-table | 180 s for elf-load/spawn-wait; 300 s for fd-table |
| `f2-runtime` | mmap, signals-fault, threads-wait, libc-smoke | 180 s for mmap/signals-fault/libc-smoke; 300 s for threads-wait |
| `f2-desktop` | crash-isolation, normal and no-LFB/safe fallback cases | 300 s per boot |
| `f2-app` | app-gate | 900 s per boot, including both Lua runs |

**[F2]** An `f2-all` runner alias MUST expand these suites in the listed order.
The deadlines MUST NOT extend automatically after partial progress. QMP quit,
owned-scope termination, child reaping and lock release MUST retain F0's bounded
cleanup procedure; cleanup failure MUST fail the run. The application deadline
is an initial allowance for interpreted tests under TCG, not measured performance.

**[F2/T3]** Each mandatory probe MUST pass on `qemu-t23`, `qemu-e500` and
`qemu-min128`, using their pinned machine/firmware, pentium3/TCG,
`-icount shift=1,sleep=on`, PIIX storage and F1 input policy. Guest instruction
time and host wall duration MUST be separate fields. KVM MAY supply developer
smoke but MUST NOT establish instruction/timing correctness. Input injection
MUST wait for READY and use F1's paced QMP key/motion/button actions; receipt
MUST be established by guest events, not QMP command success. These practices
follow [QEMU icount](https://www.qemu.org/docs/master/devel/tcg-icount.html) and
[QMP](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).

**[F2]** Each boot MUST use a qcow2 overlay under build/test-runs with the
canonical image hash verified before/after. Tests MUST never rebuild, copy a
whole image, use build-time variants or modify the backing image. Declared
reboot/durability subcases MAY reuse an overlay sequentially. Host read-only
fsck.fat/mtools checks MUST occur only after QEMU stops; passing overlays and
captures MUST be deleted after their textual/hash evidence is retained.
Failure retention, five-run history, 2 GiB artifact budget and 4 MiB log limits
MUST remain those of F0/F1.

**[F2]** The lead MUST run QEMU and heavy builds sequentially after checking
host memory, with the shared worktree lock and existing systemd caps (QEMU:
MemoryMax=1500M, MemorySwapMax=0; full build: 3G/1G). MemAvailable before QEMU
MUST be at least 2 GiB. Host runner tests MUST NOT overlap a suite. Codex's
implementation directives MUST request host-testable work; QEMU and hardware
execution remain the lead's responsibility. No unrestricted fallback or
unowned-process termination MUST be introduced.

## Combined hardware gate and exit record

**[F2/T4]** After QEMU closes, the lead MUST write the same canonical image to
explicitly authorized expendable disks under F0's stable identity, flush and
readback-hash procedure. The record MUST identify each laptop, BIOS, CPUID,
installed/usable/reserved RAM, disk, framebuffer and input policy. T23 and E500
MUST each repeat the earlier F0/F1 hardware matrices and all nine F2 probes,
using actual desktop interaction for crash-isolation. Unsafe device corruption
injection MUST remain T0/T3; ordinary user fault/signal probes MUST run on T4.
Missing hardware evidence MUST remain `not_run`.

**[F2/T4]** Firmware startup MAY add at most 120 seconds to each external
deadline. Serial or complete paged screen evidence MUST retain run/sequence
identity; disk logs alone MUST NOT substitute for missing fault evidence.
Known application-file hashes MUST survive durable shutdown/power-cycle and
read-only host collection. Unexpected resets, desktop restarts, survivor damage,
leaks or input loss MUST fail. E500 MUST retain its qualified firmware-first
ownership; F2 MUST NOT add a second controller reader to obtain events.

**[F2]** The lead's exit record MUST list the image/SDK/application/test hashes,
each profile and hardware outcome, exclusions and measured memory high-water,
then distinguish QEMU integration from completed hardware qualification. A
failure MUST receive the existing short validation record and Italian diary
entry under the lead's workflow. Pending measurements MUST NOT be converted
into architecture choices by the implementer. Open qualification items are
actual newlib/desktop/Lua footprint, guest suite success, F1 laptop behavior and
later Wine requirements; no ABI numbering or process-semantics question is
intentionally left for implementation to choose.
