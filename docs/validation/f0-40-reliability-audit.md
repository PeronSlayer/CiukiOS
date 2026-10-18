# F0-40 core-path reliability audit

Scope: the production checkout after f0-39/f1-80/f3-43, audited on
2026-10-19. Host evidence cannot qualify visible T23 scanout or firmware
latency. The five `legacy/local/physical/<image>/BOOT*.LOG` directories were
read through the main checkout: those untracked logs are absent from this
worktree. No physical log was copied or modified. The cited old QEMU run
remains directive-only evidence.

Direct log checks (line numbers count the original padded files as text):

| Trial / file | Observed evidence |
| --- | --- |
| d598bf4c / BOOT1.LOG:1944–1964, BOOT2.LOG:416–417 | Desktop stalls reach 8715/8884 ms; USB scan takes 8704 ms and runs at 0.114 scans/s. Earlier native restores show the old CR33 mismatch; reserved-bit coverage remains in place |
| 43d72a5c / BOOT2.LOG:509, 685–686 | A 3266 ms desktop wait names `media-read` / `directory-compare`; native restore can also succeed (`restore_us=185694`), so the recipe is not permanently broken |
| 041b221f / BOOT.LOG:389–444, 732–849 | 2123 us NIC handler gap precedes IRQ 11 quarantine of AC97 and all three USB members. Return checkpoints report EAGAIN; last restore checkpoint is streams, followed by 151–160 ms DOS VM stretches |
| eca4d773 / BOOT1.LOG:246, 585–586, 668 | Stick mount 252 ms; native claim conflicts with retained `savage-vga`, then the desktop-frame timeout. Existing f3-43 generation/takeover tests exercise both causes |
| 9af46936 / BOOT.LOG:1, 540–565 | UART state 2. Restore blank starts at tick 103597, panel-clock at 103637 (40 ms later), VGA at 103638, streams at 103687; final result -5 with reg/mask zero, restore 96231 us. VBE returns zero, yet strike 1 is charged. Lease/frame returns take 2114/2190 ms against the 2000 ms target |

The 40 ms first-stage gap plus no register mismatch strongly implicates the
first retrace deadline; the log does not directly record that wait, so this
remains an inference. New wait/delay-specific records distinguish it on the
next hardware trial.

## Research and decisions before implementation

