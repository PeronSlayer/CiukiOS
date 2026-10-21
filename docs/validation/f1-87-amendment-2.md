# F1-87 Amendment 2: admission and VM scheduling

Research checked on 2026-10-21, before implementation:

- Microsoft's [Win95 SDK Priority Inversion](https://techshelps.github.io/Win32Api%20Documentation/html/o9bn.3.htm)
  describes temporary dependency-owner boosting. Keep WAIT_PRESENT donation;
  do not turn the focused client's promotion into the VM's configured base.
- Microsoft's [Win95 DDK Deadlocks](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM)
  separates ring-0 scheduling from Kernel32 waits and warns against waiting
  for a thread that cannot run. Presentation must retain its bounded wait.
- Microsoft's [IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
  distinguishes request operations and current-thread contexts. Preserve
  explicit operation/site attribution and thread-specific refill demand.
- Microsoft's [IOS Services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM)
  describes request packets and completion status; it does not establish the
  numeric admission or boost limits below.

The binding policies are C3 and R2/R3 of
[the resource contract](../design/win98-class-resource-management.md) and
[the scheduling ABI](../design/execution-abi.md#threads-and-scheduling).
Close urgent overtaking when any queued volume ticket reaches 100 ms, so
existing readers can drain. This bounds admission, not an already-issued
device command or a logical mutation (B belongs to f1-89). Refill urgency
requires a writable started stream, low buffered PCM, and a bounded demand
from the actual reading thread. Input propagation expires after the existing
20 ms grant and is withdrawn on native or DOS focus loss. These are CiukiOS
limits, not claimed Windows 98 constants.

The additive `AUDIO_REFILL_READ` command uses the existing 16-byte command
record. It grants the calling decoder thread at most 64 KiB/20 ms of source
read urgency against a writable, started stream with at most 1536 queued
frames. Reset/close generation checks prevent stale descriptor reuse.
Music invokes it from its reader, with its existing reservoir mutex
serializing the short demand command against output close/reopen. The
decoded reservoir remains two seconds; PCM accounting is unchanged.

`vfs_hold tagged=` is a bit mask: 1 means an explicit operation, 2 an
explicit site, and 3 complete attribution. Fallback text keeps the
corresponding bit clear across reservation and I/O re-entry. The host hold
observer rejects anything other than 3; explicit `metadata` remains a valid
operation. Native reader admission publishes one accumulated successful wait
at completion with the first blocking owner's copied tags. The existing
100 ms wait threshold and eight-record/two-second suppression remain.

The continuous VFS replay keeps each complete reconstructed exclusion
interval (125–1837 ms), runs all 555 operations, and uses production native
reader admission before the production block request. The negative control
represents the former coarse volume conflict as all file identities
conflicting; it changes no production policy switch. A 250 ms decoder
admission deadline follows the fixture's 12000-frame credit per completed
read. It is distinct from the scheduler's device CPU budget: sleeping for
storage does not consume that CPU budget. Report both metrics separately.

Host evidence and exact commands are recorded in `build/codex/f1-87-report.md`.

Final host evidence: 14 latency/scheduler/replay tests and nine Music tests
pass; all seven changed kernel units compile with canonical i686 flags.
The DOS file-API/admission regression runner passes. All FS functional
groups, three fsck checks and three mtools comparisons pass. Its final
artifact audit initially found older host executables; removing regenerable
executables and rerunning that unchanged audit gives 179847671 logical
bytes, below its 200000000-byte limit. The FAT CPU-time check passes at
879 us against 1000 us; an earlier attempt exceeded that unchanged limit.

Audio (11255008 checks) and DOS VM (64390705 checks) reach functional PASS,
then LeakSanitizer fails because sandbox ptrace is unavailable. The full
kernel runner stops at the same restriction before later groups. LSan was
not disabled in those runners; the lead must rerun them outside the sandbox.

The VFS positive control has zero admission misses and PCM starvation;
its old coarse-conflict control has 89 misses and 38 starvation events.
Both have zero audio-refill CPU-budget overruns. The separate 5 ms CPU-turn
negative control triggers the real sampler. Reproducing the trial's specific
CPU-overrun symptom from sleeping volume exclusion remains unestablished;
the lead must reconcile that negative-control metric. No T23 runtime or
OS image qualification is claimed.
