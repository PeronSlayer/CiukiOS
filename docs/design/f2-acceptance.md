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
desktop when a qualified LFB and `/Programs/desktop` are available. Safe mode and
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
| `payload` | Embedded ELF SHA-256 | Production `/Programs/desktop` launch |
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
desktop without fault injection. Existing F0/F1 selectors retain their grammar.
F1-28 adds `f2:sweep` through BOOT.CFG only, beside the per-case suites. It
traverses the production registry on hardware, retaining per-probe BEGIN/END
and the same run id across reboots. `all:sweep` chains F0, F1 and F2. QEMU
suites still use one boot per case; the sweep is hardware traversal evidence,
and missing stimuli, exported-disk checkers and operator observations remain
not_run. This alias does not add an F2 `all` or `core` alias.
`platform=e500` and selector `safe=1` MUST remain validated-QEMU-only, through
`opt/it.alcybercloud.ciukios/test`; physical selection MUST use the menu/bounded
serial path or a BOOT.CFG probe line, and menu/BOOT.CFG safe mode. This retains the bounded
[QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html) interface.

**[F2]** Evidence MUST retain `CIUKI_TEST v=1`, at most 240 bytes per ASCII
record, unique keys, monotonic sequence numbers per boot (and one run id across sweep boots), decimal counters and
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
| `fd-table` / T0,T3,T4 / U,K | A fixed file workload MUST verify O_EXCL, append atomicity from two descriptions, shared dup/spawn offsets, dup2 replacement/self case, CLOEXEC, fcntl flags, pread/pwrite position preservation, 64-bit seek above 4 GiB without allocation, FAT EFBIG refusal, zero-filled truncate growth, fsync/reopen data, open-unlink lifetime, replacement/case-only/cross-directory rename, EXDEV, cwd reconstruction, pinned rmdir EBUSY and directory cookies/rewind. Rename MUST return EISDIR for file over directory, ENOTDIR for directory over file and EINVAL for a move into its own descendant; identical-entry rename MUST succeed without mutation even for a pinned nonempty directory, while case-only rename MUST update spelling. Fill 128 fd slots, observe EMFILE, close/reuse slots without leaks. Bad buffers/flags/encoding/lengths MUST match errors with zero validation side effects. Read-only/share-conflict and post-commit I/O failures MUST preserve F1 rules. EXDEV and the read-only second-volume check are fixture-backed T3 subcases; without that fixture volume they are `not_run` and do not fail the probe. | Operation IDs, expected/actual offsets/sizes, hashes, file identity/link counts, normalized paths, fd/status flags, directory digest, errno, side-effect counts, handle ledger; checker digest/results for durable workload. |
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
`LC_ALL=C`, `TZ=UTC0`, `HOME=/Documents`, `TMPDIR=/tmp`, and only fd 0–2 inherited.
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

