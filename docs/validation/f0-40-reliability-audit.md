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


## F0-41 research and implementation decision (2026-10-19)

Research completed before implementation. The directive names no URLs;
the relevant primary references from the latency/audio audits were checked:
[Linux v6.12 softirq.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/softirq.c)
(2 ms and finite restart bounds), and
[Linux locking rules](https://docs.kernel.org/locking/locktypes.html).
Decision: retain voluntary preemption, with checkpoints only after complete
page ownership/accounting updates, outside IRQ-off/spinlocked sections.
Shared VM copies must exclude the owning VM worker's emulation transactions
and the private disk gate. No DPMI dispatch changes.
[Linux intel8x0.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c)
and the [ALSA PCM interface](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)
separate period service from buffer capacity and recovery. Decision: fix
the producer's scheduling gap and replay the unchanged audio policy; do
not reduce its adaptive safety margin to conceal a stall.

The physical 110 ms stretch is an aggregate, not a per-page microbenchmark.
At the maximum 24 MiB pool, 6144 pages give 110000/6144 = 17.90 us/page
(about 19694 cycles at 1.1 GHz). Model allocate + zero + map at 18 us/page;
16 pages cost 288 us, leaving over 6x margin to 2 ms. Charge 0.2 us per
checkpoint (220 cycles) in host replay. These are explicitly model costs,
not a new hardware measurement. Preserve eager backing and admission.


### Bounded sites and host model

Model units below are CPU work, excluding time sleeping for disk/device
completion or execution of another task. Pool costs are 4 us allocation,
8 us zero and 6 us mapping per page; checkpoints add 0.2 us. Bulk memory
is charged at 8 us/4096 bytes; VGA model byte operations at a conservative
2 us/byte. These deliberately different bandwidths represent ordinary RAM
and emulated VGA operations, respectively. The before control disables
checkpoints while retaining the same work and mappings. The full lifecycle
also runs with real host address-space functions, page ownership, admission
and allocation-failure unwinding, rather than the replay's single scratch page.

| Site | Loop/work bound | Checkpoints | Worst span before → after (host model) |
| --- | --- | --- | --- |
| `dosvm_create` slot clear | One 4096-byte chunk | Existing, every page | 8 → 8 us per chunk |
| Low-memory backing | 272 pages; at most one additional page-table allocation at a boundary | Existing, after each completed page/map/ledger update | At most 28 → 28 us for one backing page plus table work |
| `dosvm_memory_init` eager pool | 6144 pages at maximum 24 MiB quota | Added after 16 pages and final partial batch | 110592 → 288.2 us; one extra table allocation adds model margin well below 2 ms |
| Creation device/BIOS/memory/ROM/disk stages | Fixed 6 audio pages, 256 IVT entries, 2000 text cells; ROM fonts already yield every 16 glyphs and BIOS initialization already has safe points | Added between completed stages, outside registration/allocator/VFS calls | Full-creation replay: maximum 117378.552 → 292.2 us; total 117378552 → 117567752 ns (+0.1612%); the before control disables all checkpoints |
| `dosvm_range` external validation | Maximum 6144 pool pages | Added every 16 validated pages | 6144 → 16 page validations (1 us/check envelope: 6144 → 16.2 us) |
| `transfer` ordinary RAM | One physical page per copy; external caller only | Added after completed page fragments | Maximum pool copy: 49152 → 8.2 us per copy slice, plus bounded range validation |
| `transfer` VGA model and physical aperture | At most 256 byte operations per slice; external caller only | Added after every 256 completed byte cycles | Model page: 8192 → 512.2 us; physical aperture uses the same slice bound |
| `dosvm_text` | 4000 VGA byte peeks plus cursor/mouse bookkeeping | Inherits 15 internal checkpoints from `transfer`; latches unchanged | 8000 → 512.2 us for the text read, plus final bounded bookkeeping |
| `dosvm_boot` | One 512-byte boot-sector read/copy, then fixed register/frame setup | After disk-gate release and after boot-sector copy, before monitor setup | RAM copy ≤1 us; no quota-sized loop remains |
| `dosvm_disk_read` | At most 1024 overlay rows, two cache windows, one read of at most 128 sectors (64 KiB) | Added only after dropping disk gate; excludes VM-worker INT 13h transactions | Local index/copy work <2 ms in the 1 us/row + 8 us/page envelope; storage waits sleep. Underlying FAT chain walks remain a separate caveat below |
| `storage_enable_write` (`mount.c`) | FAT census/check/reclaim already service every 128 clusters; directory scan services each iteration | Retained inner `fs_service`; added service after all outer VFS locks are released | Existing 128 → 128-entry batch; including the 1 ms service-clock gate, a 1 us/entry envelope gives ≤1128 us; no scheduling point inserted under a namespace/cache/spin lock |
| Failed-creation teardown dependency | Up to 6144 pool pages plus low pages in `core/vmm.c::as_destroy` | Existing cleanup has no internal checkpoint; `vmm.c` is outside write authority | Ownership/rollback tested; maximum cleanup span not qualified by this creation model |

`kernel_latency_test.c` replays the exact production pool loop with the
production scheduler and CPU sampler: the negative control records a
110 ms `dosbox` stretch, and the checkpointed replay records 288200 ns
(maximum before integer-ms formatting). Total modeled pool time is
110592000 → 110668800 ns (+0.0694%). The audio harness independently
regenerates that loop and advances DMA at 48 kHz; a 1 ms timer wake can
only run refill at the next checkpoint. The uncheckpointed control must
emit one `late-grow` and reach 28 periods. The repaired case must retain
8 periods with no `late-grow`, halt or deadline miss. The passing audio replay
records 110592 us / 28 periods / one growth without points, versus 1153 us /
8 periods / zero growth with points. No audio policy or
DPMI implementation changed.

### Limits and remaining qualification

The directive's aggregate T23 evidence does not measure each allocator,
VGA byte operation or FAT entry. The host cost model cannot certify a
physical 2 ms bound. Claude must run the canonical kernel/image and QEMU
regressions, then check the T23 `stretch_max_ms`, refill-gap and lead logs.
The known IRQ 11/SMI attribution is unchanged.

The disk's seek/read pair remains under its sleeping serialization gate;
adding a point there would break the directive's lock/transaction rule.
FAT scans already have service points, but `fat.c::chain_at` has no internal
service point and a sufficiently long cached outer-image chain could still
consume more than 2 ms. A fragmented or unusually large directory can reach
that helper during lookup/repair too. `fat.c` is outside this directive's
write authority. Also, `dosvm_vga_model` may perform the existing indivisible
harvest/exchange before a copy; `dosvm_vga.c` explicitly forbids yielding
inside that model transaction and is outside the permitted files. No
universal all-media/all-VGA-state 2 ms guarantee is claimed for those
pre-existing dependencies. These sources require a separate storage/VGA
review if physical evidence identifies a residual stretch. The new host
creation/attach replay itself must show no residual lead growth.

Failed-creation teardown additionally reaches `core/vmm.c::as_destroy`,
which walks/frees every mapped page without an internal checkpoint. That
file is outside this directive's allowed core changes (`task`/scheduler
helper only). Allocation-failure rollback correctness passes, but the timing
model stops at creation return, so it does not qualify a late-failure cleanup
stretch. This is another dependency for a bounded cleanup review.

