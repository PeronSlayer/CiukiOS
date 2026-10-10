# F1 acceptance: drivers and disk

Author: Codex (teammate). Reviewer and integrator: Claude (lead).
Status: reviewed and integrated by the lead on 2026-10-10; no implementation qualification claimed.

## Decision

F1 is accepted when registry-driven activation, keyboard/mouse input, the boot
framebuffer, ATA PIO, MBR partitions, FAT12/16/32 with LFN, safe mode and disk
logging have reproducible evidence on the canonical image. QEMU qualification
and physical T23/E500 qualification are separate gates. F0's QEMU success does
not waive its outstanding hardware gate. All eleven F0 probes remain regressions,
including the ten-boot matrix, isolation, scheduling, lifecycle, FPU and panic.

**[F1]** Requirements below extend [F0 acceptance](f0-acceptance.md) and
[test architecture](test-architecture.md). MUST is mandatory; SHOULD is a
recommendation. Read qualification MUST precede write enablement. Missing
required evidence is `not_run`, never PASS. **[F2+]** Application loading,
public file/input syscalls, the desktop compositor, DOS personalities, DMA,
ATAPI, acceleration and runtime video-mode changes retain their existing phases.

## Sources

Research and repository comparison: 2026-10-09. Primary documents establish
interfaces; thresholds and probe workloads below are CiukiOS acceptance decisions.