**[F2/T4, f1-34 amendment]** Post-fault ARM/interaction records identify `criteria=operator` for an answered sweep prompt and `criteria=exact` for runner input; both retain `keys >= 2`, `motion >= 1`, `buttons >= 2` and all liveness/redraw requirements. Research (2026-10-11): Linux v6.12's [PS/2 decoder](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/mouse/psmouse-base.c) separates button state from motion; Ciuki's `desktop_input_event` likewise maps `INPUT_BTN` to `CIUKI_INPUT_BUTTON`, with no button mask in motion events. The capture's desktop exit preceded any counted button edge; a type mismatch is not established. Linux's [nonblocking message send](https://raw.githubusercontent.com/torvalds/linux/v6.12/ipc/msg.c) returns EAGAIN on a full queue, as does Ciuki's `channel_send`, so pending replies are retried across gate frames with a 1000 ms command-barrier deadline, below the controller's 3000 ms deadline. Interaction summaries coalesce at 100 ms intervals and do not require drained client queues; fatal gate paths name their reason on stderr. The native harness must consume 3000 motion, 200 key and 74 button events with transient backpressure and remain alive through the last release.

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

### f2-31 desktop applications: libdeskui and Files

Directive `docs/directives/f2-31-deskui-toolkit-files.md`. Research
(2026-10-11), compared with `apps/desktop/{protocol,compositor,input}.c`,
`src/kernel/drivers/i8042.c`, `src/kernel/proc/posixpath.c` and the SDK headers:
Win32 [DrawEdge](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawedge)
(raised/sunken/etched edges), [list-view views](https://learn.microsoft.com/en-us/windows/win32/controls/list-view-controls-overview)
(icon, list with column flow, report with column headers and in-place label
editing), the [500 ms default double-click time](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdoubleclicktime),
[File Explorer keys](https://support.microsoft.com/en-us/windows/keyboard-shortcuts-in-windows-dcc61a57-8ff0-cffe-9796-cb9706c75eec)
(F2, F4, F5, Alt+D, Alt+Enter, Alt+Left/Right/Up), the legacy 0.8 Files
description (`legacy/CiukiOS-docs-legacy-2026-10-09.zip:docs/desktop-apps-2026-09-29.md`),
POSIX [rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html)
(atomic replacement of an existing file, implemented by `px_rename_locked` with
`fat_rename_replace`) and the [QMP input API](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event).

Decisions:

1. **Font.** The compositor has no 8×16 font: its single glyph table is 5×7,
   drawn at scale 1 with a 6 px advance. libdeskui uses exactly that table and
   metrics (17 px list/menu rows), so client text matches window titles. The
   compositor places g, p, q and y on the baseline, where they read as
   capitals; the toolkit draws those four glyphs one row lower. The bitmaps and
   the compositor (with its pinned digests) are unchanged.
2. **Look.** Windows 98 classic geometry (3D edges, menu bar with underlined
   mnemonics and accelerator column, flat toolbar buttons with 22×22 Tango
   icons and labels, sunken fields, dithered scrollbar tracks, raised column
   headers, dotted tree lines and focus rectangles) in CiukiOS colours: the
   3D face equals the compositor's frame and taskbar (#b8c9cc), selections the
   focused title colour (#213c4c). Rendering is integer-only and a pure function
   of state; the build audits every toolkit object for x87/SIMD.
3. **One window.** Protocol v2 allows one window per channel, so menus, context
   menus and modal dialogs (message box, input box, open/save, progress with
   Cancel) are drawn inside the client surface. The client renders into a
   private back buffer and copies only changed pixels, damaging their bounding
   box: the compositor never reads a half-drawn frame.
4. **Window size.** The protocol has no display query. The shell passes
   `CIUKI_DISPLAY=<w>x<h>` to graphical programs it launches; Files opens at
   760×520 on 1024×768, 680×440 on 800×600 and 620×380 below.
5. **Text input** comes from KEY events with Shift and Caps Lock (US set-1
   layout): the native i8042 path emits TEXT only for the unshifted character
   (`input_unshifted`), so TEXT events are ignored.
6. **Event loop.** No poll/select in the F2 ABI: nonblocking channel turns plus
   `nanosleep` of at most 10 ms (1 ms while timers are due); timers use
   CLOCK_MONOTONIC. Focus loss and RESYNC release keys, buttons, captures and
   open menus. The shell SIGTERMs a client two seconds after CLOSE_REQUEST, so
   Files answers a close request during a job by cancelling it (the unfinished
   copy is removed, never a source) and closing, without a dialog.
7. **Files model.** Absolute POSIX paths; typed paths are normalised and take
   each component's stored spelling; bounded history; folders first in every
   sort; Windows 98 type names ("File Folder", "Text Document", "XYZ File";
   extensionless files with an ELF32 i386 header are "Application"). Copies
   stream into an exclusive `.ciuki-copy-N.tmp` sibling, fsync, close and
   publish with rename (which replaces atomically); EXDEV alone makes a move
   copy then delete; copying beside the source makes "Copy of NAME"; an existing
   file or folder asks Yes / Yes to All / No / Cancel, folder merges keep
   unrelated files; Delete is permanent after confirmation; pasted items are
   selected. The status bar shows the selection's size only (a folder total
   would tie it to unrelated sizes such as the kernel's).
8. **Opening.** Folders navigate; text and script types run
   `/Programs/ciuknote <path>` when it exists (it is not in the image yet: Files says
   so); ELF programs run through `ciuki_spawn` with stdio only.
9. **Shell.** The Ciuki menu labels `files` as "Files" with the Tango
   `system-file-manager` icon (also on its taskbar button); `demo` and `files`
   receive `--channel-fd=3`. The canonical menu now has five programs, so the
   two `desktop-shell-*` scripts move 197 px (was 169) from the Ciuki button to
   the Demo row; nothing else in those cases changes.

**API gaps for the lead.** (a) The F2 ABI has no timestamp or FAT attribute
setter (`chmod` is excluded, no `utime`): copies cannot keep dates and
attributes as the directive asks. Moves within a volume keep them (rename).
The model has an optional metadata adapter (exercised by the host fake) and
Files reports "(dates not kept)" after a copy. Closing this needs an
authorised setter or a scope amendment. (b) Programs started from Files get no
desktop channel (there is no channel service), so a graphical program opened
from Files cannot show a window; the Ciuki menu remains the launcher for those.
f2-32 closes this for CiukNote only: Files asks the desktop to start it (LAUNCH
with a path, section f2-32 below).
(c) Properties shows "Read-only" from the informational mode bits only; hidden,
system and archive attributes are not exposed by `stat`.

**QEMU case `files-qemu-t23`** (`tests/suites/f2-desktop.json`). The existing
crash-isolation desktop gate runs; at its interaction ARM the script presses
F11, which selects the Files case in the desktop (shell, taskbar clock fixed at
12:00:00) and adds `files=1 checkpoint=N` to its controller reports. Test
selectors start read-only, so the controller then opens the C: write gate
(`case=files-write-gate`, expected 0) and allows 40 s instead of 15 s for the
interaction. The script opens the Ciuki menu, launches Files (which receives
`--test=files` and moves its frame to 4,36), chooses View > List (dates depend
on build and run time), clicks Places > System, File > New Folder, types
"Ciuki" + Enter, selects BOOT.CFG by typing `b`, Ctrl+C, `c`, Enter (opens
Ciuki), Ctrl+V, Del, Enter (Yes), then Alt+F4 (Files' File > Close) and `a`.
The kernel reports a PS/2 packet's X motion before its Y motion, so a diagonal
move passes through (x1, y0): inside the menus the script moves vertically
first (the first script's direct move from View to List crossed the Go title
and opened Go; checkpoint 1 never came). Files sends each
milestone (1 /system shown, 2 folder created, 3 renamed, 4 copy done, 5 delete
done) as a PING with serial 0xF2310000+N in the turn after the frame's DAMAGE;
the desktop presents that frame and reports one checkpoint per report; the
controller hashes the framebuffer (FNV-1a, as the interaction digest) inside
that report syscall and emits `case=files-checkpoint checkpoint=N
pixel_digest=…`. The final interaction record counts keys=4 only after the
fifth checkpoint and Files' exit status 0. After teardown the controller flushes
storage (`case=files-durable flush_result=0`) and the runner's overlay check
requires a clean fsck and `CIUKI` in `::/SYSTEM`. The five `pixel_digest`
fields hold the placeholder `PIN-FROM-FIRST-RUN`: the lead pins them from the
first run's records (the case fails until then). The host test
`tests/host/desktop/test_deskui.py` checks that the script's relative moves
land on the targets computed from the real Files layout, and
`tests/host/desktop/test_desktop.py` (`AppCaseReplayTests`) replays the whole
case through the production desktop and Files (docs/desktop.md).

Host evidence: `python3 -B -m unittest discover -s tests/host/desktop`
(toolkit geometry/hit-testing/input/menus/dialogs/timers and 9 pinned frames;
the Files model against a fake tree with short I/O and every injected failure;
the Files controller replaying the QEMU script with 23 pinned frames; the
shell launcher's Files convention and checkpoint accounting), plus
`tests/host/test_desktop_gate.py` and the desktop part of
`scripts/test/host_kernel_tests.sh`. QEMU evidence and the owner screenshot
remain lead-owned.

### f2-32 CiukNote desktop application (2026-10-11)

Contract and rationale: `docs/desktop.md` (section f2-32), directive
`docs/directives/f2-32-ciuknote.md`. This section records the acceptance decisions only;
it adds no mandatory F2 row and no `f2-all` probe.

**[F2+, f2-32]** `/Programs/ciuknote` MUST be a payload of the canonical image (builder, SDK ELF
checks and manifest as `/Programs/files` and `/Programs/tasks`). Its host evidence is the text area
model and rendering test, the application test with a fake file system (20 pinned screens,
a 25,000-step random input run), the replay of the committed QMP key script, and the
desktop launcher test of the CiukNote scenario and of LAUNCH with an argument; none of it is
guest qualification. `apps/desktop/protocol.h` changed, so the SDK (which installs it as
`ciuki/desktop.h`) MUST be rebuilt before `make desktop`.

**[F2+, f2-32]** Opening a text file from Files: the desktop MUST honour LAUNCH with an
argument only from a client it launched as `/Programs/files`, only for `ciuknote`, and only with
an absolute path (at most 160 bytes, no control characters); it MUST answer every other
LAUNCH with an argument `-EPERM`, and the Task Manager's argument-less LAUNCH is unchanged.
Files MUST refuse a path the wire cannot carry itself and MUST report a negative TASK_RESULT.
The host launcher test covers the wire rules, Files, CiukNote, an ordinary client and the
Task Manager.

**[F2+, f2-32]** QEMU case `ciuknote-qemu-t23` (`f2-desktop`, profile `qemu-t23`) uses the
crash-isolation selector `server=desktop`, the Files case's predicates with `note-*`
records, three checkpoints and `keys eq 4`. Scenario selection is generic: the key pressed
at the interaction ARM selects the case (F9 Task Manager, F10 CiukNote, F11 Files, F12
shell). F10 and F11 are app scenarios sharing one mechanism: the first program of that name
started from the Ciuki menu receives `--test=<name>`, checkpoint PINGs (Files
`0xf2310000+n`, CiukNote `0xf2320000+n`) are reported one per presented frame as `files=1`
or `note=1` plus `checkpoint=N`, the controller hashes the framebuffer in that report,
opens the storage write gate for these cases only and allows 40 s, and the A key edges
count only after the last checkpoint and a clean exit: `keys=4`. The case MUST fail if the
application never starts, does not reach its three checkpoints or exits uncleanly. The
distance from the Ciuki button to menu row k of n programs is `-1 - 28 * (n + 2 - k)`; `/Programs`
lists seven programs (ciuknote, demo, files, hello, libc_smoke, lua, tasks), so `ciuknote`
is 253 px up. A host test fails when the image builder's `/Programs` inventory changes.

**[F2+, f2-32]** Durability and content: the controller flushes storage after the
desktop stops (`case=note-durable flush_result=0`). After QEMU stops the runner MUST digest
`::/system/tests/note.txt` with the declared SHA-256
(`8295f4d0977502fc61022209d665ff588382b0bd1564e5e38736f022e57f807d`, the typed text with
CRLF and the fixed `9:41 AM 10/11/2026` stamp) and `fsck.fat -n` MUST return 0. The pass
criterion is guest events and the host digest, not QMP success.

**[F2+, f2-32]** Screen evidence: `desktop_screen` is the runner's whole-screen before and
after capture (clocks included, never pinned). The three `note-checkpoint` `pixel_digest`
fields hold `PIN-FROM-FIRST-RUN` until the lead pins them from the first run's records (the
case fails until then). The application also prints `CiukNote checkpoint=1|2|3
event=ready|saved|opened ... digest=XXXXXXXX` (FNV-1a 32 of its own 640x440 surface), whose
expected values come from the host replay (`ready 780ef981`, `saved 7f605aaf`, `opened
033a8c51`); a mismatch with an otherwise passing case is a rendering or event-order
difference to explain, not to overwrite.

**[F2+, f2-32]** Time budget: the interaction window is 40 s from ARM for app scenarios; the
script's nominal pacing is 23.3 s and `test_ciuknote.py` rejects a script above 30 s.

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

## F2-33 Task Manager (2026-10-11)

**[F2+, f2-33]** Implementer draft; lead review and QEMU evidence pending. The
ABI rows (calls 70 `proc_list` and 71 `proc_terminate`, records, errors and the
authority descriptor) are in
[execution-abi.md](execution-abi.md#f2-33-amendment-process-enumeration-and-termination-requests-2026-10-11);
the SDK wrappers in [posix-subset.md](posix-subset.md#f2-33-amendment-process-enumeration-and-the-termination-authority-2026-10-11);
the desktop protocol extension and the application in [docs/desktop.md](../desktop.md).

Decisions. The process table records an image basename and the publication
tick; the scheduler counts idle ticks (`g_idle_ticks`), so CPU usage is the
busy share of timer ticks and per-process CPU is the tick delta of
`cpu_ticks` (`file_clock_tick`) over the interval; PID 1 carries the busy
ticks not charged to any process (the Windows "System" role) and a synthetic
"System Idle Process" (PID 0) row shows the idle share. Termination authority
is a descriptor, delegated like the F4-01 mixer handle: PID 1 installs it in
the bootstrapped (or probe) desktop, the desktop maps it to fd 4 only for
`/Programs/tasks`, and the kernel honours it only for its owner and the owner's
direct children. The desktop's own group (the desktop and every program it
launched, including the Task Manager) is never a `proc_terminate` target
except for the desktop itself: Task Manager ends those processes through the
desktop's existing close path (CLOSE_REQUEST, SIGTERM after 2 s), and refuses
PID 0/1 and the desktop. Update speeds follow the commonly reported Windows
values (High 0.5 s, Normal 1 s, Low 4 s, Paused; Windows XP used 2 s for
Normal), with one history point per second over 60 s.

**[f2-33/T3]** `tasks-qemu-t23` (`tests/suites/f2-desktop.json`) is the
`desktop-shell-qemu-t23` case with the same probe, selector, deadline,
predicates (`keys` = 4) and `desktop_screen` evidence and a different script,
selected at run time by F9 (F12 is the shell case, F11 the f2-31 Files case, F10 the f2-32 CiukNote case)
at the post-fault interaction ARM:

1. F9 makes the gate desktop enter shell mode with the tasks scenario
   (`tasks_test`); both F9 edges count towards `keys` like the shell's F12.
2. The script clamps to the bottom-left corner, clicks the Ciuki button
   (40, h-17) and the `tasks` menu row (40, h-102), from the menu formula
   below with k = n-1: `tasks` is the last program in the sorted `/Programs`
   scan, so its row sits directly above "About CiukiOS" whatever n is. A
   future `/Programs` program sorting after `tasks` moves it. Stage 1: launched from the menu with `--authority-fd=4
   --test=tasks`; stage 2: its window exists.
3. Alt+N, `demo`, Enter: Task Manager's New Task sends the v2 LAUNCH request;
   the desktop launches `/Programs/demo` as its own child (stage 3), whose window
   follows (stage 4). Task Manager selects the launched PID's row.
4. While the demo starts, the script's waits move the pointer onto Task
   Manager's taskbar button (300, 751: slot 1 of three windows, x 244..403)
   and clicks it to take keyboard focus back from the demo window (a click
   into Task Manager's client area would be hit-tested at a stale pointer
   position: the desktop sends a newly focused client no MOTION before the
   BUTTON), then Alt+E (End Task) sends END_TASK for the demo's window: the
   desktop requests its close (stage 5); the demo acknowledges and exits 0
   and the desktop reaps it (stage 6).
5. Task Manager's next 0.5 s refresh no longer finds the PID in call 70;
   it then requires call 71 with its delegated descriptor to refuse the
   ended PID (ESRCH, which also proves the descriptor is a valid authority
   since EBADF is checked first), PID 1 (EPERM) and the desktop (EPERM), and
   only then pings the private checkpoint serial `0xf2330007`; the desktop then checks
   its own call-70 snapshot (no record for the demo PID, Task Manager and the
   desktop live) and reaches stage 7 (`DESK_TASKS_DONE`).
6. Only then do the two A edges count, so `keys=4` proves the whole sequence;
   omission or failure at any step leaves `keys=0` and the 15 s interaction
   window expires (`changed=0`). The script has 40 batches (pauses 3.2 s;
   several key taps per batch): the first script's 107 batches needed about
   31 s at the pace measured in the first QEMU run of `files-qemu-t23`
   (0.33 s per 100 ms batch), twice the 15 s window, which applies to this
   case because it reports no app flag; the host replay puts the new script's
   last batch at about 11 s at that pace.

Task Manager stays open for the post-input screendump (400x420 content,
frame x 8..414, y 40..487 at 1024x768): it covers neither the upper-right portrait region
nor the centre cursor footprint that `observe_desktop_screen` checks. The
native cleanup now also collects every remaining child of the desktop before
the desktop itself (generalizing the survivor/victim rule), so a program left
open by a script cannot be adopted by PID 1 past the ledger.

Menu click formula for scripted cases (`apps/desktop/compositor.c`
`desk_menu_row`/`desk_menu_box`, `client.c` `desk_shell_click`,
`desk_program_add`). The desktop admits at most 16 `/Programs` ELF programs
except `desktop`, sorted by `strcmp` of their names (ASCII byte order, not
the displayed label). With n admitted programs, screen height h and the
32-pixel taskbar, the row height is r = min(28, floor((h - 64) / (n + 2)))
and the menu is anchored above the bar: its top is y0 = h - 32 - r(n + 2);
program k (0-based in sorted order) occupies [y0 + kr, y0 + (k+1)r), then
"About CiukiOS" (k = n) and "Restart" (k = n + 1). A row is selected by
floor((y - y0) / r). The scripts clamp to the bottom-left corner and move
(+40, -16) onto the Ciuki button, i.e. to y = h - 17, so the vertical move
to the centre of program k is dy = y0 + kr + floor(r/2) - (h - 17)
= -15 - r(n + 2 - k) + floor(r/2), which for r = 28 is dy = -1 - 28(n + 2 - k),
independent of h. It holds while r = 28, i.e. h >= 64 + 28(n + 2). For the
desktop batch image (n = 8: ciuknote, demo, dosbox, files, hello, libc_smoke,
lua, tasks) the suite uses `desktop-shell-*` Demo k = 1, dy = -253;
`files-qemu-t23` files k = 3, dy = -197; `ciuknote-qemu-t23` ciuknote k = 0,
dy = -281; `tasks-qemu-t23` tasks k = 7 = n - 1, dy = -85 (the last program,
so independent of n). `tests/host/desktop/test_tasks.py`
(`test_menu_clicks_follow_the_image_inventory`) derives n and k from
`scripts/build_image.py` and checks every scripted case, so adding or
removing a `/Programs` program fails the host tests until the offsets follow.

The case pins no
pixel digest in the suite (like `desktop-shell-*`): the runner records
`desktop_screen.ppm_sha256` and `before_ppm_sha256` for the image it ran on;
the lead pins or archives them from the first passing run. Host renders of
the four Task Manager views are pinned in `tests/host/desktop/test_tasks.py`.

Host evidence (no guest qualification): `tests/host/proc/proc_test.c`
`test_enumeration` (record layout and order, truncation, EINVAL/EFAULT,
names, accounting over a fake process set, zombies, EINVAL/EBADF/ESRCH/EPERM,
descendants, delegated authority and its one-level limit, desktop-group
protection, kernel controller owners, leak-free release),
`tests/host/abi_layout_test.c` (both records and all constants),
`tests/host/desktop/shell_launch_test.c` (v2 opcode parsing, window list,
End Task/Switch To/LAUNCH rights, authority mapping to fd 4 only for Tasks,
scenario stages, the desktop's call-70 verification and `keys`),
`tasks_model_test.c` and `tasks_view_test.c` (model with fake data, layout,
mnemonics, pinned renders). `f2-all` is unaffected by construction: no F2
gate program issues calls 70/71, F9 is never sent by another case, and
the only behaviour change outside the tasks scenario is one more descriptor
in the desktop (counted before every baseline) and the cleanup above, a
no-op when the desktop has no extra children.