- [Linux lockup watchdogs](https://docs.kernel.org/admin-guide/lockup-watchdogs.html)
  distinguish failure to schedule from failure to receive interrupts.
  [hung_task](https://docs.kernel.org/admin-guide/sysctl/kernel.html#hung-task-timeout-secs)
  reports blocked tasks rather than establishing device progress. Decision:
  name the stage, retain independent clock/iteration bounds, and distinguish
  detection from recovery. A cooperative loader watchdog cannot preempt a
  BIOS call that never returns; no hardware watchdog is programmed here.
- [Microsoft Windows 98 KB 188867, archived Microsoft text](https://ftp.zx.net.nz/pub/mirror/ftp.microsoft.com/MISC/KB/en-us/188/867.HTM)
  describes minimal protected-mode drivers and standard VGA in Safe Mode.
  [Microsoft NT-family startup troubleshooting](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/troubleshoot-startup-problems)
  distinguishes Safe Mode from Last Known Good configuration. Decision:
  retain Ciuki's explicit safe input/driver policy and verified text/VBE
  fallback; do not introduce an automatic configuration rollback policy.
- [SeaBIOS runtime boot order](https://raw.githubusercontent.com/coreboot/seabios/master/docs/Runtime_config.md)
  tries configured boot methods in order; its
  [boot implementation](https://raw.githubusercontent.com/coreboot/seabios/master/src/boot.c)
  owns that policy. Decision: optional video failure falls back to text;
  failed kernel loading stops visibly, never jumps into a partial ELF or
  invents a new boot device. [SeaBIOS clock.c](https://raw.githubusercontent.com/coreboot/seabios/master/src/clock.c)
  documents the BDA daily tick wrap and INT 1Ah time service. Reuse the
  loader's existing BDA clock without installing a new interrupt handler.
- [X.Org SavageWriteMode / SavageResetStreams](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
  restores mobile timing and starts streams last. [Linux savagefb's
  retrace helper](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb.h)
  reads VGA status bit 3 only when CR17 bit 7 enables timing. Decision:
  preserve all writable-register readbacks and the existing programming
  sequence. During recovery a missing retrace edge in the blanked/disabled
  stream pipeline is an explicitly recorded synchronization warning, not
  proof of a failed register restore. Entry remains strict. The observed
  `-5, reg=0, mask=0` is consistent with a folded wait/delay error, not a
  register mismatch. The original log's 40 ms blank-to-panel-clock gap
  strengthens the first-retrace-timeout inference; hardware confirmation
  remains necessary.
- [Linux USB hub handling](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/core/hub.c)
  bounds reset/enumeration and disables the affected port after exhausting
  retries. [USB-IF USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification)
  is the governing hub/port reference. Decision: root-port reset/capacity
  failures use existing port-disable operations and retain sibling devices;
  uncertain controller DMA/interrupt isolation still requires controller
  quarantine. No new register programming.

- [Linux writeback error tracking](https://docs.kernel.org/core-api/errseq.html)
  retains errors at the object whose work failed. Repository evidence:
  `cache.c` already retains each device's error, but `storage_writeback`
  also copied it into the global writer-startup error, preventing write
  activation and BOOT.CFG updates on healthy volumes. Decision: retain
  device errors in the existing cache, revoke only affected volumes, retain
  the all-volume shutdown error, and reserve the global field for a failed
  writer service. Identify the affected drive in a protected record.
- Repository evidence for final power records: `storage_sync` already
  invokes `blackbox_checkpoint` after `storage_shutdown`; that reserved raw
  sink does not depend on a mounted FAT or the stopped BOOT.LOG writer.
  Decision: use that existing checkpoint after final power/fallback records,
  before hardware reset/soft-off, without reopening the filesystem.
- [Linux generic IRQ handling](https://docs.kernel.org/core-api/genericirq.html#c.free_irq)
  separates device identity/removal from shared-line ownership. Repository
  evidence: f0-39 detaches a quarantined member, but `registry.c::claim`
  still treats its retained IRQ row as an exclusive overlap for a newly
  activated peer. Decision: a different owner may acquire a shareable IRQ
  alongside that retained row only while a surviving chain member exists
  and the line itself is not quarantined. Retain the failed generation,
  refuse same-owner restart, empty/unsafe lines and exclusive resources,
  and emit a protected admission record. No PIC transaction changes.

## Invariants and failure domains

All `klog` and `klog_periodic` rows below are protected BOOT.LOG publications
and enter the independent RAM black box through `core/output.c`, including
quiet boots with `loader_uart state=2`. Verbose `klog_diag` rows are supplementary.
Protected means priority over droppable records, not unlimited storage or
guaranteed durability after loss of the boot disk. A failed disk sink cannot
record its own future failures on that same disk. Early boot uses the bounded
protected prefix; runtime uses class-aware queue admission. BOOT.LOG has a
finite capacity and a finite drain rate; power loss before a commit can still
lose queued records. The reserved-chain recovery and independent raw recorder
reduce that gap; neither is a hardware watchdog.

| Path and invariant | Single points of failure / isolation | Bounds and fallback | Protected diagnosis | Failure-injection regression |
| --- | --- | --- | --- | --- |
| Boot/menu/loader: a returned menu makes observable progress; no partial ELF executes | BIOS execution, selected boot disk, loader CPU and BDA clock. Pre-kernel failure has only screen/UART and a volatile trail; a successful handoff replays the trail before loader reclaim | Existing 3 s menu / 30 s selector; outer 60 s menu, 20 s video, 120 s ELF, 3 s handoff; each stage also 0x2000000 checks if ticks freeze. Video timeout selects text; essential ELF timeout halts; no key wait at fatal. Disk retains three firmware retries. Firmware calls themselves are not preemptible | `L:STAGE`, once-only `L:STALL <stage>`, `L:FAIL`; kernel `[loader] stage=... completed=... stalled=... failed=...` | New assembled `reliability/test_loader_progress.py`: every stage, frozen tick, midnight, partial ELF refusal, warm reset, video/menu/ELF loop wiring. `test_loader_menu.py`, `test_loader_model.py`, `test_boot_modes.py`: menu/firmware policies and actual UART instructions |
| Input PS/2 + USB: a bad source cannot revoke an independent source; bounded controller initialization | The i8042 data/command pair is one controller domain (keyboard and auxiliary share it); firmware VM/backend is another; USB host/port/HID lifetime separate. i8042 quarantine legitimately stops both PS/2 devices, retains ambiguous leases, leaves USB alone | i8042 setup 500 ms, replies 200 ms with finite poll/resend bounds; firmware failure permits one native attempt after safe lease handoff. USB interrupt loss polls; failed device/port blocks until reboot | `[init] ... result/error/reason`, `[i8042]` named initialization steps; `[input] devices/source/event`; USB quarantine and `irq_lost` rows | `i8042_test.c`: stalled clocks, ACK/RESEND, firmware/native handoff, active-fault survivor; `usb/input_grant_test.c`, `usb_test.c::irq_loss_polling`: mouse/keyboard events survive lease loss; generation/release tests |
| Boot volume + removable storage: failure stays with its disk/volume, and unrelated reads/writes continue | Boot disk is a system dependency and evidence sink. Per-device cache errors and per-drive VFS admission isolate removable faults. Global writer-service allocation failure legitimately prevents writable activation everywhere | Existing transport bounds, cache fill/error propagation, FIFO sleeping locks; faulty FAT becomes read-only and remains dirty. Removed media cannot retain live references. A delayed device error no longer sets the global writer-startup error; all-volume shutdown still fails on dirty/failed media | Existing `[storage] readonly reason/file/cluster`; new `failure stage=writeback drive/disk/result scope=volume fallback=readonly`; repair/mount/log-sink results | New `fs/test_storage.c::writeback_isolation`: write and flush errors on D: leave C: writable, identify D:, still fail all-volume shutdown. `test_cache.c`, `test_storage_namespace.c`, `wait_latency_test.c`: 25 ms failing reads, coalesced fills, independent volumes and sleeping admission. `test_log_recovery.c`, `test_powercuts.c`: cut/reopen durability |
| Display modes/restore/full screen: return the desktop after a recoverable native failure, retain a windowed guest after refusal | One physical scanout is shared; unknown DMA writer or failed VBE backend cannot be safely bypassed. Proven accelerator access takeover remains scoped to exact ranges/generations. A recoverable native restore no longer exhausts native availability | Native retrace: 40 ms plus 65536-read cap per two-edge wait; missing recovery edge is recorded and saved controls still restored/read back. Entry wait stays fatal. Guest park 250 ms, return target 2 s including fallback, missing-frame watchdog 10 s; check-mode 5 s with one restoration attempt | `[display] fullscreen_step`, `native-restore-retrace-off/on result=-110`, provider readback stage/register/expected/actual/mask, native/fallback results, recovery verified/failed, strikes and lease/frame timings | New `supersavage_mode_test.c::restore_without_retrace`: stuck low/high, frozen time, real PLL write loss, delay failure, strict entry timeout. `supersavage_display_test.c::strikes_and_retry`: four recovered restores cost zero; failed fallback consumes one/session. `display_test.c`, `fullscreen_checkpoint_test.c`, `dosvm_fullscreen_test.inc`: slow logging, forced park, fallback, present generation, repeat refused request |
| USB controllers: a lease-lost IRQ, failed port or child cannot detach unrelated root devices | Device register/DMA lease loss, stopped frame clock, fatal HC state require that controller to stop and pin DMA; a physically stuck shared IRQ can require whole-line quarantine. Root reset/capacity faults now isolate a port; failed hub child-disable isolates that hub's root port. Other HCs and PS/2 survive | IRQ loss disables local interrupt source and polls; HID 20 ms fail-closed watchdog, control 500 ms, root debounce 2 s, BOT bulk 10 s and bind 20 s with existing reset/clear recovery. No automatic retry of a quarantined port | `[usb] irq_lost ... mode=poll`, new `quarantine controller/port stage/result scope=port`; device/hub/HC reason and isolation; BOT stage/packet/recovery trace | New `usb_test.c::root_port_isolation` for UHCI/OHCI: reset timeout, capacity exhaustion, failed child-disable, surviving mouse events and no reset storm. Existing `irq_loss_polling`, HC-fault/frame-clock and `usb/f1_74_test.c` BOT recovery cases |
| Audio transport: a failed engine stops or retains its DMA safely; audio quarantine cannot revoke USB/NIC peers | Codec/DMA device and its leases form the quarantine domain. Shared IRQ member detach is independent; physical stuck line is an unavoidable common domain. No transport or refill policy changed here | Existing backend activation/stop budgets (including 500 ms DMA proof), runtime progress guard; unproved stop retains pages/claims and inhibits reuse, software callers receive the existing error | Existing `[audio] backend failure err/op` with triggering ring state, activation `failure stage`; new shutdown failure backend/reason/IRQ/fallback, including AC97 wrapper; protected `[vm-audio]` cadence and late-ACK counters | `audio/audio_test.c`, `audio/second_set_test.inc`: failed stop retains pages/leases, now asserts shutdown-stage record. Existing late refill/recovery/second-set cases. `dosvm_audio_test.inc` and `dosvm_game_audio_test.inc`: f3-40 late acknowledgements and independent DMA cadence |
| DOS VM lifecycle: a stopped/faulted VM loses callbacks/ports, retains resources until quiescent, then cleans up | One VM's address space and overlay generation, not the entire native scheduler. Display return may pin a VM temporarily. COW cleanup errors retain the generation for retry | Destroy wait 6 s; failure retains ownership, never frees a running task. Full-screen park/restore bounds above. No VIF or virtual IRQ logic changed | New quiet-boot `lifecycle stage=stop` with VM/generation/reason/vector/EIP; `failure stage=destroy-wait/destroy-display ... fallback=retain`; `lifecycle stage=cleanup result/data_loss/retained`; existing full-screen and disk errors | Extended `dosvm_test.c` fault-worker/cleanup assertion; existing destroy retry, stale generation, COW failure and full-screen lifecycle cases. Scheduler/transport stretch replay runs independently of VM IRQ semantics |
| Shutdown/reboot: no reset/power register write before durable storage acceptance; a failed power mechanism has a bounded fallback | Boot storage, ACPI table parsing and legacy reset mechanism; unclean media refuses power action. Unsupported soft-off returns a safe-to-turn-off result after storage stop | EBUSY/EIO refuses before hardware, repeat requests sync again; ACPI SCI_EN 300 x 10 ms, no-effect sleep 2 s; 8042 reset poll 10000 iterations then triple-fault reset, UART drain bounded | `[power] requested`, storage flush failure, ACPI step/SCI_EN/no-effect/no-soft-off; new prepared/reset-stage records. Final diagnostics checkpoint the reserved raw recorder after FAT shutdown and replay on the next boot; a failing sink cannot block hardware fallback | Extended `power_runtime_test.c::test_power_sync_retry/test_power_sci`: failed-flush record, no PM writes on failure, SCI_EN timeout/reboot, final no-effect/unsupported soft-off checkpoints, recorder EIO still reboots. New `fs/test_storage.c::shutdown_recorder`: real raw publication after storage_sync, zero subsequent FAT writes. `power_test.c`: malformed tables. `sweep_test.c`: stuck 8042 reset falls through to triple-fault |

## Review of the historical fixes

- f0-33/f0-35/f0-39: `kernel_sync_test.c::test_chain_smi_budget` and
  `test_irq_chain` distinguish the injected SMI-sized gap from repeated
  unattributed handler work, detach a faulted member while serving peers,
  and still quarantine 1000 unclaimed passes / a full continuation ring.
  Existing registry generation checks stop a reused handle from poisoning a
  new owner. The audit found a remaining admission cascade: a retained
  quarantined IRQ row rejected a different future sharer although surviving
  members continued. The new regression fails before the fix at
  `newcomer>=0`, then passes with a different-owner, exact shared-range
  admission on the still-used line. Its protected `stage=irq-member-admit`
  record names both owners. Same-owner retry, exclusive claims, an empty
  retained line and the existing physically stuck/inconsistent line remain
  refused. The failed member's generation/lease stays quarantined. PIC
  writes remain unchanged. `registry_probe.c` adds only the corresponding
  shadow-fixture symbol rename required by its second production include.
- f0-34/f0-36/f0-38: `bootlog_test.c::class_pressure`,
  `pressure_watermarks`, `stalled_metadata` and `recovered_prefix`, real FAT
  recovery/cut tests, and assembled UART tests cover protected retention,
  slow/failing devices, unpublished log tails, absent COM1 and disabled
  serial. Loader progress replay uses that existing protected path.
- f1-78/f1-79/f1-80: the production scheduler replay exercises
  `fs_lock_take`, `fs_event_wait` and VFS admission with a 25 ms device
  read that subsequently fails; waiters consume zero modeled CPU and the
  other volume continues. `fullscreen_checkpoint_test.c` injects a sink
  that does not drain: return checkpoints enqueue without EAGAIN/I/O on
  the display worker. The separate display/VM tests exercise forced park
  and resumed presents. No locking model or FAT durability rule changed.
- f3-36/f3-39/f3-43: retain computed-panel/read-only masks and every real
  writable readback. The old `streams_off` folded `retrace()` timeout into
  EIO, while restore kept updating `s->stage`, so its final message could
  say `native-restore-streams reg=0 mask=0` even when the first blanking
  wait failed. Both recovery waits now emit their own protected result;
  failed delay and strict entry wait identify their actual stage too.
  Recovery waits are advisory; readback/delay failures still request VBE
  fallback. The final commit decision, including any VBE recovery, controls
  whether a restore costs a strike. Entry failures still cost one.
- f3-40/f3-41: existing DMA/late-ACK/tiny-auto-init cases inject delayed
  interrupt acknowledgement, rather than re-clocking DMA on the ACK.
  Scheduler, VGA safe-point and presenter cost tests cover the long kernel
  stretches. f3-44 owns subsequent VIF work; f4-31 owns audio transport.
  This audit adds lifecycle/stop diagnostics only in those areas.

## Loader trail and limits

`CLDRPRG1` is an optional private record at offset 24544 of the unchanged
24576-byte loader file: eight magic bytes; four little-endian u32 masks
(entered, completed, stalled, failed); eight reserved zero bytes. Stage bits
0..3 mean menu/video/ELF/handoff. It changes neither boot_info nor native
public headers. A warm loader entry clears every mask. The kernel checks
the loader extent and magic, rejects unknown/inconsistent mask bits, and
copies records through `klog` before `pmm_init` reclaims loader memory.
The disk-sector scratch buffer moves to already reported loader BSS; a
successful sector read initializes all its bytes before use. Mode-ranking
and selector scratch offsets and the fixed loader file extent stay intact.

Checkpoints occur between firmware calls and at menu/video scan/ELF copy
and BSS-zero loops, plus before/after disk reads. The menu's outer bound is
not restarted by repeated safe/log/selector keys. The handoff is straight-line
CPU setup after its token. Each bound includes a finite check counter;
counter bounds are not calibrated elapsed-time guarantees. Video expiry
chooses text or omits optional EDID after a committed mode; ELF expiry
cannot transfer control to a partial kernel. BIOS text/disk/video calls that
never return, IF-off firmware and complete CPU lockups remain outside this
cooperative mechanism. A pre-kernel fatal cannot replay a RAM trailer into
BOOT.LOG; it does display its stage without an indefinite keyboard wait.

## Quarantine cascade inventory

The preventable cross-function policies corrected here are: retained IRQ
member blocking a different future sharer; root-port reset/capacity failure
stopping all USB root devices; failed hub child-disable stopping another
root port; a delayed removable write error poisoning healthy volumes' write
admission; a recovered display restore exhausting native full-screen strikes.
The already fixed f0-39 IRQ-member detachment and f3-43 safe accelerator
handoff remain covered.

Necessary shared failure domains remain explicit: one i8042 controls both
PS/2 ports; a failed USB HC owns every schedule/DMA descriptor on that HC;
unproved display DMA shares one scanout; a physically stuck IRQ affects its
line. `biosvm.c::quarantine` also stops both firmware input and video when
they share its disabled firmware execution context. The video/native-input
tests prove that a native input source is independent, not that the same
faulted firmware VM can safely resume keyboard calls. Native PS/2 and other
USB controllers remain separate. No faulted firmware context or unproved
hardware writer is reenabled to hide those common dependencies.

## Host verification (2026-10-19)

The final source passes the full
`ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh` gate,
the standalone filesystem/audio/USB suites, and 68 standalone loader checks
(39 model/menu, 13 boot-mode, six version, four assembled progress and six
MBR/handoff statics). Each job ran sequentially at `nice -n 19`.
`python3 scripts/gen_api_reference.py` produces no API-reference diff;
`python3 scripts/build_kernel.py` passes its FPU/SIMD audit;
`make CAP= apps` validates and links the SDK/application payloads with guest
qualification `not_run`. `git diff --check` passes. No OS image or QEMU run
was produced. The full host gate skips its optional existing-DOSBOOT guest
workload because this worktree has no prebuilt DOSBOOT image; available
single-step CPU and ordinary VM model tests pass.

The lock/event/admission replay gives zero waiter CPU and a maximum modeled
stretch of 1 ms with three delayed reads and one device error. Independent
volume/media calls peak at 1 ms against their 50 ms limit. Full-screen return
replays take 684 ms normally, 784 ms with a 100 ms park delay, and 410 ms when
the guest never parks and capture is skipped. Four return log checkpoints
use 0 ms of caller time and commit on the storage worker. USB tests preserve
sibling events across port/hub faults and verify no port write after HC
quarantine. The IRQ admission regression fails before the fix and passes
after it while retaining all exclusive/empty/stuck-line refusals.

Detailed command logs, kernel/app hashes and integration groups are recorded
in `build/codex/f0-40-report.md` in this worktree. These host results do not
qualify an image SHA or visible output on physical hardware.

## Remaining qualification and evidence gaps (priority order)

1. Lead: fresh-image boot smoke and the existing focused QEMU gates, then
   T23 native full-screen cycles with the approved image. Confirm that
   `native-restore-retrace-*` warns only during recovery, all writable
   readbacks return zero, repeated recovered returns cost zero strikes,
   and the panel/desktop actually presents. Host register equality does not
   prove visible output or establish the previously unnamed wait beyond
   the timing inference above.
2. Loader: independent recovery from a BIOS call that never returns needs
   a separately specified platform watchdog/firmware-execution strategy.
   Pre-kernel failures have no durable disk writer. The optional trail
   becomes a protected disk record only if the kernel starts. No unsafe
   FAT/raw-disk write path is added to a read-only loader for this audit.
3. Evidence sink failure and sustained all-protected queue saturation still
   have finite capacity. Retain the existing RAM/raw evidence and report
   drop/error counters; do not claim guaranteed disk persistence when that
   disk is faulty. Shared-controller/physical stuck-line failures necessarily
   affect every function physically dependent on that controller/line.
4. Recheck hardware latency and ordinary input/storage/audio under concurrent
   VM load on the canonical image. The five physical trials were reviewed
   read-only in the main checkout. f4-31/f3-44 integration and confirmation remain
   with their owners; no audio transport or VIF fix is claimed here.
