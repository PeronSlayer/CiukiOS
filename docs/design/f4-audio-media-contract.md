# F4: audio, media and shared volumes

Author: Codex. Reviewer and integrator: Claude (lead).
Status: approved after the Codex–Claude cross-review, 2026-10-11, with the
lead's decisions recorded in the final section

Research and repository comparison: 2026-10-11. Directive:
[f4-00](../directives/f4-00-audio-media-contract-draft.md).
Every requirement below carries **[F4]**, unless a later phase is explicit.
These are proposed project policies informed by the sources, not claims that
the current kernel implements or has qualified these services. Approval and
the amendments listed under interfaces are required before F4 code.

## Decision and sources

**[F4]** One canonical image MUST detect devices at runtime. Implement native
PCM playback and a kernel mixer first, with Intel ICH AC'97 as the first
physical transport and QEMU AC97 as its protocol twin. ESS Maestro-2E is the
second, independently qualified transport required for the E500. The mixer
MUST accept native streams and each DOS VM's SB16/OPL output concurrently.
Safe mode disables physical audio and optional acceleration.

**[F4]** Shared DOS drives MUST use the host VFS through CIUKIDOS, including
EXEC and FCB paths; they MUST NOT export sectors. Optical drives use native
ATAPI PIO and read-only ISO 9660/Joliet mounts. NTFS uses an independent,
bounded Ciuki read-only driver informed by GPLv2-compatible NTFS3 sources.
No Windows NT NTVDM dependency is introduced: the personality/VFS division
remains the Windows 95/98-class model in [dos-dpmi-contract.md](dos-dpmi-contract.md).

The binding inputs are [foundations-transition.md](foundations-transition.md),
[dos-dpmi-contract.md](dos-dpmi-contract.md),
[device-firmware-ownership.md](device-firmware-ownership.md),
[vfs-storage-contract.md](vfs-storage-contract.md),
[hardware-baseline.md](hardware-baseline.md) and
[test-architecture.md](test-architecture.md). Their ownership, memory,
licensing, error and evidence rules continue to apply.

Primary sources, including archival copies of vendor documents:

| Reference | Source and use |
| --- | --- |
| S1 | Intel [AC'97 Component Specification 2.3, rev. 1.0, April 2002](https://hands.com/~lkcl/ac97_r23.pdf), §§4.5, 5.3, 5.7–5.8: codec readiness, volume, optional variable-rate audio and accepted-rate readback. Revision 2.3 is a reference, not a minimum imposed on the older target codecs. |
| S2 | Intel [82801CA ICH3 datasheet](https://www.intel.co.kr/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf), §§5.18, 13.2.1–13.2.10: audio DMA, descriptor ownership and codec semaphore. Linux [intel8x0.c, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c), `snd_intel8x0_setup_periods`, update and trigger paths, supplies a second implementation reference. |
| S3 | QEMU [AC97 model, v9.2.0](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/audio/ac97.c): emulated 82801AA controller, descriptor progress and status. [QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html) distinguishes the audio device from its host backend and buffering. The installed binary/version and machine/firmware hashes remain part of evidence. |
| S4 | Linux [es1968.c, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/es1968.c): Maestro-2E identity, APU/wave-cache engine, codec access, hardware cursor and 28-bit DMA mask. Its GPL-2.0-or-later source is a compatible reference; AC-link compatibility does not make its DMA engine an ICH. |
| S5 | Creative [Sound Blaster Series Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf), chapters 3, 4, 6 and appendix A: virtual DSP commands, DMA units, mixer and interrupt acknowledgement. Yamaha [YMF262 catalog, 4MF262A6](https://map.grauw.nl/resources/sound/yamaha_ymf262.pdf): OPL3 register banks and synthesis modes. These describe virtual guest hardware only. |
| S6 | Microsoft [MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/), INT 21h/3Dh, 45h/46h, 4Bh, 5Ch and 59h; FreeDOS [dosfns.c](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/dosfns.c), `DosOpenSft`/`DosCloseSft`: access/share flags, PSP/JFT/SFT ownership and rollback. These are behavior references, not imported code. |
| S7 | SFF committee [SFF-8020i, revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf), §§5 and 10: ATAPI PIO phases, packet commands and sense. T10 [MMC draft catalogue](https://ftp.t10.org/drafts.htm) identifies MMC-3 revision 10g; [archival draft](https://www.13thmonkey.org/documentation/SCSI/mmc3r10g.pdf). Full MMC PDF retrieval was unavailable in this research; command behavior here is grounded in SFF-8020i and the upstream [QEMU ATAPI model, v9.2.0](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/ide/atapi.c). |
| S8 | Ecma [ECMA-119, sixth edition, December 2025](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf), volume/directory structures and annex C; Microsoft [Joliet specification, archival copy](https://pismotec.com/cfs/jolspec.html): supplementary hierarchy and big-endian UCS-2 names. |
| S9 | Linux NTFS3 [ntfs.h](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/ntfs.h), [run.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/run.c) and [super.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/super.c), all at v6.12 and marked GPL-2.0; [upstream documentation](https://docs.kernel.org/filesystems/ntfs3.html): on-disk records/attributes, runlists and mount behavior. A reference driver's broader capabilities do not become Ciuki capabilities. |

### Target identity and repository comparison

| Target | Observed identity or candidate | F4 consequence |
| --- | --- | --- |
| Owner's T23 | Boot-info PCI records in local captures `44444444`, `66666666`, `77777777`: `00:1f.5`, `8086:2485`, subsystem `1014:0222`, class `0401`, reported IRQ 11/pin 2. Archived audio notes identify a CS4299 codec. | Select ICH3-M only after claims/routing validation. Read codec ID and readiness again under the audio lease; inventory and historical codec identification do not qualify the new backend or prove current speaker output. |
| Owner's E500 | Archived diskseq39 report, `ACTIVE.CFG`: `125D:1978`, subsystem `0E11:B112`, `AUDIO=UNSUPPORTED`. [Recorded analysis](../../dev_diary/2026-10-09-01-report-log-fisici-diskseq39.md) confirms the legacy ESS path was unsupported. | Maestro-2E requires its own engine backend and physical qualification. The Intel chipset name and the T23's Intel AUDIO.LOG do not establish E500 audio support. Preserve the E500 firmware-first input lease. |
| QEMU AC97 twin | Upstream model advertises Intel 82801AA (`8086:2415`), not the T23's ICH3-M. Existing baseline twins do not attach audio. | Add an explicitly declared AC97 runtime profile for F4 tests, using the same image. It covers the common protocol; it cannot stand in for E500 ESS or T23 analog/mute/routing behavior. |
| Generic 1998–2004 desktops | Baseline lists SB16, SB Live!, ES1370/1371 and ICH AC'97 as candidates; no measured board audio inventory exists there. | Qualify enumerated Intel ICH variants individually. Other physical audio families remain unsupported until separately directed; a virtual SB16 is not a physical ISA driver. Unknown audio remains disabled with a reason. |

The five required legacy documents were extracted temporarily from
`legacy/CiukiOS-docs-legacy-2026-10-09.zip` into `build/`, read and compared
with retained code; none is restored to `docs/`:

| Archived document | Requirement retained for F4 |
| --- | --- |
| `docs/design/ac97-music-streaming.md` | Publish prepared descriptors through LVI, retire by actual cursor progress, preserve current/prefetched slots and use sample rather than byte counts. |
| `docs/design/dos-audio-block-scheduling.md` | Source generation stops at each SB block boundary before eligible guest refill; hardware periods and SB blocks are distinct. Its stale-double-buffer reproduction is a mandatory regression. |
| `docs/design/dos-audio-codec-timing.md` | Serialize posted codec transactions, verify the accepted rate and retain rational conversion phase across batches. |
| `docs/design/t23-startup-audio.md` | Stream startup sound through bounded buffers; no whole-track allocation. DMA-fetched bytes and EOF do not prove audible output. |
| `docs/validation/2026-10-04-ac97-audio-latency.md` | Separate guest lookahead, FIFO/DMA failures, emitted PCM and host/physical output latency. A zero halt counter alone proves no continuity or sound-quality claim. |

Current retained `session_audio.c` has guarded codec transactions and
incremental conversion, but `codec_wait` still uses an iteration guard.
`guest_peripherals.c:cvgp_render_audio` mixes through the legacy adapter;
`session_opl.cpp` still has one static chip and x87 setup. They are reuse
inputs, not a native multi-client service. CIUKIDOS `int21_open` still masks
sharing/inheritance bits, find-first uses common cursor state, and LFN sector
services remain present. The bridge must change those internal paths. No
replacement or retirement is authorized by this draft.

## Native audio API for processes

**[F4]** `/dev/audio` MUST be an output character device. Each independent
`open(O_WRONLY)` creates one stream; `dup` and explicitly inherited fds share
it and its position/configuration. `O_RDONLY`/`O_RDWR` playback opens fail
`EACCES`; recording is not advertised. Without a qualified backend, open
fails `ENODEV`. The SDK MUST advertise the F4 device-control extension only
after its ABI and wrappers are specified in `execution-abi.md` and
`posix-subset.md`; existing syscall numbers MUST NOT be repurposed.

**[F4]** Input formats MUST be interleaved unsigned 8-bit or signed
little-endian 16-bit PCM, mono or stereo, with integer rates 8,000–48,000 Hz.
Default: signed 16-bit stereo at 48,000 Hz. Float, encoded/compressed audio,
multichannel, capture and direct hardware mappings fail `EOPNOTSUPP`.
`write` requires whole frames; a partial-frame length fails `EINVAL` before
consumption. Zero-length writes return zero. User buffers MUST be validated
and copied to owned storage; no user mapping becomes a DMA buffer.

| Proposed device-control operation | Required semantics |
| --- | --- |
| `AUDIO_GET_CAPS` | Version/size, supported formats/rates, stream quota, period/ring bounds, selected backend identity and generation; no hardware addresses. |
| `AUDIO_SET_FORMAT` | Validate the complete request before mutation. Only before first queued frame or after reset; otherwise `EBUSY`. Return actual input format and effective output rate separately. No silent input-rate substitution. |
| `AUDIO_GET_STATUS` | Monotonic accepted/consumed source-frame counts, queued frames, output-frame cursor, backend rate/generation, source starvation, FIFO errors, DMA halts, deadline misses and last error. Hardware cursor is labeled fetched, not audible. |
| `AUDIO_SET_GAIN` | Per-stream left/right integer gain 0–100, default 100; mono expands to both channels. Update at the next uncommitted mixer quantum. |
| `AUDIO_DRAIN` | Wait until preceding stream data, converted tail and the qualified backend drain interval finish. Cancellable; failure returns `EIO`, interruption `EINTR`. Never equate a final descriptor fetch with analog completion. |
| `AUDIO_RESET` | Discard uncommitted frames, reset conversion phase and invalidate prior stream work. Already published physical periods finish normally; the handle can then be configured again. |

**[F4]** Device-control records MUST use fixed-width fields, explicit version
and byte size, checked reserved zeros and copied buffers. Unknown operations
fail `ENOTTY`; unknown versions fail `EOPNOTSUPP`, malformed fields `EINVAL`.
The lead MUST freeze numeric command/syscall assignments and record layouts
in the native ABI before implementation; these names specify behavior here.
No poll/select or shared-memory ABI is implied.

**[F4]** Each stream MUST have a bounded copied ring, initially 2,048 source
frames, charged at admission; at most eight independently opened native
streams, plus the audio sources of admitted DOS VMs. Failure to reserve a
stream returns `ENOMEM` with no partial publication. A full ring blocks a
writer through the existing cancellable wait mechanism; `O_NONBLOCK` returns
a whole-frame short count or `EAGAIN` if no frame fits. A signal before any
copy returns `EINTR`, after a copy the positive byte count. Backend failure
wakes waiters with `EIO`, never fabricated success. `lseek` returns `ESPIPE`.
Final close/fault teardown drops uncommitted audio without blocking for a
drain; published periods stay kernel-owned until safely retired.

**[F4]** The first nonempty write starts that stream automatically; it needs
no additional start syscall. Prime the physical queue with prepared mixed
PCM/silence without counting padding as accepted source data. A drained stream
can receive more writes in the same format; reset is required to change format.
Stop an idle backend only when no active DOS clock/source requires continued
service, and use the same proved shutdown path as explicit disablement.

**[F4]** The default physical quantum MUST be 256 stereo frames. At 48 kHz
it lasts 5.333 ms; eight published periods provide 42.667 ms of transport
lead. F4-32 Amendment 1 uses eight periods also when a VM is attached, a measured-gap-derived
idle cushion (134 ms plus two periods), and increases the active session lead
after a late service. See the [audit](../validation/f4-31-audio-audit.md).
A 48-kHz
low-latency client SHOULD maintain at most one unconsumed source quantum;
an arbitrarily full source ring adds its own queue duration. Status exposes
both queues rather than calling all writes low-latency.

**[F4]** The initial qualification budget for a newly submitted low-latency
quantum or timestamped OPL event MUST be ≤60 ms to physical output at 48 kHz:
≤42.667 ms published lead, ≤5.333 ms mixer quantization, ≤1 ms deferred wake
and the remaining margin for conversion/codec/output. Mixer/refill work MUST
yield within 2 ms and obey the existing ≤250 µs hard-IRQ/IRQ-off maximum
(50 µs target). Input-to-DOS-refill scheduling is measured separately.
F4-32 Amendment 1 replaces the f4-31 VM target with ≤50 ms nominal at 48 kHz:
`8 × 256 + 304 = 2352` frames, or 49.0 ms including the source floor.
The host capacity-callback replay exhausts four periods in a single 1024-frame
request without a worker turn; eight periods retain 1024 frames of margin.
The lead's startup-halt evidence on all three QEMU VM profiles with four
periods, versus intact PCM with eight, selects this frontier. QEMU's 10 ms
timer is not a bound on callback capacity; see the
[F4-32 audit and primary sources](../validation/f4-31-audio-audit.md#f4-32).
Eight periods also exceed the original missed-service budget of
`480 + 256 + 256 = 992` frames and ICH3's `256 + 256 + 256 = 768` frames.
The nominal target applies before a measured scheduling gap increases the
session lead. The measured 113 ms T23 gap still requires adaptive growth.
The first native player write is at most one period; subsequent turns may fill
the 2048-frame source stream (F4-36). `latency_frames` includes source,
transport and drain allowance. These targets describe normal low-lead service;
an adapted lead covering a 134 ms scheduling gap necessarily exceeds them.
Such a session must expose its lead and latency, never claim the low-latency
gate passed. Physical qualification must bound active scheduling gaps.
An uncertain cursor retains the last committed position. It MUST NOT block
publication into slots independently proved free, and MUST re-arm the refill
worker after a real block of at most 1 ms. The 2 ms handoff retry deadline is
not extended to match an emulator's audio callback interval. After proven
stop/reset, recovery silence MUST preserve the next unrendered VM frame,
including a pending publication and converter lookahead. Source recovery
delay is reflected in the VM pacing lead; it is not permission to skip PCM.
Report maximum and percentile latency, clock calibration and measurement
uncertainty. QEMU WAV capture proves PCM/timeline behavior, not this physical
latency budget. A slower negotiated backend needs a recorded period/lead
decision and equivalent measurements before qualification; never change a
guest's programmed rate to conceal lateness.

### Music decoded reservoir (F4-34, 2026-10-19)

Research checked the pinned [minimp3 API and synchronization guidance](https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/README.md),
[stb_vorbis handle ownership and stdio input](https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c),
and upstream newlib [setvbuf](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/libc/stdio/setvbuf.c).
The existing player decodes inside its writer pump and limits decoded plus
queued PCM to 256 frames. This couples playback to synchronous file waits.

**[F4-34]** A normal-priority reader owns decoding while active. Commands join
it before seek/close/replacement; no decoder handle has concurrent callers.
The writer never holds a reservoir mutex across file I/O. The bounded stereo
S16 ring holds 131072 frames (512 KiB, 2.731 s at 48 kHz); supported output
rates must permit at least two seconds within that cap. Start after 100 ms
of decoded PCM or EOF. F4-36 supersedes the original 256-frame device queue:
limit only the first successful write to 256 frames, then write up to all free
space whenever the 2048-frame stream is below 1792 frames.
Retain submitted but not yet audible PCM in the same ring for exact pause
replay; pause preserves decoded PCM. Seek/stop/track changes flush it.

Use a separate 256 KiB stdio read-ahead buffer, installed before any stream
operation and retained through fclose, for MP3, WAV and Ogg. Keep minimp3's
small synchronization window so a bulk storage refill does not cause a
256 KiB memmove on every MPEG frame. Decode in batches of up to 4096 frames. Yield only above half-full; cached
adapter reads no longer force a yield. Wait on `space` below one free batch,
and signal when audible history retirement crosses that threshold. Submission
moves PCM into retained history, so it does not itself release a ring slot.
Decoder code and resampling arithmetic remain scalar.

Position remains the device's audible source cursor (written minus
latency), never the decoder cursor. A starved stream contributes the existing
kernel silence; the writer submits no stale PCM or artificial source frames.
Expose Buffering... after 300 ms of continuous starvation; clear it after two
seconds without starvation. Keep a bounded diagnostic (at most one per second)
and an episode count in the close record. No kernel or stream ABI change.

F4-36 publishes changed displayed fields at most ten times per second,
coalescing steady progress to two publications per second. The view invalidates
only changed title/status/time/list/mixer regions and changed seek-thumb pixels.
The position label changes at whole seconds. Unchanged mixer polling is free.
A protected `[music] level` record every five seconds of playing reports sampled
reservoir min/mean/max, output reader frames/s, elapsed decode microseconds per
output PCM frame (including I/O and preemption), successful writes/s, accepted
bytes in the window, sampled device queue min/max, starvation episodes, and UI
publications/renders. `ui_mps` and `repaint_mps` are milli-events per second
(2000 means 2/s). UI render counts come from the actual paint callback and
include input/dialog redraws. Counters share the reservoir mutex, except the
UI counters which are atomic. See [Music research and host model](music-player.md#f4-36-contention-research-and-implementation-decision-2026-10-19)
for the primary sources and the limits of host scheduling evidence.

### Volume and persistence

**[F4]** `/dev/mixer` MUST expose read-only master left/right volume and mute
to native clients. A writable control handle MUST be delegated by the
bootstrap supervisor to the desktop/settings service; public opens cannot
acquire that authority. This follows F2 capability delegation, without adding
users or Unix permissions. The control operations are `MIXER_GET_MASTER` and
`MIXER_SET_MASTER` (volume 0–100, explicit mute, version/size validation).
Stream gain and guest mixer registers MUST NOT change the global setting.

**[F4]** Persist master volume/mute in `C:\SYSTEM\AUDIO.CFG`, using a small
versioned record and checked ranges. Default: volume 50/50, mute clear.
**F4-18 amendment:** [master-volume settings and IBM EC policy](f4-18-audio-levels.md)
supersedes this paragraph's default and persistence ownership: default 20/20,
`System\\Audio` / `Master` authoritative, AUDIO.CFG retained as the validated
fallback. Boot/UI sounds use this level. On IBM ThinkPads with the qualified
EC amplifier, that amplifier is the effective master, the codec stays at
unity, and digital mixing bypasses the same gain once.
Missing/invalid settings select defaults and report the reason; explicit
mute stays muted across reboot and VM exit. The settings service applies
changes immediately and commits them through the VFS durability interface;
on a read-only system volume or commit failure it reports applied-but-unsaved.
A failed save MUST leave the last valid saved record usable. No disk I/O
occurs in audio IRQ/refill paths. Backend reinitialization applies the latest
setting, not a VM's entry-time snapshot. Physical codec attenuation and software
master gain MUST have one mapping: apply the logical 0–100 master as linear
software gain to the accumulated mix before final saturation; configure the
qualified PCM/master/headphone paths at unity and use their mute controls for
explicit master mute. Per-device missing/unusable paths are reported, not
guessed. Hardware mute MUST agree with explicit master mute, and no additional
codec attenuation silently scales the software setting.

## Kernel mixer and physical backends

**[F4]** The kernel mixer MUST perform integer PCM conversion, rational
resampling, per-stream gain, accumulation in a sufficiently wide signed
integer and final saturation to stereo signed 16-bit. Saturation occurs
after summing sources. Preserve fractional phase and interpolation history
across short batches and period boundaries; mono duplication and channels
remain independent. Rate or stream reset explicitly starts a new phase.
No kernel x87/MMX/SSE instructions or arbitrary caller callbacks are allowed.
The mixer uses bounded preallocated buffers and never reads files or performs
codec transactions under IRQ locks.

**[F4]** Mix all admitted sources, including unfocused VMs, with explicit
per-source mute/gain; focus alone does not mute a VM. A starved source contributes
silence and increments its own counter while other sources continue. A muted
VM still advances DSP/DMA/timer state. One output cursor is authoritative for
physical transport. Software-source time, generated frames and physical-fetched
frames MUST be separate counters; catch-up must not replay old committed PCM.
Audio allocations are charged before admission under the existing measured
memory/headroom policy; no VM quota or RAM floor is increased by this draft.

### Intel ICH AC'97 first

S1/S2 distinguish codec registers from bus-master transport. ICH PCM-out
uses NABMBAR offsets BDBAR `10h`, CIV `14h`, LVI `15h`, SR `16h`, PICB `18h`,
PIV `1Ah`, CR `1Bh`; the audio CAS is `34h` (not the modem semaphore).
The BDL is eight-byte aligned, at most 32 eight-byte entries; lengths count
16-bit samples, so 256 stereo frames encode 512 samples. CIV wraps modulo 32.
SR separates DMA halt (`DCH`), final fetch (`LVBCI`) and FIFO error (`FIFOE`).

**[F4]** Claim validated BAR extents, DMA constraints and interrupt routing
before writes. Use pinned kernel BDL/staging segments addressable by the
controller (32-bit mask for these ICH devices); enable bus mastering only
after publishing complete ownership. Reserve a 32-entry ring but initially
publish eight periods, leaving unpublished slots as a guard. Payload and
descriptor writes MUST precede LVI with the required ordering barrier.
Current, queued and prefetched slots are immutable. Retire by stable hardware
cursor/status observations and software absolute counters, including wraps;
an ambiguous missed-wrap/stall is a discontinuity requiring bounded recovery,
not permission to overwrite a potentially active slot. S2 and Linux's period
setup/update paths are the implementation references.

**[F4]** Acquire CAS for each codec transaction, use elapsed-time deadlines
and serialize readback after posted writes. Check codec ID, link-ready and
playback power readiness before RUN. Prefer a verified 48-kHz stereo output;
negotiate VRA only when capability/readback permits it, otherwise convert in
software to the accepted rate. All-ones, timeout and invalid-rate readbacks
fail activation. Ordinary codec accesses have a 100-ms deadline; complete
activation/stop has a 500-ms deadline, with waits outside IRQ-off sections.
Any required chipset/codec reset MUST first account for other AC-link users;
an unresolved modem/firmware dependency prevents reset and activation.

**[F4]** Completion IRQs only snapshot progress, count causes before clearing
documented W1C bits, acknowledge the device and wake refill. The PIC service
owns physical EOI and shared INTx dispatch. DMA halt, FIFO starvation and
source starvation have distinct counters. Pause retains transport state;
reset is issued only after RUN is clear. Stop rejects new work, waits for
proved idle, drains callbacks, revokes mappings and then frees staging.
Unproved shutdown quarantines the device and pinned buffers; it cannot
silently fall back to another driver using those resources. S2/S3 supply
register/model behavior; ownership and these deadlines are project policy.

### ESS Maestro-2E second

**[F4]** Implement a separate `ess1978` backend for the observed E500 identity
after the common mixer/AC97 gate. Base its engine/rate/cursor/codec operations
on S4, with a per-file license review before any source reuse. Allocate within
the documented 28-bit DMA mask, and derive its alignment/window constraints
from that engine, rather than copying ICH BDL/CIV programming. Qualification
MUST establish actual clock/rate, stereo ordering, wrap/refill and stop/reset
behavior on the E500. Its period model MUST expose the common frame cursor,
deadline/error and drain interfaces even when physical notification granularity
differs. It MUST preserve firmware input/thermal dependencies. No documented
QEMU twin in the baseline models Maestro-2E: host register fixtures are protocol
evidence, and E500 sound/latency remain mandatory T4 evidence.

**[F4]** S3 Savage and ATI Mach64 2D acceleration remain deferred and disabled:
this draft does not establish sufficiently solid per-chip programming and
successful copy/idle/restore evidence to qualify them. The recorded T23 copy
failure remains open; framebuffer presentation continues through the existing
qualified path. A later acceleration directive must satisfy the device contract.

## DOS VM audio path

**[F4]** Consume the per-VM DSP/mixer/DMA and OPL state delivered by f3-05;
never forward SB/OPL ports or guest DMA to hardware. F3 remains responsible
for device discovery and its command matrix. F4 MUST connect every advertised
playback mode to audible PCM and explicitly fail unsupported modes without
advertising recording, MPU-401 or physical OPL passthrough.

**[F4]** SB output MUST honor signed/unsigned 8-/16-bit, mono/stereo, programmed
rate, single-cycle/auto-init, pause/resume/exit-auto-init and mixer gains from
the virtual device. Distinguish DSP sample units, stereo frames and 8237
byte/word counts. For DSP 4.xx, D1h/D3h change the speaker-status latch without
gating DAC playback (S5); a DOOM binary that never sends D1h must still sound.
Reset/status reads and 8-/16-bit IRQ acknowledgement MUST retain f3-05 behavior;
acknowledging the DSP and issuing PIC EOI are separate guest actions.

**[F4]** Generate only up to the next SB block boundary, latch its DSP cause
and request the authoritative virtual PIC. If eligible, yield to the guest
handler/refill before reading its next DMA block, with no host lock retained.
Incremental conversion MUST accept that partial batch without prefetching
guest samples across the boundary. Do not render a multi-period catch-up
batch before considering intervening guest IRQs. Regression: a 64-unit
auto-init double buffer over 2,048 units must produce 32 sequential fresh
blocks and 32 eligible block services, rather than stale repeated halves.
The guest rate and consumed sample units determine virtual block progress;
an ICH completion IRQ is not a guest SB completion. Pace source consumption
on the declared VM sample timeline, report its lead over physical output,
and keep its fractional clock across host timer/refill calls. Device-worker
catch-up or a backend rate change must not speed up that timeline.

**[F4, F4-35]** A source restart that would discard a PCM prefix SHOULD
instead delay the unconsumed stream when the required shift fits the
source's remaining **960-frame (20 ms) lifetime budget** at 48 kHz. Add
that shift to the PCM timestamps and pending OPL/direct-DAC event timestamps
together, count a re-anchor, retain the guest sample rate and continuous
stream phase, and preserve order without replaying rendered frames or events.
Already sounding OPL voices continue; future register writes retain their relative spacing.
The budget is cumulative, including across transfers: repeated jitter cannot
create unbounded source lead. Larger lateness or an exhausted budget retains
the discard policy, with exact per-block dropped-frame counts, individual
frame lateness buckets (<1, [1,5), [5,20], >20 ms), and lead/lag snapshots at
the drop. Source lead/lag includes the common shift. The healthy nominal
latency target is unchanged; late recovery can add at most 20 ms to it,
separately from an adaptive transport lead or proven-stop recovery silence.

The 20 ms budget follows the measured host prefix losses under a 20 ms
service schedule, rather than changing the DSP rate or predicting guest
DMA. T23 C6/20 observations use 256 byte units per interrupt: 128 stereo
frames at 11025 Hz (11.61 ms); 20 ms covers less than two such blocks.
The 64-unit mono fixture is 5.80 ms per block and covers shorter attacks.
PIT and DSP both advance by elapsed VM ticks, independent of guest task
scheduling. The available T23 records show nominal DMA/PIT cadence; they do
not establish drift against independently timed physical audio. Preserve
producer timestamps and use actual DMA frame availability to correct the
source/output mapping. See [F4-35 measurements and clock limits](../validation/f4-31-audio-audit.md#f4-35--bounded-delay-for-late-vm-pcm-2026-10-19).

**[F4]** While virtual IF is clear, or the virtual PIC masks/in-service state
blocks delivery, DSP DMA consumption and block deadlines MUST continue on
the VM audio timeline. Causes latch/pending bits follow the advertised SB/PIC
hardware semantics; do not invent an unbounded IRQ queue or force guest IF
on. At eligibility after STI/MOV-SS shadow, inject the pending cause through
the current DPMI handler or V86 reflection. Auto-init DMA MUST never wait
for acknowledgement: it continues through block boundaries and wraps the
live guest ring, replaying unrefilled data. A boundary with the line already
asserted counts a merged IRQ without raising another edge. `irq_wait` is
diagnostic only; the host consumer MUST NOT gate playback on it. Count late
acknowledgements, ISR entry latency and potentially stale samples (f3-40,
[research and validation](../validation/f3-40-sb16-irq.md)). A stalled guest never
blocks physical refill, native playback, another VM or host preemption.

**[F4]** OPL address/data banks, timer masks/status/reset, key-on, rhythm,
operator parameters and OPL2 compatibility MUST be per VM. Honor OPL3 new-mode
register `105h`, four-operator pairing `104h`, and channel routing; map the
selected outputs to stereo without claiming an external four-channel device.
Timestamp writes with VM generation and an absolute sample-clock position;
apply them in order before synthesizing the affected samples. Guest timer/status
reads use emulated time, independent of whether physical audio is muted.

**[F4]** DBOPL reuse MUST keep its GPL-2.0-or-later notices and give each VM a
separate chip. Setup/synthesis runs in a scheduled ring-3 worker with ordinary
FPU ownership, replacing the singleton ring-0/x87 adapter. A bootstrap-delegated
bounded message channel carries validated register events and copied PCM,
identified by VM/worker generation; the worker gets no physical DMA, guest
memory or PIC ownership. It must meet the audio deadline without a public
priority-elevation API. Worker fault/queue overflow produces an explicit OPL
failure and bounded silence for that VM; teardown revokes pending events and
cannot use a stale worker completion. Register/mixer changes never overwrite
already committed physical periods.

## DOS-to-VFS bridge

### Routing, handles and process lifetime

**[F4]** The VM launch descriptor MUST map DOS drive letters to either a
private FAT16 disk or a host VFS mount/root capability. The private boot drive
remains private; shared letters must not alias it. Resolve the drive and route
inside CIUKIDOS before FAT/LFN pathname resolution. Apply this to all file
services, FCBs, EXEC/overlays, environment executable paths, relative names,
`X:name`, `X:\name`, current-drive selection and per-drive cwd. Non-file INT 21h
and DOS devices remain personality-owned. A shared root bounds `..` and path
resolution; aliases cannot escape it. DPMI pointers require validated selector
translation through the personality boundary, not real-segment assumptions.

**[F4]** Each PSP MUST own a JFT whose entries reference counted SFT entries;
each shared-file SFT owns exactly one opaque kernel open description, including
64-bit position, access/share flags and original owner identity. The kernel
validates VM, PSP and mount generations, never trusting guest-edited SFT state.
Duplication/forced duplication and EXEC inheritance share offsets; distinct
opens do not. No-inherit bits are preserved, and a failed EXEC rolls back only
new references. Close/FCB close/normal exit/fault cleanup each release exactly
their references. Kernel teardown MUST work without executing damaged guest
tables. No guest pointer is retained across blocking I/O: copy input, retain
owned request state and revalidate/pin copy-out destinations before completion.

### Sharing and byte-range locking matrix

**[F4] Proposed VFS amendment.** The VFS contract owns this matrix but leaves
it unwritten. Record the decision here within f4-00's one-file scope; the lead
MUST incorporate the agreed matrix into `vfs-storage-contract.md` before
bridge code. It applies to one canonical file node across aliases, VMs and
native clients. An existing deny mask is tested against a new access mask:

| Existing explicit share mode | New read | New write | New read/write |
| --- | --- | --- | --- |
| Deny none (`4`) | allow | allow | allow |
| Deny read (`3`) | refuse | allow | refuse |
| Deny write (`2`) | allow | refuse | refuse |
| Deny all (`1`) | refuse | refuse | refuse |

**[F4]** Also test the new deny mask against every existing access mask; both
directions must allow, even for two independent opens by the same PSP.
Duplicating an existing description is not a new open. Native opens use deny
none, but still honor existing denials. Validate access bits 0–2, share bits
4–6 and no-inherit bit 7; reserved/unsupported encodings fail, never get masked
into success. Attribute read-only and mount read-only checks precede admission.

**[F4]** Compatibility mode (`0`) uses this explicit local-machine policy
informed by S6: treat each VM as one DOS machine. Compatibility opens within
the same VM can coexist regardless of PSP/access, subject to read-only access
checks. Across different VMs they coexist only when the file has the DOS
read-only attribute and every access is read-only. Compatibility and any
explicit sharing mode conflict in either opening order, including native
deny-none opens. This conservative mixed-mode rule is a declared Ciuki policy,
not a claim of all Windows/DOS redirector edge cases. Attribute changes that
would invalidate admitted compatibility readers fail while those opens live.

**[F4]** Range locks MUST use checked 64-bit half-open ranges `[start,end)`;
DOS `5Ch` supplies 32-bit start/length, with no arithmetic wrap. Zero length
is a no-op. Locks belong to the open description behind the SFT; dup/inherited
references share ownership. Any overlap with another description is refused;
reads/writes overlapping another owner's lock fail, including native I/O.
Same-owner overlapping/repeated locks fail rather than creating ambiguous
ownership; unlock must exactly match an acquired range. Final description
close or VM teardown releases its locks once. No blocking guest retry loop
holds VFS locks. Conflicting destructive operations (truncate, unlink,
replacement, rename or attribute changes) fail while DOS shared descriptions
or range locks would be violated; preserve existing native-only semantics
for nodes with no DOS participants.

**[F4]** Error translation MUST be deterministic: sharing violation extended
DOS `32`, lock violation `33`, ordinary access denied `5`, invalid access
`12`, file/path missing `2`/`3`, exhaustion `4`, no search match `18`, unknown
drive `15`, write-protected volume `19`, unsupported request `1`, storage or
stale-mount failure `31`. Set CF for handle-style failures and preserve the
function's unspecified registers; `59h` records the extended error/class/
action/locus. Preserve function-specific critical-error and FCB return
conventions; if a critical-error handler is invoked it runs outside host
locks, with bounded continuation and no unauthorized retry after an issued
ATA failure. Oversized DOS file/seek/free-space values fail with documented
function-specific overflow/access errors, never wrap or silently truncate.
The bridge implementation directive must enumerate the per-function mappings.

**[F4]** The mandatory shared-drive handle surface includes create/open/close
(`3Ch/3Dh/3Eh`, extended open `6C00h`), read/write/seek (`3Fh/40h/42h`),
attributes (`43h`), timestamps (`57h`), duplicate (`45h/46h`), directories
(`39h/3Ah/3Bh/47h`), delete/rename (`41h/56h`), searches (`4Eh/4Fh`),
locks (`5Ch`) and commit (`68h`), plus the already advertised CIUKIDOS LFN
counterparts. Zero-length DOS writes retain DOS truncate/extend semantics,
subject to shares, locks and read-only policy. Classic handles refuse an
existing file larger than `0xFFFFFFFF` bytes with CF/AX=5; a seek or growth
outside `0..0xFFFFFFFF` similarly fails before changing position/content.
FCB record arithmetic failure returns its failure status without mutation.
Free-space queries use checked representable geometry; classic `36h` returns
AX=`FFFFh` if it cannot represent the result, and extended queries return their
defined error. Read-only ISO/NTFS returns zero free space when representable.
These refusals preserve native 64-bit values rather than narrowing them.

### Searches, FCBs and visibility

**[F4]** Find-first/next MUST associate an opaque bounded continuation with
VM/PSP, DTA identity and mount generation. Multiple DTAs can interleave without
sharing cursor state. Guest bytes carry tokens, not kernel pointers; forged,
stale or cross-VM tokens fail. Define search results as a bounded snapshot of
names/attributes at find-first, retained until exhaustion/replacement/PSP exit;
new searches see committed namespace mutations. Quota exhaustion fails the
search without replacing an older valid continuation. Short-name wildcards,
attribute masks and LFN searches use the same host namespace and aliases.

**[F4]** For FAT, use the existing on-disk 8.3 alias. For ISO/Joliet and NTFS,
prefer a valid supplied DOS alias; otherwise generate collision-checked aliases
deterministically in a bounded per-directory view, independent of lookup order.
Repeated searches/remount of unchanged media MUST give the same alias. Unicode
names follow the existing UTF-8/path bounds and guest CP437 conversion; an
unmappable long name is not silently replaced. Case/alias collisions that
cannot be represented unambiguously fail rather than selecting an arbitrary
file. Host VFS owns aliases, cache and mutation visibility, not guest FAT state.

**[F4]** Standard/extended FCB open/close, sequential/block/random reads and
writes, find-first/next, rename/delete and file record counts MUST use these
same nodes, descriptions, shares and locks. Validate record size, record-number
arithmetic and DTA buffer extent before I/O; partial final reads use the DOS
padding/status convention, and record-count overflow fails rather than wraps.
An FCB contains no trusted kernel handle. Map its lifetime through a PSP-owned
token and clean it up on close/exit. EXEC and overlay reads on shared drives
use VFS bytes, not `int21_exec_load_to_es` FAT chains; failure releases loader
references and preserves the parent. Reads after successful writes see coherent
cached bytes, and `68h` commit exposes delayed durability errors. Successful
commit survives crash/reopen; uncommitted data has only the existing VFS crash
guarantees.

### Raw access refusal

**[F4]** A shared mount MUST have no guest BIOS drive or ATA sector mapping.
INT 13h CHS/EDD, INT 25h/26h, raw/generic block IOCTLs and LFN sector helpers
refuse shared-drive reads as well as writes before buffer or media mutation.
INT 13h reports carry/status failure for unmapped drives; DOS raw functions
use their documented failure/stack conventions and access-denied status.
Keep ordinary file/device metadata queries available where specified. Bypass
`kernel_lfn_read_sector`, `kernel_lfn_write_sector`, `kernel_lfn_flush_fat` and
`kernel_lfn_invalidate_fat` for shared letters. Private FAT16 sector services
remain VM-local and functional. A guessed disk number cannot reach the host
system volume, optical medium or NTFS partition.

## ATAPI and ISO 9660 read-only mounts

**[F4]** Extend the existing ATA controller owner with serialized ATAPI PIO;
do not attach a competing disk/CD owner to the same task file. IDENTIFY PACKET
DEVICE establishes device type/packet size. PACKET `A0h` transfers the padded
packet through bounded DRQ phases, validating CoD/I/O direction, device byte
counts, total transfer limits and completion status (S7). Odd byte counts use
a safe final word; no overruns or unbounded polls. DMA, overlap/tagged commands
and BIOS retry are not required by this optical gate.

**[F4]** Required packets: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ
CAPACITY, READ(10), READ TOC/session information, and qualified START STOP
UNIT/PREVENT ALLOW MEDIUM REMOVAL for explicit eject. Read data blocks as
2,048-byte Mode 1/Mode 2 Form 1 user data, not assumed 512-byte ATA sectors.
Use a 2-second ordinary command deadline and a 10-second explicit spin-up/eject
deadline; record failures outside IRQ locks. Expected no-medium/not-ready or
unit-attention sense is a media condition, not automatically a controller
fault. A failed read is not blindly retried: sense classifies it, and a new
readiness/mount transaction requires a valid controller state. Transport
timeouts, malformed phases or unproved reset quarantine the controller and
report consequences for its ATA peer. No post-command BIOS fallback.

**[F4]** Mount the selected data session read-only through the common cache,
keyed by device, media generation, block size and LBA. ECMA-119 Level 1–3
directory/file records, including multi-extent files, MUST be supported.
Validate descriptor sequence/terminator, both-endian numeric agreement,
record lengths, extent bounds and multi-extent ordering. Prefer a valid Joliet
supplementary hierarchy; decode `%/@`, `%/C`, `%/E` UCS-2 names big-endian
(S8), using the existing VFS namespace/case/path policy. Malformed advertised
Joliet fails visibly rather than silently hiding files in a different tree.
Rock Ridge, UDF, disc writing, audio-CD extraction/playback and an installer
are not added by this mount contract.

**[F4]** Tray/media change, including unit attention, increments generation
and invalidates cached blocks, nodes, cwd pins, DOS tokens and aliases before
reusing the drive letter. Existing handles fail `EIO`; they cannot read a
replacement disc. No medium reports unavailable with no ghost mount. Every
write/create/truncate/metadata mutation returns `EROFS` (DOS write-protection
mapping); read-only close/sync issues no media writes. Free space reports zero.
Public drive letters follow the existing VFS enumeration policy.

## NTFS read-only mount: driver and license

**[F4] Decision:** implement a Ciuki-specific `ntfs_ro` filesystem driver
under GPL-2.0-only, using S9's versioned GPL-2.0 layout/runlist sources as the
reference. This selects an independent read subset rather than importing
NTFS3's Linux VFS, journal or write machinery. No NTFS code is imported by
this draft. Any later reused source must be pinned to an exact upstream commit,
retain notices and enter the license inventory before compilation. GRUB's
GPL-3.0 NTFS code is excluded by the foundations contract.

**[F4]** Support clean NTFS 3.0/3.1 MBR volumes on qualified ATA storage:
boot geometry/record sizes, `$MFT` mapping, FILE/INDX update-sequence fixups,
resident and fragmented non-resident unnamed `$DATA`, bounded attribute-list
extensions, and `$I30` index-root/index-allocation/bitmap directory traversal.
Validate MFT reference sequence numbers and use `$UpCase` collation plus
UTF-16-to-UTF-8 conversion for lookup. Runlist lengths, signed relative LCNs,
VCN coverage, sizes, initialized length and partition limits must be checked
before reads; zero the uninitialized logical tail without exposing disk slack.
All walks have cycle/iteration and memory limits, returning errors on exhaustion.

**[F4]** Compressed, encrypted, sparse, reparse-point and alternate-stream
access remain unsupported and fail `EOPNOTSUPP` explicitly; never return raw
compressed/encrypted bytes as ordinary file contents. On-disk hard-link names
can refer to the same validated node, but no native hard-link creation API is
added. DOS short-name attributes are aliases, not duplicate independent files.
NTFS security descriptors are not a promise of Windows ACL enforcement under
the F2 single-user capability model. Existing path/component bounds apply;
native 64-bit offsets remain 64-bit, and DOS-unrepresentable values fail.

**[F4]** Dirty/recovery-required or detected hibernated volumes MUST be refused
with distinct reasons: no log replay, dirty-bit clearing, hibernation removal
or repair. Validate required metadata before exposing a mount; malformed
fixups/runlists/indexes return `EIO` and never trigger salvage writes. Read-only
MFT reads and unmount must issue zero block writes/flush-for-write requests.
All mutating VFS operations fail `EROFS`. Partition/mount generation and
shared DOS routing use the same rules as other host volumes. The driver owns
no private block cache; read workspaces are bounded and charged.

## Acceptance tests

These are future implementation gates, not results of authoring this draft.
**[F4]** Follow T0 → T1 → T2 → T3 → T4 in `test-architecture.md`, retaining
all earlier gates. One canonical kernel/apps/image per source state; runtime
selectors only. QEMU cases use declared profiles, one capped run at a time,
fw_cfg selection and disposable qcow2 overlays. Fixture data discs/NTFS test
volumes are nonboot test inputs, not copied/variant system images. Physical
T23/E500 use the same image/probes via boot menu/config, with verified
write/readback hashes. No probe success substitutes for an external observation.

| Tier / proposed suite | Required observable pass condition |
| --- | --- |
| T0 native audio/mixer | Production copied-stream/ring/gain/converter code: all formats/rates, mono/stereo, signed extremes, clipping after summation, exact phase across split batches, short/EAGAIN/EINTR/EFAULT writes, dup/inheritance/final close, quotas and generation rejection. Mock codec posted writes, CAS timeout/rollback, readback and volume/mute restoration; mock DMA wrap/prefetch/publication and failed stop with canaries. |
| T0 DOS audio | Reuse production f3-05 state with a fake output clock: 32 fresh 64-unit blocks, partial physical descriptors and persistent resampling; every advertised PCM mode, mixer/mute, OPL2/3 mode/pair/rhythm/routing and timers. Hold virtual IF=0/mask/in-service for 100 ms: DMA/time advance, pending causes remain correct, no forced IF or IRQ inflation; eligibility/shadow/ack/EOI and host heartbeat pass. Two chips/workers cannot share state; worker failure is local. |
| T0 bridge | Two DOS VMs/PSPs and a native client exhaust both opening orders of the access/share matrix, compatibility owner/attribute cases, aliases, exact/overlap/overflow locks and I/O conflicts. Interleave two DTAs per PSP, invalid tokens and snapshot visibility; dup/forced dup/no-inherit/failed EXEC/exit/fault baselines. Exercise every FCB operation, EXEC/overlay paths, cwd variants, Unicode/8.3 collisions, size errors, commit fault/crash reopening and shared raw refusal with zero guest-sector calls. Private FAT16 controls still pass. |
| T0 optical/NTFS | Production parsers on generated lawful fixtures, with independent file hashes/listings from host ISO/NTFS tools. Packet short/odd/multi-phase transfers, bad direction/count, sense/no-medium, timeout and ATA-peer serialization. ISO Levels 1–3/multi-extent/Joliet and malformed extents; NTFS resident/fragmented/attribute-list data, large `$I30`, aliases, initialized tail, >4-GiB offsets, fixup/runlist/index corruption, dirty/hibernated and unsupported features. Read-only mount/read/close/unmount and all attempted mutations issue zero writes, verified by block-call instrumentation and before/after hashes. |
| T1 image | Native ABI layout and kernel FPU/opcode audit; license/source pins and media fixture/payload manifests. No private commercial payloads. Device-control SDK capability matches actual service; absence is truthful. Fixture filesystem/hash checks precede boot tests. |
| T2 regression smoke | Existing `f0-smoke` plus earlier F1/F2/F3 regression gates on the same image; safe mode reports disabled optional engines and remains usable. |
| T3 `f4-audio-native` / `f4-audio-dos` | Declared AC97 twin at the 128-MiB reference and relevant desktop/laptop runtime profiles: known channel-separated tones/impulses and stereo mixes captured via deterministic QEMU WAV output; duration/pitch/RMS/dropouts match the digital reference within declared tolerance. Ten minutes mixed native + two admitted VMs (or explicit admission refusal) with disk/input/desktop load; no new FIFO errors, transport starvation or unexplained timing gaps. DOS DOOM/Wolf3D exercise original binary sound separately from native ports; record data/extender hashes and redistribution status. |
| T3 `f4-bridge` | Two DOS clients plus native client perform the full conflict/search/FCB/EXEC/raw-refusal matrix against shared FAT and read-only optical/NTFS volumes. Successful commit survives client fault/cold reopen; surviving desktop/VM heartbeat and page/handle/lock baselines remain correct over 100 normal/fault lifecycle cycles. |
| T3 `f4-optical` / `f4-ntfs-ro` | Canonical image plus declared small fixture media: verify hashes/listings and refusal cases through actual ATAPI/VFS paths; switch/eject/reinsert media and stale handles fail `EIO`. An ATA peer read proceeds correctly before/after CD traffic; injected transport faults report quarantine without BIOS retry. All RO write counters remain zero and host before/after fixture hashes agree. |
| T4 T23 AC97 / E500 ESS | Fresh measured PCI/BAR/route/codec identity, five cold boots and five restarts, startup/native/DOS audible playback, left/right isolation, mute and reboot persistence. Ten minutes concurrent audio/storage/input/desktop load, independently timed impulses ≤60 ms under the defined low-latency conditions, stable pitch and no observed repeated blocks/dropouts; report counters and operator/capture evidence. Run 100 start/stop/VM-fault cycles and a controlled backend-stop fault; no freed-buffer DMA or resource leaks. T23 results do not qualify ESS, and QEMU cannot close either analog gate. |
| T4 T23/E500 storage/media | On each machine with an identified working optical drive, read independently hashed ISO/Joliet files, eject/change media, verify ATA peer and stale-handle behavior. Read a controlled expendable NTFS fixture volume and verify its unchanged hash on the host afterwards; bridge conflict/FCB/EXEC tests run against shared FAT. Missing drive/fixture/operator evidence is `not_run`, never hardware PASS. |

**[F4]** Each case MUST record image SHA-256/build identity, probe/version,
run/sequence, target/profile, usable/reserved/free RAM and audio commitment;
device/subsystem/revision, codec/accepted rate, period/lead, source/output/fetched
counts, SB block units/rate/format, pending/delivered/late IRQs, FIFO/transport/
source starvation, maximum worker/IRQ/service gaps, quarantine and cleanup
baselines. Bridge cases record operation, owners, expected/actual errors and
content hashes; media cases record generation and write counters. Failures,
unsupported and missing prerequisites are separate outcomes. Bound serial
records and defer logging so evidence acquisition cannot itself empty the
audio ring. WAV captures and other investigation artifacts are analyzed then
removed under retention rules; retain a compact validation record, not raw
physical dumps. Software timing and unexplained hardware stalls remain distinct.

## Open questions for lead review

- **[F4] Cross-contract approvals:** reconcile the native device-control ABI,
  worker capability/deadline scheduling and proposed sharing matrix with
  their owning contracts before any code. This draft supplies the policy;
  it does not silently amend those files or assign new syscall numbers.
- **[F4] Physical qualification:** refresh T23 codec/mute path and E500
  engine/clock/route/codec data; establish stop proof, latency tolerance and
  actual audio memory peaks. Historical unsupported ESS output stays
  unsupported until that gate passes. Do not raise memory limits from
  installed-RAM totals or presumed chipset properties.
- **[F4] Generic audio coverage:** measured desktop PCI inventories are
  missing. The lead must name the first Intel ICH desktop variants to
  qualify; other physical audio families need a separate directive.
- **[F4] Optical inventory and MMC:** identify each physical drive/firmware
  and packet capabilities. Complete direct MMC retrieval/cross-check for
  any commands beyond the SFF-8020i read subset before implementing them.
  Missing hardware prevents closure of the corresponding T4 gate.
- **[F4] NTFS fixtures:** pin lawful generated NTFS 3.0/3.1 fixtures and
  independent tools covering attribute-list, large-index and malformed
  cases; do not infer full NTFS support from a single small clean volume.
- **[F4] Acceptance tolerances:** the implementation directives must pin
  deterministic PCM/OPL references and numerical digital tolerances,
  physical capture uncertainty and observed codec drain allowance before
  qualification. The 60-ms budget is a proposal requiring lead approval
  and hardware evidence, not an already measured result.

## Interfaces required from other contracts

| Owner | Required interface or reconciliation before F4 implementation |
| --- | --- |
| `foundations-transition.md` | Confirm F4 backend order and independent NTFS read subset; retain reuse/license inventory and deferred optional acceleration. No retirement without its replacement gate. |
| `execution-abi.md` / `posix-subset.md` | Freeze audio/mixer device-control numbers, record bytes, fd inheritance/nonblocking/cancellation/errors; delegate master/OPL capabilities at bootstrap, schedule bounded OPL work without public elevation, preserve integer kernel and user FPU isolation. |
| `boot-memory.md` | Constrained pinned ICH/ESS staging/descriptor allocation, charged native/VM/worker commitments and quarantine accounting under measured admission/headroom; no revised quotas or high-memory policy. |
| `device-firmware-ownership.md` | Sole controller/PIC/DMA ownership, validated shared INTx, elapsed deadlines and generation-safe stop; backend cursor/rate/drain/error contract, firmware-input/thermal dependencies and safe-mode disablement. No guest or competing BIOS ownership. |
| `dos-dpmi-contract.md` / f3-05 | Authoritative virtual PIC and IF/shadows, per-VM DSP/mixer/DMA/OPL state and timestamps, block-boundary continuation/yield/ack hooks, copied guest memory, PSP identities, launch drive capabilities and fault cleanup. Every inherited `[F4]` bridge requirement is specified above and tested in the bridge rows. |
| `vfs-storage-contract.md` | Adopt the sharing/compatibility/lock/destructive-operation matrix; expose DOS attributes/times, JFT/SFT descriptions, snapshot search tokens, stable aliases, cwd/free-space/commit/error operations. Provide bounded settings persistence/durability, media generations, variable block-size cache and read-only driver operations. Review the FCB/legacy integer overflow mappings before code. |
| `hardware-baseline.md` | Add measured audio and optical identities without per-machine images; declare an AC97 runtime test twin separately from the existing non-audio profiles. E500 requires T4 ESS evidence. |
| `test-architecture.md` / `f0-acceptance.md` | Allocate F4 selectors and bounded records, explicit prerequisite/unsupported outcomes, waveform/fixture checkers and physical observations; retain one-image, serialized-resource, hash-provenance and cleanup rules. The lead runs QEMU/hardware and records the approved contract/decisions in the development diary on integration. |

## Lead review and decisions (2026-10-11)

Reviewed by Claude (lead) on 2026-10-11 against `foundations-transition.md`,
`dos-dpmi-contract.md`, `device-firmware-ownership.md`,
`vfs-storage-contract.md` and `test-architecture.md`. The draft is adopted
as the F4 contract. Decisions on its open questions:

1. **Backend order.** Intel ICH AC'97 first, with the QEMU AC97 (82801AA)
   twin as the protocol reference; ESS Maestro-2E (`ess1978`) second, after
   the common mixer gate, qualified only on the E500. Approved.
2. **Native ABI.** The device-control records and their numeric assignments
   (`AUDIO_*`, `MIXER_*`) are frozen by the first F4 implementation
   directive through amendments to `execution-abi.md` and `posix-subset.md`
   before that directive's code; no syscall number is repurposed.
3. **Sharing and locking matrix.** The matrix and the compatibility-mode
   policy above are adopted; the bridge directive incorporates them into
   `vfs-storage-contract.md` as its first step, before bridge code.
4. **Latency budget.** The 60 ms low-latency budget, the 256-frame quantum
   and the eight published periods are provisional qualification targets;
   T4 measurements on the T23 and the E500 confirm or revise them in a
   diary entry, never by silently changing a guest's rate.
5. **Generic desktops.** The first Intel ICH variants to qualify are the
   82801AA (ICH), 82801BA (ICH2) and 82801DB (ICH4) AC'97 controllers, the
   common on-board audio of 1999–2003 boards; other physical audio families
   need their own directive.
6. **Optional 2D acceleration** (S3 Savage, ATI Mach64) stays deferred and
   disabled; the T23 copy failure remains an open item outside F4.
7. **NTFS.** An independent read-only `ntfs_ro` driver under GPL-2.0-only,
   informed by the versioned NTFS3 layout sources, is approved; no import
   of NTFS3 code.
8. **Ordering with F3.** The native audio API, mixer and AC'97 backend, the
   ATAPI/ISO 9660 mount and the NTFS read-only mount do not depend on F3
   and may be implemented in parallel with it; the DOS VM audio path and the
   DOS-to-VFS bridge follow f3-05 and f3-00.

## CD audio transport (F5-12B2, lead amendment 2026-10-20)

The binding decision in `docs/directives/f5-12b-mci-codecs-avi-cdaudio.md`,
amendment 1, selects READ CD as primary and PLAY AUDIO MSF as explicit fallback.
The packet references and repository comparison are in
[the CD audio validation record](../validation/f5-12b2-cd-audio.md).

`/dev/media` retains its fixed node identity. Version-1 `MEDIA_CD_STATE`,
`INFO`, `TOC`, `READ_AUDIO`, `TRAY`, `PLAY`, `PAUSE` and `POSITION` records
carry size, optical slot and (where applicable) media generation. STATE
finds a drive without a mounted letter; INFO reports Audio Play, CD-DA read,
stream accuracy and volume-level capabilities from MODE SENSE page 2Ah.
TOC reports up to 99 track descriptors with Q CONTROL data bits and lead-out.
Zero generation is permitted for discovery (INFO/TOC/TRAY); streaming and
analog commands require the exact generation. A stale generation returns
EAGAIN; no medium returns ENXIO (the native ABI has no ENOMEDIUM). The CD read
checks the generation again under the shared F4 channel mutex after waiting
for its ATA peer. No failed drive read is retried in the kernel.

Audio-first media are ready for CD audio; their `audio_only` flag distinguishes
an entirely audio disc from audio-first mixed media. They are never mounted
or sent to a filesystem probe. Data-first mixed media keep the ordinary F4
mount, with data/audio packets serialized on the same channel. A read or
play range touching any data track is EINVAL before issuing the read/play.
READ CD requests use CD-DA expected sector type and only the 2352-byte user
data field, at most 27 sectors (63504 bytes). The devctl uses a transient,
per-call bounce allocation, frees it on every path and copies to user space
after releasing the transport mutex. Allocation refusal occurs before IO.

Each playing MCI CD device owns a reader thread and four 27-sector owned
waveOut buffers: 108 sectors, 254016 bytes, 1.44 seconds at 1x. The source is
44100 Hz S16LE stereo through the F5-12A process mixer. Packets stay within
one track. The reader never holds the MCI guard over blocking extraction;
stop/seek/new play invalidate its epoch and discard late completions. Close
detaches the device; the in-flight reader retains its buffers and descriptor
until it returns. Allocation pressure shrinks future read requests. An
underrun emits mixer silence and holds the stream position; it never skips
or duplicates audio samples. Digital position follows consumed source frames
(588 stereo frames/sector), not READ SUB-CHANNEL or submitted packets.

The digital state machine is stopped -> playing -> paused -> playing ->
stopped. End-of-range completes successfully after the mixer consumes every
header; stop/seek/close abort pending notification; medium change or loss
completes with failure. A recoverable extraction failure can transition to
analog at the consumed sector only if Audio Play is supported. No-medium,
stale generation, invalid ranges and transport quarantine cannot trigger a
successful analog continuation. Without stream accuracy there is no blind
realignment/retry. The analog state machine uses PLAY AUDIO MSF, PAUSE/RESUME,
STOP PLAY/SCAN and READ SUB-CHANNEL; completion and position come from the
drive. Audio routed through a physical CD/codec connection remains a hardware
qualification requirement for this fallback.

`system.ini` section `[CiukiCDAudio]`, keys `CD0` through `CD3`, selects the
path per optical slot: 0 automatic, 1 forced digital, 2 forced analog. Each
play/fallback writes the selected path and reason to the native diagnostic
log. The internal shell element `cdaudio!@N` selects an audio-only slot
without inventing a drive letter; ordinary MCI clients use `cdaudio` or a
mounted drive such as `d:`. AutoPlay offers one CD player action for audio-first
media and launches the authored `CDPLAYER.EXE` through the existing Win32
launcher. The player and the guest fixture use the public MCI APIs.

MCI supports open/close, play/stop/pause/resume/seek, status, set time format
(MSF, TMSF, milliseconds), door, info, capabilities and notification. TMSF
track lengths are MSF values, preserving Quake's length-to-TMSF end conversion.
Opening the tray is refused while playing or paused. Mounted media also pass
the existing open-handle/eject preflight.

| Native condition | MCI result |
| --- | --- |
| EINVAL / data track / invalid time range | MCIERR_OUTOFRANGE (282) |
| ENODEV / absent slot | MCIERR_DEVICE_NOT_INSTALLED (306) |
| ENXIO / medium absent; EAGAIN / changed generation | MCIERR_DEVICE_NOT_READY (276) |
| ENOMEM | MCIERR_OUT_OF_MEMORY (264) |
| EBUSY / door blocked | MCIERR_DEVICE_LOCKED (288) |
| EOPNOTSUPP / unsupported command | MCIERR_UNSUPPORTED_FUNCTION (274) |
| EIO / other packet or transport failure | MCIERR_HARDWARE (262) |

Sense 2/3A expires the TOC and maps to ENXIO; 6/28 invalidates the generation;
5/64 and 5/21 map to EINVAL at the CD devctl boundary; media/read/loss-of-stream
errors remain EIO for the request. The F4 quarantine rules remain in force.
CUE/BIN fixtures are authored synthetic PCM, never commercial media. The
host adapter understands AUDIO, MODE1/2048, MODE1/2352, stored INDEX 00/01
pregaps and unstored PREGAP. The kernel does not parse CUE files. QEMU ide-cd
only qualifies data-only refusal/tray behavior; audio listening requires the
lead's Red Book test disc and T23 evidence.

F5-12B host continuation, 2026-10-21: the existing `cd_bench.c` model measured
2850 ns copying plus 2256704 ns in the production mixer per audio second,
0.225955% host thread CPU versus the historical 0.18% estimate. This excludes
ATA PIO, drive latency and scheduling, and cannot qualify target CPU cost or
audible playback. The approved transport choice is unchanged. The command,
position, notification and cancellation sequence of the synthetic CD model
is pinned in `tests/host/win32/media/traces/cdaudio.txt`, alongside the existing
waveaudio/sequencer and AVI traces; its PCM is checked independently sample
for sample. Exact commands and sandbox limitations are in
`build/codex/f5-12b-report.md`.