### Verification completed in this worktree

`ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash scripts/test/host_kernel_tests.sh`
completed with exit 0, including DOS VM, storage/log recovery/power cuts,
audio, USB, networking, OPL reference and Win32 host cases. Standalone
`ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash tests/host/audio/run.sh` also
completed with exit 0. `nice -n 19 python3 scripts/build_kernel.py` built
`build/f0/VMM.ELF` (5775492 bytes) and passed its FPU/SIMD audit. No OS image
was built and no QEMU/hardware test ran. Full logs and the per-site report
are under `build/codex/f0-41-*` in this worktree.

## F0-44 bounded-work inventory and decision (2026-10-19)

Inventory recorded before implementation on `wt/f0-44`. Authority is
[f0-44](../directives/f0-44-bounded-teardown-allocation-chain-audit.md).
Research checked the primary documents behind R1/M06/S05 and f0-41/f1-82:
[Linux v6.12 softirq finite restart/time bounds](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/softirq.c),
[Linux locking rules](https://docs.kernel.org/locking/locktypes.html),
[Microsoft DDK deadlocks](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM),
[Microsoft FAT 1.03](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf),
[Linux dirty-page controls](https://www.kernel.org/doc/html/v6.12/admin-guide/sysctl/vm.html).
Decision: finite work batches, complete ownership updates before yielding,
no point inside guest/device publication, preserve FAT ordering and f1-82
writeback policy. `fs_service` explicitly permits cooperative service under
sleeping filesystem reservations; these are not spinlocks. Cache cursors
must survive concurrent LRU movement, invalidation and layout conversion.
Neither the DDK nor Linux supplies a Pentium III timing guarantee.

### Initial inventory (all size-dependent loops in authority)

Costs are deterministic T23-class envelopes, not new physical measurements:
18 us allocate/zero/map page; 8 us bulk 4 KiB; 6 us release/update page;
1 us cached FAT decode; 0.2 us checkpoint. Table/bitmap/descriptor checks
use 1 us unless a smaller transaction-specific envelope is stated. A
512 MiB volume with 4 KiB clusters has at most 131072 clusters; other
supported geometries can have more, and retain their geometry-derived bounds.
8 MiB is 2048 budget pages, not blocks: i686 has 14016 512-byte slots after
workspace/metadata/writeback overhead (upper bound 16384); supported 32 MiB
caches have at most 65536 slots. A process has at most 786432 VA pages
below C0000000; physical private backing is additionally RAM limited.

| File / functions (each size-dependent loop) | Original bound and cost / physical maximum |
| --- | --- |
| `core/vmm.c::vmm_init` | 255 kernel tables; 256 low pages; E820 descriptor count and up to 196608 direct-map pages; text/rodata pages. Boot-only, before runnable tasks; no runtime latency qualification. |
| `map_physical`, `vmm_revoke_mmio`, `replace_mmio` | MMIO region 120 MiB / 30720 pages; mapping publication must remain indivisible; 1 us/PTE provisional envelope, unqualified for maximum arbitrary mapping. |
| `kstack_alloc/free`, `as_create`, `as_replace_pages` | 1360 slots, eight stack pages, 255 PDE copies, at most 32 PTE validations/updates; fixed counts previously without model. |
| `as_destroy` | 768 PDEs x 1024 PTEs, frees all present pages; no points, 6 us/free plus 1 us/check. Failed creation and normal destruction share it. |
| `as_range_ok` | At most 786432 pages, no points, 1 us/PTE lookup. |
| `core/kheap.c::kheap_init/refill/kmalloc` | Seven classes; at most 128 free slots on one page; at most seven class tests. No reclaim walk exists; kfree is constant. Refill private construction then short IRQ publication. |
| `proc/uaddr.c::free_pages` | Up to 786432 pages plus 768 x 1024 empty-table checks, no points. |
| `reserve_pages`, `protect_pages`, `ua_map_shared` | Up to 786432 pages; reserve has 16-page points only on newly allocated pages; protection/shared setup have none. |
| `ua_find/vacant/ua_map_at/first_fit/pinned/prepare_split/split/remove_range/ua_munmap/ua_mprotect/ua_range/ua_pin/ua_unpin/ua_cancel_pins/ua_release_owner/w32_coalesce/ua_w32_change/ua_w32_release` | Extent/pin walks bounded by disjoint VA pages / active tasks, native mmap arena cap 256; Win32 extents can exceed 256. No internal service; metadata mutation/validation transactions require stable references. Nested ua_find walks amplify cost. |
| `transfer/zero_range` | Page fragments; transfer caller bytes up to VA span (public I/O normally 1 MiB); zero_range actually < one page in brk caller. No internal points. |
| `fs/fat.c::chain_at/release_chain/allocate/zero_chain/grow_directory/write_free/fat_write` | Geometry-bounded chain/census/allocation iterations; cached walks no internal points. Mutations retain barriers. |
| `fat_next/fat_lookup/slots/alias_exists/make_alias/fat_remove/fat_rename/fat_rename_replace` | Directory entries up to clusters*spc*16 (UINT32 capped), numeric aliases <=999999; nested chain_at. No internal cached-loop bound; string/LFN subloops <=260 characters/20 entries. |
| `fat_read/replace/write_data/fat_write_reserved` | Size/sector loops, coalesced run <=64 KiB, FAT cluster hops; no cached-work service bound. |
| `scan_release/scan_memory/scan_reset` | SCAN_PAGES budget and workspace <=256 pages; no points in page allocation/clear/release. |
| `scan_chain/scan_census/scan_check/scan_reclaim/scan_tree/recover_log_size/scan_volume/fat_prepare` | Existing 128-entry points in chain/census/repair, per-tree-entry/per-log-sector points; repeated geometry-derived ownership passes. Paired FAT reads multiply per-iteration cost. |
| `fs/cache.c::device/filling/join_fill/read_transport/wait_block/unhash/find` | 32 devices, active fills/tasks, hash chain <=slots; waits sleep. Hash/list operations require stable lock ownership. |
| `convert/convert_page/convert_half/half_activate/half_deactivate/get` | All data pages / LRU slots, four slots per half; service exists in conversion/get but cached candidate scans have no internal points. |
| `cache_init/destroy/cache_workspace_begin` | 2048 budget pages default, 8192 maximum; allocation/unwind/clear loops have no points. |
| `cache_read_run/cache_read_ahead` | <=128 sectors, nested LRU victim walks up to slots; reservation/publication transactions. |
| `write_batch/barrier/cache_writeback_tick/cache_invalidate` | Selection and dirty-age scans up to slots, sorting <=64 selected entries; 32 KiB copies/writes; transport unlocks, scan does not. Existing per-entry tick fs_service follows mutable LRU links. |
| `vm/dosvm.c::pool_pin/pool_unpin` | <=7 pin records, each ledger helper can inspect <=1024 PFNs; helper cost unqualified (ledger outside authority). |
| `dosvm_create/dosvm_range/transfer` | VM slot clear, 272 low pages (per-page points), 6144 pool pages (16-page points); external range 16 pages, RAM copies per-page, VGA per-256 bytes. Preserve f0-41. |
| `dosvm_destroy/dosvm_snapshot` | <=4 VMs; worker quiescence sleeps with 6s deadline; cleanup delegates to memory_close/as_destroy. |
| `vm/dosvm_xms.c::pool_find/dosvm_pool_query/dosvm_pool_alloc/free/resize/dosvm_copy_region/dosvm_memory_init/close` | <=6144 pool pages, 272 low mappings, 16 EMS aliases; allocation and resize zero whole pool without points. Copies already schedule after complete 256-byte exchange; guest state must stay indivisible. |
| `vm/dosvm_vga.c::video_linear_seed/video_exchange/video_harvest` | Exactly <=16 aperture pages (64 KiB), <=4x16 pending masks, <=32 PTEs; indivisible, no points allowed. |
| `dosvm_vga_present/rom_fonts/video_planar_changed` | <=480 rows, <=640 pixels/row, four planes; existing four-row/16-glyph points, bounded presenter transaction. Guest decode/string/burst loops have explicit instruction bounds; no points inside guest emulation. |

Other loops in dosvm.c are fixed device/port/trace/context arrays, or the
long-lived worker's bounded instruction/event iterations with sleeps and
existing transaction-boundary service. They do not scale with RAM/VM/file/
cache size. IRQ/timer budgets and DPMI dispatch remain outside this change.

### Implemented bounds and per-site evidence

All numbers below are microseconds of deterministic CPU work. “Construction”
means an arithmetic bound from the production loop and the stated envelope,
not a separate timed hardware result. The controls execute the same production
loops with modeled checkpoints disabled. They retain the f0-41 110 ms negative
control. Every paired replay asserts maximum <=2000 us and total work <=110%
of its control. Host ASan/UBSan replays exercise actual page ownership, failed
creation cleanup and FS reservations; synthetic PFNs avoid allocating 3 GiB
of host RAM for the architectural VA maximum.

| Site | Iteration / physical maximum | Service or transaction bound | Before -> after worst CPU span (us) |
| --- | --- | --- | --- |
| `vmm.c::as_destroy` | 768 PDEs, 786432 PTEs; physical pages limited by RAM; VM pool 6144 pages | Every 16 PDEs and every 16 PTEs, including holes; clear PTE/counters before service | Architectural synthetic: 5510406 ->119.2; 512 MiB page count with empty high tables: 1578246 ->119.2 |
| `vmm.c::map_physical` | At most 30719 pages plus guard in 120 MiB region | Reserve VA before preparation, publish pointer on return, every 128 PTEs | Construction <=30719 ->128.2; runtime replacement discussed below |
| `vmm.c::kstack_alloc/free/as_create/as_replace_pages/as_range_ok` | 1360 slots, eight stack pages; 255 PDE copies; <=32 exchanged PTEs | Indivisible small transactions: <=484 us stack allocation, <=72 us directory creation, <=64 us exchange, excluding allocator internals | Construction unchanged. Actual as_range_ok callers validate one VM page, <=240-byte legacy strings, or <=4096-byte probe buffer: at most two PTE checks (<2 us) |
| `kheap.c::refill/kmalloc/kfree` | 128 slots/page, seven classes; no reclaim walk exists | <=128 us private refill construction, constant IRQ publication/free; no point needed | Construction unchanged <=128 us, excluding allocator internals |
| `uaddr.c::free_pages` | Up to 786432 VA pages; <=768 empty-table tests | Every 16 VA pages, even holes and only-new skips; one service per PDE; 1024-check emptiness test remains atomic (256 us) | Synthetic maximum 5706240 ->262.2; 512 MiB page count 1774080 ->262.2 |
| `uaddr.c::reserve_pages/protect_pages` | Same VA maximum | Preserve 16-page allocation points; also checkpoint existing-backing skips; protection every 16 pages | Existing-backing maximum 786432 ->16.2; protection 786432 ->16.2 for check-only replay, conservative update envelope <=96.2 |
| `uaddr.c::ua_find/vacant/first_fit/pinned/prepare_split/ua_map_at/remove_range/ua_munmap/ua_mprotect/ua_range/ua_release_owner/w32_coalesce/ua_w32_change/ua_w32_release` | Native mmap arena <=256 extents; architectural disjoint-page maximum 786432 records; Win32 has no 256-record cap | Shared 128-visit counter also bounds nested lookups; stack mutation pins retain all nodes and split-count assumptions over service. Overlapping insertion/mutation returns EBUSY while this metadata reservation exists | Actual maximum lookup/vacancy replays 786432 ->128.2; callers inherit the same visit bound plus bounded page work |
| `uaddr.c::ua_pin/ua_unpin/ua_cancel_pins` | Active stack-owned pins (16 native threads/process plus kernel callers/nested transactions) | No service while following another caller's stack links; see qualification limit below | Construction only; not a measured maximum kernel-caller pin fan-out |
| `uaddr.c::transfer/zero_range/ua_map_shared` | Transfer/aliases <=VA span; brk zero tail <4096 bytes | Transfer every 16 page fragments with mutation reservation; shared mapping every 16 pages while preparing; zero tail indivisible <=8 us | Construction: bulk transfer <=128.2, shared table/backing work <=288.2; page ownership and rollback checked by process/ledger suites |
| `dosvm.c::dosvm_destroy` / `dosvm_xms.c::dosvm_memory_close` | One VM; 272 low pages +6144 pool pages +tables; <=4 VMs globally | Low-frame restoration every 16 pages, then bounded as_destroy; repeat destroy returns EBUSY while cleanup is yielded | Full normal-stop maximum 48151.514 ->132.839; no ownership/admission leaks |
| `dosvm_create` late failure | Full maximum pool already allocated; first disk-open operation fails | Same real cleanup and idempotent generation guard; creation points preserved | Full create+failed unwind 165530.066 ->292.2; work 165530066 ->165821666 ns (+0.177%) |
| `dosvm_xms.c::dosvm_pool_alloc/resize/free` | <=6144 pages | Zero/occupancy/accounting complete before each 16-page allocation/growth point; free every 128 bitmap updates, accounting published before point | Construction allocation/growth <=128 us zero per batch plus lookup/accounting; bitmap release <=128.2 us; no quota change |
| `dosvm_xms.c::pool_find/dosvm_pool_query/resize` adjacency | <=6144 byte entries | Read-only bitmap transaction, 0.1 us/simple byte check =>614.4 us; no yield into guest state | Construction unchanged <=614.4; not a calibrated physical byte-check cost |
| `dosvm_xms.c::dosvm_copy_region` | At most quota bytes, 256-byte slices | Retain existing reschedule after a completed copy/exchange slice; XMS/EMS endpoints are validated RAM | Existing slice construction: <=2 us bulk copy work; guest validation/dispatch remains separately owned |
| `dosvm_vga.c::video_harvest/video_linear_seed/video_exchange` | Exactly <=65536 aperture bytes, 16 PTEs, <=4x16 pending masks | No internal point. Scalar conversion charged at 8 ns/byte (rounded upward from 4x bulk-copy cost), plus mask/PTE work | Combined actual transaction + fixed-size charges: 1248.576 ->1248.576; no point, latch change or extra backing |
| `fat.c::chain_at` / common `get_copy` | 131072 randomized clusters on the specified 512 MiB/4 KiB-cluster profile | Decoder service every 128 entries; read/write-sector paths every four calls. All original link validity checks/barriers retained | All links resident: 131071 ->4.2, no post-warm transport reads; work +5.16% |
| `fat.c::scan_census/scan_chain/scan_check/scan_reclaim/scan_tree/recover_log_size` | Geometry-bounded census, paired copies, tree passes and log sectors | Common decoder/sector points plus existing 128-entry/tree-entry/log-sector points | Two-copy census 262148 ->4.2; ownership chain 393216 ->4.2; work +5.24% /+5.21%. Resident mount-window decoder envelope <=128.2 |
| `fat.c::release_chain/allocate/zero_chain/grow_directory/write_free/fat_write/fat_read/replace/write_data/fat_write_reserved/fat_next/fat_lookup/slots/alias_exists/make_alias/fat_remove/fat_rename/fat_rename_replace` | Geometry/size/directory bounded; alias tails <=999999; LFN <=260 chars | Inherit common decoder and four-sector points, <=512 us bulk per four 64 KiB cached runs; no FAT format, copy order, publication or durability barrier changed | Construction from common points; existing fsck/mtools, cut/error, mount-budget and concurrency suites verify ordering/correctness, not new physical transport deadlines |
| `fat.c::scan_memory/scan_reset/scan_release` | <=256 scan pages or cache workspace pages | Per allocated/cleared/freed page, after complete ownership update | Construction <=18.2 /8.2 /6.2 |
| `cache.c::cache_init/cache_destroy/cache_workspace_begin` | 2048 budget pages default; 8192 maximum | Allocation every 16 data pages (with descriptor-page margin), per workspace allocation/free/clear; no runtime resizing policy | Actual 8 MiB init 38404.765 ->358.2; destroy 12251.765 ->17.765; workspace 1024 ->8.2; total overhead <=2.94% |
| `cache.c::write_batch/cache_writeback_tick` | Default i686 14016 512-byte slots; host 13384; <=64 selected blocks /32 KiB | Short <=32-visit newest-first fast path preserved; long stable descriptor walk every 16 visits; selected slots pinned before service and released on all errors; dirty-age scan uses stable cursor | All-dirty 8 MiB: 227121 ->272.2; work 240506000 ->240840600 ns (+0.139%). LRU touch/invalidation during selection leaves no busy pins |
| `cache.c::find/unhash/get/cache_read_run/filling/read_transport/convert/cache_invalidate` | <=all cache slots or active fills; read runs <=128 sectors | Every 16 hash/victim/fill visits; read-run hit/reservation bookkeeping every 16 sectors; conversion per page. Sleeping cache ownership remains held across service. Epoch publication follows preparation | Maximum hash lookup/unlink/victim replay: 13384 /13383 /13384 ->16.2; work +1.25%. Nested short walks have <=624 us conservative slices before the 1 ms service gate |
| `cache.c::device/half_activate/half_deactivate/convert_half/write_batch` inner sort/copy/completion / `barrier` | 32 devices, four half-slots, 64 sorted blocks, 32 KiB snapshot; waits sleep | Fixed transactions: <=8 us device lookup at 0.25 us/check, <=64 us bulk snapshot; selection comparison+move total <=64/visit; barrier batches transport/wait between iterations | Construction included in <=300 us selection slices; transport and sleeping excluded. No dirty thresholds, writeback age, batch byte limit or media/error policy changed |
| `task.c::sched_tick` test only | 256 native threads, stronger 1360-stack-slot ceiling +idle | Production scheduler unchanged; count actual scan visits and assert <=2000 us at 1 us/visit; next-point wake/dispatch tested | 1361 ->1361 model us; IRQ-off <=100 us target remains f0-46, not passed here |

`fs_service` retains its existing one-millisecond clock gate. A modeled call
boundary is not a claim that every invocation dispatches another task: add
up to one millisecond before the next eligible checkpoint. The largest new
FS selection slice is <300 us (nested short cache walks <=624 us), leaving
margin below two milliseconds under the stated model. Sleeping reservations
still serialize competing operations; this patch does not qualify a reader
latency deadline or replace the f1-82 writeback policy.

Metadata pins now cover a complete page/protection/publication or list-walk
phase. Native/Win32 callers can observe transient EBUSY for a mutation or
insertion conflicting with that reservation. This is required to keep extent
nodes, page tables and split accounting stable at the added checkpoints;
readable mappings retain their rights and public structures/ABI are unchanged.
Host coverage includes rejected concurrent insertion, concurrent preparing-map
access/protection, overlap pin refusals, shared mappings, rollback and exit.

### Explicit remaining qualification / ownership boundary

- T23 physical calibration and end-to-end audio/reader evidence are pending;
  no QEMU, hardware run or OS image build is claimed. Host costs are conservative
  policy envelopes inherited from f0-41 plus explicitly stated new envelopes,
  not measurements of every allocator, cache miss, firmware or chipset state.
- `vmm_revoke_mmio` / `replace_mmio` keep the old aperture-replacement
  transaction indivisible. Their 120 MiB region ceiling alone allows 30720
  PTEs: at 1 us/PTE a maximum arbitrary revoke/replace exceeds two milliseconds.
  Only private new-map preparation is sliced here. Maximum runtime remapping
  needs a separately reviewed device-publication protocol and physical record;
  a blanket all-MMIO R1 closure is **not** claimed.
- `pmm_alloc*` fragmentation scans and `ledger.c::mm_pin/mm_unpin` PFN walks
  are outside f0-44 authority. Seven VM pin records can each inspect 1024 PFNs.
  The modeled 18 us allocation cost and six-us release cost do not prove their
  maximum internal runtime. No memory cap, direct map, pool quota or admission
  headroom was changed to hide this dependency.
- Stack-owned pin lists are deliberately not yielded while their owners could
  return. Native thread fan-out is bounded by the process contract; the maximum
  kernel-caller/nested-pin fan-out still needs a dedicated ledger audit. Shared
  object callbacks and `ww_cancel` similarly retain their owners' contracts.
- `copy_user` assembly used by `copy_from_user`/`copy_to_user` is outside this
  authority; only the supervisor direct-map transfer loop is sliced. Guest
  instruction/REP/DPMI dispatch, VGA bursts and device/firmware callbacks keep
  their existing transaction limits and remain with their owners.
- Boot-only `vmm_init` precedes runnable service; IRQ/timer/scheduler work and
  <=100 us IRQ-off qualification remain f0-45/f0-46. The 1361 us task-scan model
  explicitly does not pass the IRQ-off gate. No task.c or sync.c change was made.
- Only the specified 131072-cluster and 8 MiB cache maxima get new worst-case
  host data here. Larger supported FAT geometries and the 32 MiB cache ceiling
  inherit iteration batches, but require their own transport/CPU calibration.
  No universal Windows 98/ME policy equivalence or whole-kernel R1 closure is
  inferred from the passing host cases.

### F0-44 verification completed

All four required commands completed with exit 0, sequentially under
`nice -n 19`: kernel host suite and filesystem host suite with
`ASAN_OPTIONS=detect_leaks=0`, standalone `tests/host/audio/run.sh`, and
`python3 scripts/build_kernel.py`. The FS suite passed all groups, three
fsck checks and three independent mtools comparisons; its 1681 crash cuts
had zero unknown fsck diagnostics. The audio lifecycle replay retained eight
lead periods without growth; its negative control retained 28. The optional
DOSBOOT payload runner reported the existing missing-header/payload SKIP;
that optional payload case is not claimed as passing.

`build/f0/VMM.ELF` is 5915356 bytes, build `11229c27b559-dirty`, with the
FPU/SIMD audit passing and SHA-256
`39a391de096fced4848ce2601cd9be47546567fc6d1ac44fa216f25a485f4134`.
This is an ELF hash, not an image qualification. Full logs and the report
are in `build/codex/f0-44-*`. No public-header/ABI change occurred, so the
conditional API-reference/apps rebuild was not triggered. No QEMU, systemd
scope, OS image build, commit/push or diary edit was performed.