- T13, [ATA/ATAPI-6 draft 1410D revision 3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
  sections 7–9, especially 8.12 and 8.15: task file, PIO, IDENTIFY and flush.
- Intel, [keyboard-controller documentation](https://intel.github.io/ecfw-zephyr/reference/kbchost/index.html),
  and upstream [SeaBIOS 1.16.3 PS/2 implementation](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/hw/ps2port.c):
  controller/device transactions, AUX routing and bounded reply handling.
- VESA, [VBE Core Functions 3.0](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf),
  functions 00h–03h: copied mode information, linear pitch, masks and framebuffer.
- Microsoft, [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
  and [fatgen103](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf):
  cluster classification, reserved entries, FSInfo and LFN validation.
- [dosfstools upstream](https://github.com/dosfstools/dosfstools): independent
  `mkfs.fat`/`fsck.fat` fixtures and validation.
- Linux v6.12, [MBR/extended partition parser](https://github.com/torvalds/linux/blob/v6.12/block/partitions/msdos.c):
  relative EBR addressing and bounded traversal reference.
- QEMU [input QAPI](https://github.com/qemu/qemu/blob/v9.2.0/qapi/ui.json),
  [fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html),
  [blkdebug](https://www.qemu.org/docs/master/devel/testing/blkdebug.html) and
  [icount](https://www.qemu.org/docs/master/devel/tcg-icount.html): stimulus,
  selection, I/O fault injection and synthetic timing.
- [IBM T20/T21/T22/T23 maintenance manual](https://download.lenovo.com/pccbbs/mobiles_pdf/62p9631.pdf),
  printed pages 63 and 141; [Compaq E500/V300 service guide](https://h10032.www1.hp.com/ctg/Manual/c01129749.pdf),
  hard-drive servicing, rear components and Appendix A: physical handling and
  serial connection, not proof of installed controllers or firmware behavior.

Repository comparison used Semble, then bounded reads; Semble cannot index
`src/boot` assembly, so exact labels were inspected there. Reuse decisions:

- [`session_disk_ata.c`](../../src/vm/session_disk_ata.c), `issue_identify`,
  `poll_drq`, `transaction_finish`, and
  [`session_disk_ata.h`](../../src/vm/session_disk_ata.h), `cvata_transfer`:
  retain command-state and no-BIOS-retry principles; replace VM/PIC coupling
  with kernel services. Existing 30-second polling does not qualify FLUSH CACHE.
- [`lfn.c`](../../src/lfn/lfn.c), `lfn_sum`/`dir_next`, provides naming logic;
  its FAT16-sized cluster fields, private directory cache and conversion behavior
  require replacement for the unified FAT/VFS contract.
- [`input_platform.inc`](../../src/boot/input_platform.inc),
  `input_platform_firmware_first`, preserves the exact E500 PCI match.
- [`registry.c`](../../src/kernel/core/registry.c), `registry_release`, currently
  marks release without quiescence; `registry_init` reserves the framebuffer.
  [`console.c`](../../src/kernel/core/console.c) already consumes boot LFB data.
  These are starting points, not F1 lifecycle/presenter evidence.
- [`syscall.c`](../../src/kernel/core/syscall.c) implements F0 calls 0–5;
  [`probes.c`](../../src/kernel/probes/probes.c) selects F0 probes; since
  commit `2003adb` the selector accepts the fw_cfg-only `safe=1` key and the
  `core` alias, and the loader validates probe names from a fixed list, so
  the `f1:` prefix and the F1 probe names below need a loader and kernel
  extension. F1 input injection requires runner additions.

## Normative scope and probe selection

### Activation, input and display

**[F1]** Drivers MUST acquire transactional registry claims before hardware
access: `discovered/firmware-reserved → claimed → active → quiescing → released`.
Activation MUST establish resource sizes, routing and parent ownership; firmware
reservations require explicit lease transfer. Conflicts MUST refuse without
register writes and identify both owners. Quiescence MUST reject submissions,
stop sources, drain callbacks, prove idle, revoke mappings and then release
buffers/claims. Stale generations MUST fail. Uncertain shutdown MUST quarantine
and retain resources. Unknown devices remain disabled. Shared INTx and conditional
DMA requirements in [device ownership](device-firmware-ownership.md) still apply;
F1 PIO MUST NOT enable DMA merely to satisfy an enumeration test.

**[F1]** Exactly one input backend MUST own ports `60h/64h`, IRQ1/12 and
controller consumption. Native i8042 MUST serialize commands, separate AUX,
ACK/RESEND and unsolicited bytes, validate packet framing, and use finite
elapsed-time waits plus a stalled-clock escape. Initial native limits are
200 ms per ordinary reply, 500 ms total setup without reset, and at most two
RESEND retries within the original deadline. Reset-dependent hardware needs
separate qualification. Timeout disables/quarantines the backend without blind
reset or competing takeover. IRQ work captures bytes; deferred work SHOULD
yield within 1 ms. The inherited 250 µs hard-IRQ/critical-section bound applies.

**[F1]** E500 matching ATI `1002:4C4D` plus ESS `125D:1978`, subsystem
`0E11:B112`, MUST select firmware-first before native controller reads/writes.
Validated QEMU `platform=e500` tests the policy only. A persistent firmware lease,
serialized V86 BIOS service, private IVT/BDA/stack and mediated firmware IRQs
MUST preserve host PIC/PIT ownership and preemption. Only qualified nonblocking
INT16/INT15-C2 services and required handlers are allowed; dependencies remain
reserved. Firmware deadlines remain 100 ms for input/status and 500 ms setup.
As illustrated by [SeaBIOS keyboard handling](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c),
INT16 characters alone cannot establish key-release evidence: the qualified
firmware event adapter MUST expose actual transitions without a second reader.
Mechanism (lead decision 2026-10-11, directive f1-07b): the V86 monitor
already emulates every trapped `IN` from ports `60h`/`64h` that the firmware
ISR executes; it records the returned bytes (with the AUX status bit) into
the raw set-1 decoder as a side effect of the single firmware read. The
firmware remains the only controller reader; INT16 supplies text, the
observed bytes supply make/break transitions.

**[F1]** Both backends MUST deliver ordered key transitions, text, relative
pointer motion and button transitions to one bounded kernel input queue, with
source/generation, sequence and timestamp. Overflow MUST count loss and mark
state resynchronization; normal workloads permit none. Command replies MUST
never appear as input. Queue consumers MUST NOT touch controller ports.

**[F1]** The VBE LFB MUST become an active registered device by transferring
the boot reservation, retaining validated pitch/masks/extents and supervisor-only
mapping. A kernel damage-rectangle presenter MUST clip safely, reject arithmetic
overflow and serialize console drawing. No F1 mode changes or GPU acceleration
are allowed. Missing LFB retains text/serial fallback. Geometry and selection
remain governed by [boot-memory](boot-memory.md), including safe-mode preference
for 640×480; F1 does not require the F2 compositor.

### Disk, cache and filesystem

**[F1]** Native ATA MUST qualify IDENTIFY, 512-byte sectors and 28-bit LBA
READ/WRITE SECTORS, validating capacity and overflow before issuance. Unsupported
ATAPI/addressing/sector formats MUST be disabled explicitly. Polling MUST observe
BSY before DRQ/error interpretation, preserve device-control state and acknowledge
completion correctly. Initial deadlines are 30 seconds for IDENTIFY/data commands
and 60 seconds for FLUSH CACHE, outside interrupt-disabled sections. T13 explicitly
allows flush to exceed 30 seconds; these limits are qualification policy, not
universal device guarantees. Timeout/ERR/DF after any issued command, including
IDENTIFY/flush, MUST return EIO, quarantine the controller and prohibit further
commands or BIOS fallback until separately qualified recovery or reboot.
Boot-disk binding MUST compare native identity and sector fingerprints with
loader evidence before mounting `C:`; BIOS drive number alone is insufficient.

Amendment (lead decision 2026-10-11, directive f1-12): `ciuki_boot_info` v1
carries no loader sector fingerprints, so for F1 the boot volume is bound to
ATA disk 0, primary partition 1, and the mount record states
`qualified=0 reason=loader_fingerprints_absent`. The fingerprint comparison
above becomes mandatory with boot-info v2; until then this binding is the
accepted F1 behaviour on QEMU and on the two laptops.

**[F1]** MBR parsing MUST validate `55AA`, four primary entries, extended/EBR
links, bounds, overlaps and arithmetic against identified capacity and LBA28.
Logical-partition starts are relative to their EBR; chain links use the extended
base. Walks MUST detect cycles and have a declared finite entry limit. Extended
containers may contain their logical partitions; unrelated overlaps fail.
Unsupported GPT/protective entries MUST NOT become FAT volumes.

**[F1]** The block device → partition → single cache → FAT driver → VFS
layers MUST implement [storage semantics](vfs-storage-contract.md). Read-only
qualification MUST establish FAT32/LFN checksums and enumeration before write
enablement. FAT12/16 remain required, including packed FAT12 entries crossing
sectors and fixed roots. Paths use case-preserving, case-insensitive UTF-8,
UCS-2 LFN and CP437 short names; unmappable names fail explicitly. Limits,
64-bit positions, coherent open nodes/descriptions, sharing and duplicate-handle
positions MUST follow that contract. DOS bridge/byte-lock qualification retains
its later phase.

**[F1]** Mount validation MUST check BPB/signatures, geometry, FAT capacity,
cluster-derived type, root, FSInfo signatures and reserved FAT entries. Invalid
geometry refuses mount. Per the owner's decision of 2026-10-11 (f1-26), a
dirty-only FAT16/32 volume MUST recover automatically after the bounded mount
scan reports `lost=0`, no corruption and no FAT-copy divergence, subject to the
usual read gate and qualified durability. Recovery sets `ClnShutBitMask` in
each FAT copy, with a barrier after each copy, before enabling a writable
session; activation records `reason=dirty_recovered` and mount evidence
`recovered=1`. `HrdErrBitMask=0`, lost clusters, detected chain corruption or
diverging mirrored FATs retain read-only handling; repair remains host-side.
The hardware-error flag MUST NOT be cleared automatically. FSInfo counts are
hints. Chain walks MUST terminate within cluster
count. Read failures return EIO; metadata failures revoke writing immediately.
FAT16/32 writable sessions MUST clear their type-specific clean flag before use
and restore it only after durable unmount; the pre-session recovery above is
the explicit exception. FAT12 has no equivalent flag.

**[F1]** The shared cache defaults to 8 MiB. A device-priority writer MUST
write dirty blocks within five seconds and service explicit flush/unmount/
shutdown barriers. Barriers MUST complete earlier writes and qualified E7h
FLUSH CACHE, or establish verified disabled device write caching. Unknown
durability means read-only; successful PIO transfer alone is insufficient.
Delayed errors MUST remain observable at flush/commit/unmount. Allocation orders
FAT copies, data, directory publication, then FSInfo, with barriers; deletion
removes the directory entry before freeing its chain. Cross-directory rename
durably removes the source before publishing destination. Crash cuts may lose
chains, never cross-link them under atomic sector writes. Torn sectors and
interrupted FAT-copy synchronization require separate detection/read-only tests.

### Safe mode and logging

**[F1]** Safe mode MUST be selectable in the loader menu, by `safe=1` in
`\SYSTEM\BOOT.CFG`, and on QEMU by the selector key `safe=1` already
defined in [F0 acceptance](f0-acceptance.md). Any positive
source enables safe mode; normal/absent options cannot override it. Malformed
options MUST report selection errors. The loader MUST set boot-info flag bit 0
before device activation. Safe mode permits only qualified console, timer,
essential platform services and required input/storage; it disables optional
audio, acceleration, network, DMA, power takeover and optional firmware calls.
Required E500 firmware input remains permitted only under its qualified lease.
Unsafe storage remains read-only or unavailable; safe mode never authorizes
BIOS recovery from native ATA errors. Every disabled device needs a reason.

**[F1]** Boot records MUST accumulate in bounded RAM until storage passes
identity, mount, read, durability and write gates. Only then may the normal
logger write `\SYSTEM\LOGS\BOOT.LOG`, at most 128 KiB, through VFS/cache.
No-storage/read-only cases report `disk_log=unavailable`; logging failure MUST
not recurse. Flush obeys normal barriers. Panic MUST neither allocate nor flush
disk logs; serial/screen remains authoritative. Read-only probes suppress the
disk sink and verify zero writes.

## Acceptance tests

**[F1]** `K` means a new embedded kernel probe payload; `U` adds an embedded
ring-3 survivor payload using existing F0 calls. No mandatory F1 probe requires
a new syscall: VFS/input/presenter tests invoke production kernel interfaces.
Public application syscalls remain F2. Every row requires measured DATA and one
END; host predicates/checkers determine aggregate acceptance.

| Probe / tier / payload | Action and exact pass condition | Evidence |
| --- | --- | --- |
| `registry` / T0,T3 / K | 100 activation/quiesce/release cycles restore live claims, mappings and buffers; conflicting/unknown-size/stale requests cause zero device writes. Failed idle proof retains quarantine; second claim fails. Shared-IRQ fixtures service both owners, safely remove one and quarantine a stuck source. | State/generation trace, both conflict owners, resource ledger, write/IRQ/callback counters. |
| `input` / T3,T4 / K | After READY, inject 100 `a` make/break cycles interleaved with 100 relative mouse moves and ten left-button cycles. QEMU moves use x=2,y=-1, giving totals x=200,y=-100 in screen coordinates. Expect 100 text characters, 200 key transitions, 20 button transitions, zero loss/duplicates/stuck state. Repeat on native and firmware-first profiles. | Queue digest/counts, signed motion totals, backend/lease, QMP transcript hash or operator sequence. |
| `input-fault` / T0,T3 / K,U | Inject missing ACK, bounded RESEND, mixed AUX/key bytes, malformed packet and queue overflow. Each reaches its specified error/resynchronization, no blind reset or dual consumer; survivor advances 100 ticks afterward. Firmware overrun/disallowed I/O cannot mutate physical PIC/PIT. | Deadlines, errors, overflow count, controller reads by owner, quarantine and survivor progress. |
| `framebuffer` / T0,T3,T4 / K | Present overlapping, clipped, empty and edge rectangles with 24/32-bit/padded-pitch fixtures. Pixels equal a reference renderer; surrounding canaries/padding unchanged; overflowing requests rejected. Live pattern is visible and mode identity unchanged; no-LFB boot succeeds. | Mode/pitch/masks, pixel digest, guard errors=0, mode-call count=0, screen observation. |
| `ata` / T0,T3,T4 / K | IDENTIFY capacity agrees with profile; known sector/file reads match fixture hashes. Boundary/overflow requests issue no command. FAT workload below proves writes. | Identity/capacity, LBA/count, expected/actual digest, command trace. |
| `ata-fault` / T0,T3 / K,U | Inject ERR, DF, stuck BSY/DRQ, missing device and failed IDENTIFY/flush against a scripted fake device behind the driver's register boundary, re-armed between subcases in one boot (lead decision 2026-10-10, directive f1-06); one further case injects a real read error through blkdebug on the real device. Issued failures yield EIO by deadline; the next request issues zero commands; the real device sees zero commands during the fake subcases; BIOS calls=0; survivor advances 100 ticks. | Status/error/issued flag, elapsed/deadline, retained claims, subsequent-command count, real-command count. |
| `partition` / T0,T3 / K | Valid primary/extended fixtures enumerate exact starts/lengths; malformed signature, loop, overflow, out-of-range and illegal overlap reject without out-of-device I/O. | Fixture hash, bounded walk count, partition list or rejection reason. |
| `fat-read` / T0,T3,T4 / K | FAT12/16/32 names, aliases, sizes and hashes match independent fixtures; orphan/bad-checksum LFNs use valid short names. Boundary chains and invalid names terminate correctly. Zero writes before read gate. | Fixture/list digest, file hashes, mount mode, write count=0. |
| `fat-write` / T0,T3,T4 / K | Create/read/overwrite/truncate/rename/delete files of 0 bytes, one cluster and 4 MiB; grow directories beyond one cluster; collide LFN aliases. Reopen and cold-reboot reads match; clean run has `fsck.fat -n` exit 0 and matching mtools names/sizes. | Workload/seed, operation results, hashes, host checker output. |
| `cache` / T0,T3 / K | Trace exact durability order, five-second writeback, eviction and shared-handle coherence. Unsupported flush without verified cache disablement keeps volume read-only; injected delayed/flush error reaches caller and prevents successful unmount/clean marking. | Barrier IDs, persisted-write trace, dirty ages, handle positions, errors. |
| `mount-crash` / T0,T3 / K | Bad BPB refuses; dirty-only volumes with a clean bounded scan recover after the read gate (`recovered=1`, `reason=dirty_recovered`, mode `rw`); hardware-error flags, lost clusters, mirrored-copy divergence and detected corruption force read-only and reject writes. Crash cuts produce no cross-links; checker reports only declared interrupted-state outcomes, with the dirty diagnostic absent after recovered boot 2 and durable unmount (owner decision 2026-10-11, f1-26). | Cut index, durable-sector set, scan/checker results, recovery fields and write-refusal counters. |
| `safe` / T3,T4 / K | Each entry source sets flag before activation; required input/console works, optional activation count=0. Repeat without writable storage and without LFB; no hang or fallback ATA retry. | Option provenance, disabled-device reasons, active-owner list, READY. |
| `bootlog` / T3,T4 / K | Before qualification log writes=0; afterward bounded log reopens with matching records after durable shutdown. Read-only/failure path reports unavailable; F0 panic adds zero storage calls. | Qualification sequence, first-log-write sequence, size/hash, flush result and panic counters. |

**[F1/T0]** Host tests MUST compile the production FAT/VFS/cache core against
`mkfs.fat -F 12/16/32` images, recording tool versions, geometry and seed.
Crash injection MUST cut after every sector write and barrier, discard volatile
cache, then remount independently. Model device-cache reordering between barriers;
test torn sectors separately. Cross-link freedom needs an independent ownership
scan, not only the driver's assertion. Include ENOSPC, failed writes and
interrupted rename/FAT-copy updates. Clean cases require checker success;
interrupted cases MUST classify every discrepancy without automatic repair.
Fixtures and crash artifacts MUST stay on disk within the runner budget, never
in `/tmp`; host fixtures are test data, not alternate boot-image builds.

## Evidence protocol additions

**[F1]** Keep `CIUKI_TEST v=1`, 240-byte records, unique keys, monotonic sequence,
matching serial/screen records and F0 result metadata. Selection adds:

```text
f1:<probe-id> run=<8-hex-digit-id> [platform=e500] [safe=1]
CIUKI_TEST v=1 run=12ab34cd seq=000002 probe=input event=READY backend=native
CIUKI_TEST v=1 run=12ab34cd seq=000003 probe=ata-fault event=DATA issued=1 bios_calls=0
```

**[F1]** Requests remain at most 64 ASCII bytes through
`opt/it.alcybercloud.ciukios/test`. Optional keys are unique and ordered as above;
`platform=e500` remains validated-QEMU-only. Physical selection uses the menu,
bounded serial selector or BOOT.CFG probe line; safe mode there comes from
menu/BOOT.CFG. Preserve the existing ordinary F0 grammar; F1-28 adds the
cfg-only sweep/cursor grammar. A runner `all` alias MUST expand into ordered,
individual selectors across boots. The kernel ALSO accepts `f1:all` and
`f1:core` (lead decision 2026-10-10, directive f1-03): they run every
installed F1 probe in contract order within the F1 phase only, for
photographed hardware runs; QEMU suites never use them. The guest receives
no fixture data: `ciuki_boot_info` v1 carries only the selector, so probes
report what they read (names, sizes, SHA-256 digests) and the runner compares
those records with digests it computes from the image or overlay. Unknown,
duplicate, oversized or malformed selectors MUST run no probe and report error;
absent selection MUST perform ordinary boot without destructive tests.

DATA MUST identify subcase, owner/generation, errors, gate transitions and timing
domain. READY/ARM synchronize external actions; QMP success alone never proves
guest receipt. `result.json` MUST add stimulus/fixture hashes, fault/cut point,
disk cache mode, checker versions/results and durability observations. Missing,
duplicate or contradictory evidence fails. No new kernel PASS follows panic.

## Runner and suite additions

**[F1]** Add versioned `tests/suites/f1-input.json` (`registry`, input faults,
input, framebuffer), `f1-storage.json` (`ata`, partition, ATA faults),
`f1-fat32.json` (read, cache, write, mount/crash, bootlog; FAT12/16 subcases),
and `f1-safe.json` (all safe-entry sources and fallbacks). New runner actions
MUST wait for READY/ARM, issue QMP `input-send-event` key `qcode`, relative
`rel` x/y and `btn` events, then check queue evidence. Pace make/break below
typematic delay and drain between batches; aggregate motion tolerates packet
coalescing. Use no USB tablet substitution. Runtime injection hooks MUST be
supervisor-only, inactive without valid selection, and exercise production paths.

**[F1]** Run T0, T1, F0 smoke/regressions, then T3 with read prerequisites
before writes. Use pinned `qemu-t23`, `qemu-e500`, `qemu-min128`, pentium3/TCG,
PIIX/i8042/std VGA and `-icount shift=1,sleep=on`. Record synthetic guest timing
separately from host monotonic elapsed time; KVM smoke is not gate evidence.
Initial per-boot deadlines: input 120 seconds, storage 180, FAT 300, safe 90.
The three safe-mode sources are covered as follows: `safe=1` through fw_cfg
(T3), `BOOT.CFG` through an overlay patch with its manifest (T3), and the
loader menu on the physical machines (T4); QMP key injection into the menu
window MAY be added later but is not required.

The `bootlog-read-only` failure-path case uses the existing fw_cfg selector
`f1:bootlog run=<id> safe=1`; QEMU's [fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html)
defines the `-fw_cfg name=...,string=...` transport. In production `drivers_init`
skips ATA discovery in safe mode, while `storage_init` still sets up storage;
there is no C: volume, so `probe_bootlog` takes its `!v` branch and reports
`result=-30 write_count=0 storage_calls=0 disk_log=unavailable`. The VFS metadata
has `generation=0` because C: was never attached. The storage host harness
checks that path through production activation, mount setup, and probe code,
with an ordinary read-only but durable mount as a negative control. No image
mutation or block-property inference is needed: `FAT_RO_REQUEST` alone leaves
the read gate open, and `blkdev_durable` tests write/cache/flush capabilities.

**[F1]** Tests MUST never rebuild or copy the canonical image. Every boot uses
a qcow2 overlay under `build/test-runs/`; a declared crash/reboot sequence reuses
its overlay sequentially. BOOT.CFG/corruption mutations affect only overlays,
with an exact patch manifest; avoid corrupting loader prerequisites by using
post-boot fault fixtures. Use blkdebug or deterministic driver-boundary injection,
recording which layer failed. Stop QEMU before read-only overlay export for
`fsck.fat`/mtools; never flatten a whole image or repair evidence. Verify backing
image SHA-256 before/after and correlate every reboot with that hash.
Flushes MUST reach the host block backend; `cache=unsafe` is forbidden.
Crash tests MUST distinguish guest termination from simulated device power loss.
Before arming F0 panic, quiesce background storage/logging so its zero-I/O
observation remains meaningful; panic itself performs no quiescence or flush.

**[F1]** Preserve the shared worktree lock, one-QEMU rule, 2 GiB MemAvailable
minimum, systemd `MemoryMax=1500M`/`MemorySwapMax=0`, teardown and retention rules.
No concurrent heavy build, canonical rebuild or host-runner tests. Keep checker
summaries in result.json before deleting passing overlays; retain bounded failures.

## Physical procedure and exit condition

**[F1/T4]** Follow F0's explicitly authorized expendable-disk procedure, stable
identity checks, canonical write/readback SHA-256 and servicing manuals. Capture
BIOS, RAM, PCI, ATA identity, input policy and firmware dependencies for both
owned laptops. Repeat F0 regressions and normal/safe boots; exercise 100 keyboard
cycles, pointer movement/buttons, damage presentation and the FAT workload.
Record expected/observed physical motion rather than inventing QEMU delta values.
E500 MUST retain firmware-first ownership throughout.

**[F1/T4]** After durable unmount, power-cycle and verify known files on laptop
and read-only host collection with matching checksums. Limit writes to declared
test files/logs; no raw writes to mounted filesystem, boot or kernel extents.
Unsafe error injection stays T0/T3. Use paged screen photographs/transcription
or verified serial capture with complete run/sequence records; unavailable disk
logging cannot substitute for missing evidence. Apply F0 external deadlines plus
120 seconds firmware allowance. Lead records failures in new Italian diary
entries; unresolved physical input/storage failures block F1.

**[F1]** Main prereleases resume only when this gate and F0 hardware requirements
pass for the published image, and a Windows portable bundle containing that
image is re-qualified, including launch/boot on Windows and payload/license
checks. No commercial game payloads. Per [foundations-transition](foundations-transition.md),
the lead switches release policy to `active` in the same commit as resumption,
with diary evidence. The pre-push hook remains mandatory; a Linux packaging
success is not Windows runtime qualification.

## Open questions

- E500 firmware IRQ adaptation, real key-release events and memory/I/O
  dependencies need physical evidence; absent qualification blocks its input gate.
- Per-unit ATA flush/cache behavior and command latency remain unmeasured;
  unsupported durability requires read-only fallback, not a passed write gate.
- Actual shared-INTx pair coverage may remain explicitly pending when no pair
  is enabled; synthetic coverage is mandatory and cannot authorize unknown routing.

## Interfaces required from other contracts

| Contract | Mandatory interface |
| --- | --- |
| `device-firmware-ownership.md` | **F1:** lifecycle, firmware leases, bounded IRQ/deferred work, quarantine and no BIOS retry. |
| `vfs-storage-contract.md` | **F1:** mount/scan, names/handles, cache barriers, ordering, errors and logging gates. |
| `boot-memory.md` | **F1:** BOOT.CFG/menu selection, safe flag, copied input/VBE data, firmware workspace and measured cache budget. |
| `execution-abi.md` | **F1:** kernel workers, cancellation, pinned requests, boot-time stack-protector guard and unchanged six probe syscalls; **F2:** public VFS/input/display ABI. |
| `test-architecture.md`, `f0-acceptance.md` | **F1:** selector extension review, regressions, evidence, profiles, overlays, resources and physical collection. |
| `foundations-transition.md` | **F1:** cross-review, migration/license audit, installer-layout qualification and Windows/release resumption evidence. |


### F1-28 hardware sweeps

Selectors can originate in menu P, validated QEMU fw_cfg or a bounded
BOOT.CFG `probe=` line. Ordinary selectors and safe-boot-cfg retain their
existing behaviour. See boot-memory.md for the cfg-only sweep/cursor grammar.
A configured request boots automatically after the existing three-second
N/S override window; it requires no typed selector. This resolves the
f1-28 wording conflict between skipping the prompt and retaining countdown
keys by retaining only the override window, not interactive command entry.

Production table traversal, one run id, fresh boot per step:

| Phase | Sweep order |
| --- | --- |
| F0 | boot, bootinfo, allocator, protection, isolation, preempt, localfault, syslife, fpu, panic |
| F1 | registry, input, input-fault, framebuffer, ata, ata-fault, partition, fat-read, cache, safe, fat-write (write/reopen), mount-crash (cut/recovery), bootlog (write/reopen) |
| F2 | elf-load, spawn-wait, mmap, threads-wait, crash-isolation, libc-smoke, fd-table, signals-fault, app-gate |

F2 follows the linked production registry rather than inventing a second
registration order. F1 keeps table order within ordinary and multi-boot groups;
F0 panic is last. `runner` is a host probe, not a production kernel table entry.
Each boot emits its ordinary probe records and `event=SWEEP step=<n>
result=pass|fail|not_run`, with the probe in the existing envelope (no duplicate
`probe` key). Final `probe=sweep event=SWEEP_END` reports durable totals over
boot steps (10/16/9 for F0/F1/F2, 35 for all). A panic/hang without a returning
completion remains not_run in sweep totals; its actual panic evidence can
still pass the dedicated imported panic case. Failed steps do not stop dispatch.

The cursor helper writes only the existing SYSTEM/BOOT.CFG, preserves other
option lines, uses production FAT write/commit barriers, then restores a
previously closed gate. It can temporarily remount that same volume/cache
after a probe's storage shutdown, without restarting the writer or disk sink.
Unqualified durability, hardware quarantine or corrupt mounts refuse the
cursor: `result=not_run reason=readonly` halts without attempting reset.

The mount-crash first boot arms the existing marker and resets after the first
recorded post-ARM directory-publication write is made durable (LFN precedes
the owning short entry; explicit production flush if cached).
It never cleans or unmounts at the cut. The next boot uses the same volume and
existing recovery probe. This is a bounded guest-reset cut at one boundary,
not exhaustive cut coverage or actual loss of drive power. Independent export
and checker evidence remains required for qualification.

Open contract conflict: the safe criterion requires zero optional ATA
activation, while every sweep boot must durably update BOOT.CFG through ATA.
Those conditions cannot both hold for the safe step with the permitted files.
The current sweep does not force safe mode or hide ATA activation: the
ordinary safe probe reports the unmet condition and traversal continues.
A diskless safe-continuation protocol needs a lead-reviewed contract amendment;
the dedicated f1-safe suites and safe-boot-cfg are unchanged.


Additional contract conflict: the bootlog cold-reopen predicate requires the
whole boot-volume write count to equal zero, while F1-28 mandates a durable
cursor write before that same probe. **[F1, f1-32 amendment]** The storage layer attributes the sweep's cursor writes (the `SYSTEM/BOOT.CFG` rewrite with its FAT gate, clean-mark and barrier I/O) to a separate ledger reset at storage setup; raw volume counters stay monotonic. Read probes capture every counter before BEGIN and reject any non-cursor increase through completion, reporting `writes_before_probe`, `writes_during`, `volume_writes` and `sweep_writes`. Bootlog qualification and cold reopen require zero non-cursor volume writes; the cursor write before the same probe is not a violation.
Consequently the hardware cold-reopen import cannot pass that predicate until
a reviewed contract distinguishes cursor persistence from workload writes.
FAT write/read qualification and panic write baselines likewise must account
for the explicit cursor writer without concealing its I/O.
