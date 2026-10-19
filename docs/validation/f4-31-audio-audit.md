# f4-31 audio pipeline audit

The f4-31 four-period/≤30 ms policy below is historical. The
[F4-32 amendment](#host-evidence-and-lead-decision) supersedes it with eight
periods and a ≤50 ms nominal VM target.

Research completed before implementation, 2026-10-19. Baseline: worktree
`wt/f4-31`, commit `6fcaf761a5a0`. Physical measurements below are the directive's
T23 observations, checked against the read-only neighboring checkout's
`legacy/local/physical/9af46936/BOOT.LOG` (SHA-256
`45dda7eba93972f87211eceeb66a672f5a365c742659f1af2d8c43e2e0332474`).
These are not new hardware measurements. No QEMU or hardware run is authorized.

## Primary sources and baseline gaps

| Source consulted | Relevant behavior and repository comparison (baseline lines) |
| --- | --- |
| Intel [ICH3 datasheet](https://www.intel.co.kr/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf), §§5.18, 13.2.1–7; [ICH7 AC'97 programmer's reference](https://www.intel.la/content/dam/doc/manual/io-controller-hub-7-hd-audio-ac97-manual.pdf), chapter 2 (searchable reference; PDF retrieval failed) | BDL has 32 entries; CIV/PIV are modulo indices, LVI publishes prepared descriptors. DCH alone does not prove exhaustion; CELV, PICB and the software frontier must agree. FIFOE and LVBCI are distinct. `ac97.c:362–440` already validates these ownership rules; start is unnecessarily tied to exactly eight periods. Keep stop/reset proof and quarantine. |
| Intel [AC'97 2.3 component specification](https://hands.com/~lkcl/ac97_r23.pdf), §§5.7–5.8, register 26h | Fixed 48 kHz is baseline; variable rates are optional. Codec readiness, DAC/analog readiness and amplifier power are separate. `ac97.c:245–310` already distinguishes these; no speculative codec/power programming is warranted by a stopped transport. |
| Linux v6.12 [intel8x0.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c), setup_periods/update/pcm_trigger | IOC, position accounting and `snd_pcm_period_elapsed` connect completion to service. STOP proves DCH before reset. Ciuki already wakes `audio.changed` in `ac97_irq` (`ac97.c:627–643`). `audio_dev.c:300–333` instead primes silence over multiple wakes; recovery is restricted to proven exhaustion and other errors disable audio permanently. |
| ALSA [PCM interface](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html), state machine, latency, recovery | Buffer and period are different; xrun recovery prepares/restarts, while device removal is a hard error. `audio_dev.c:575–596` only wakes on write; it does not establish RUN before returning. Status (`abi.h:786–807`) exposes transport occupancy but no total latency. |
| PulseAudio author's [timer scheduling design](https://0pointer.de/blog/projects/pulse-glitch-free.html) | Buffer capacity, operating fill and scheduling margin are separate choices; latency competes with tolerance of scheduling delay. This is a design reference only. Ciuki retains period IRQ wakeups plus a deadline. Eight 256-frame periods (`audio.h:11`) cannot cover the observed gap. |
| DOSBox-X [mixer.cpp](https://raw.githubusercontent.com/joncampbell123/dosbox-x/master/src/hardware/mixer.cpp) and [source description](https://github.com/joncampbell123/dosbox-x/blob/master/README.source-code-description) | Audio rendering follows emulation time and incremental source updates, independently of host callback size. `vm_source.c:577–602` anchors to the VM timeline; `audio_vm.h:13` targets 56 ms, already above the new 30 ms goal. Preserve f3-40 DMA/IRQ progression and never consume future guest DMA to manufacture a larger lead. |
| ESS [Maestro-2 datasheet SAM0056-102997](https://www.alsa-project.org/files/pub/datasheets/ess/DSMaestro2.pdf), pp.12–21, 25–31; Linux [es1968.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/es1968.c) | ESS uses indexed APU/WaveCache routing, not Intel BDL. The available vendor PDF describes ES1968, not a complete ES1978 programming reference; the upstream ES1978 implementation is the register reference for the plan below. `backend.c:320–343` has no ESS backend. No ES1978 register writes are introduced. |
| Creative [Sound Blaster hardware programming guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf), chapters 3, 4, 6 | Physical DSP auto-init DMA uses block interrupts and word counts for 16-bit DMA. `drivers/sb16.c` and `audio/backend.c` already implement the physical path, independently of VM SB16. Keep firmware resource assignments, DMA boundary and IRQ acknowledgement checks; no PIC programming. |
| Intel [HD Audio specification 1.0a](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf), §§3.3, 4, 7 | CORB/RIRB codec verbs, stream BDL/CBL/LVI, LPIB and stream interrupts require a separate engine. Intel AC'97 support does not qualify HDA. `audio_backend.h:7` has no HDA backend. |

Other baseline gaps: `mixer.c:16–22,61–65` uses linear interpolation for every
fractional native sample; `player.c:167–195` decodes/writes 512-frame chunks
and can queue behind a full device ring; `main.c:39` displays mixer consumption
as audible position; `audio_dev.c:189–199` suppresses lifecycle logs while
running; `hardware_check.c:255–263` runs only a one-second tone.

## Implementation decisions

Keep 256-frame periods: 5.333 ms at 48 kHz, 5.805 ms at 44.1 kHz. Use eight
periods for native playback and four when a VM is attached. Start in one
bounded service, with a synchronous write-side kick serialized with the worker.
An attached, paused VM is not an active source. A fully silent deep tail can
be stopped/reprimed on source arrival, without waiting behind its silence.
Recover proven exhaustion immediately; recover other readable runtime errors
only after stop/reset proof. An unsafe stop remains quarantined, never retried
through hardware whose DMA ownership is unknown.

The hardware allocation remains 32 × 256 frames (8192 frames); adaptive lead
uses existing staging storage. The 2048-frame `AUDIO_RING`/caps ring is the
per-stream source queue, not the physical BDL's total allocation. No memory
quota or DMA allocation limit is raised.

The physical maximum gap is 134 ms (113 ms observed refill gap, 134 ms display
stretch). Required lead is `ceil(gap_us * rate / (256 * 1000000)) + 2` periods:
28 at 48 kHz, 26 at 44.1 kHz. Idle silence uses that margin even with a paused
VM attached. An active late service grows its session lead to cover the measured
gap (at least the 134 ms physical budget), bounded by the 30-slot common safe
frontier. Refill stops after 1 ms of work, reserving a second millisecond for
one quantum and bookkeeping; incomplete work is retained and woken again.
A single conversion exceeding 2 ms is still counted as a deadline miss;
buffer policy cannot impose a real-time bound on an unbounded CPU stretch.
Hardware stop has its
own existing bounded deadline. IRQ context still only observes/acks/wakes.
An inactive attached VM does not run its mixer for the idle cushion: the worker
copies silence. On real data arrival its source re-anchors to its own clock.

**The directive's simultaneous worst-case budgets are incompatible.** A
single FIFO cannot preserve 134 ms of already rendered sound and promise a
new source reaches DAC within 30/60 ms. Four periods plus 304 source-slack
frames give VM nominal latency 27.667 ms at 48 kHz; eight periods plus one
player period give native nominal latency 48 ms. After adaptation to 28
periods, latency is at least 149.333 ms. Status and protected records expose
this explicitly. Restoring both targets under full load needs scheduler
evidence that active service gaps stay below the low-latency lead, or a
separately reviewed mutable future-mix design. Do not claim that buffering
alone eliminates this conflict or that recovery undoes an audible underrun.

Use four-tap integer Catmull–Rom interpolation for native 44.1→48 kHz, retaining
the rational linear path for other ratios and an explicit low-cost fallback.
Use a Q10 phase and bounded 32-bit Horner arithmetic; no floating point or
i686 wide multiply/divide helpers. Hold one historical sample, clamp missing
edge taps, reset history on format/reset. A measured conversion over 106 µs
selects the linear fallback for this boot, with a protected transport record;
the measurement conservatively includes VM mixing or preemption in that turn.
Measure optimized period cost; host
timing cannot certify the T23's ≤2% CPU limit (106.7 µs per 256-frame period).

`AUDIO_GET_STATUS.latency_frames` uses the former final reserved output word,
keeping version 1 and size 112. Units are output frames: native queue converted
to output rate plus outstanding transport frames plus the advertised drain
allowance. `transport_frames` remains DMA occupancy. Player audible position
subtracts transport and drain from consumed frames; prefill is at most one
256-frame period, with no startup threshold.
With an empty source queue, latency/EOF instead use its last contribution's
output position, excluding subsequent sources and the deep idle silence tail.

## Backend contract and remaining driver plans

All backends implement the common `audio_backend_*` interface: prepare/init,
publish, start/resume of a prepared generation, stop/halt with DMA ownership
proof, monotonic position snapshot, period IRQ ack/wake, master and shutdown.
Resume after xrun means stop/reset/reprime/start, not reuse of stale slots.
Cursor uncertainty never permits rewriting CIV/PIV or freeing DMA storage.
Existing ICH, ES137x, physical SB16 and VIA engines share this lifecycle.

ESS ES1978/E500 plan (permitted instead of a driver by this directive):

1. Match `125d:1978`, validate firmware BAR/IRQ and claim resources without
   changing firmware-first input or PIC state. Use ES1978 branches of
   `snd_es1968_chip_init`, `snd_es1968_ac97_read/write` and PCI PM save/restore;
   do not substitute ES1968 initialization solely from its vendor PDF.
2. Port the upstream `apu_set_register`, `wave_set_register`,
   `snd_es1968_playback_setup` and trigger/cursor operations. Register sources:
   ESM_INDEX/DATA 02h/00h, WC_INDEX/DATA/CONTROL 10h/12h/14h,
   ESM_AC97_INDEX/DATA 30h/32h, ring-bus 34h–3Ah; indexed APU current/end/loop
   and mode definitions come from that same upstream file. Use a stereo APU
   pair, one contiguous region below the 28-bit limit within a 4 MiB window.
3. Map `snd_es1968_bob_start/stop`, interrupt and `snd_es1968_get_dma_ptr`
   into period service and coherent position; silence proof before ready,
   bounded busy waits, stopped APU/cursor proof before release. Host fixtures
   must cover stereo ordering, pointer wrap, timer ack, timeout and quarantine.
4. Qualify E500 codec routing/clock/output and ten-second cadence on hardware.
   QEMU has no Maestro-2E twin; ES1370 cannot supply ESS evidence. No complete
   ES1978 vendor programming manual was found in the consulted public sources.

HDA/2004 desktop plan: identify class 0403 and validate BAR/IRQ; use GCTL reset
and STATESTS codec discovery, CORB/RIRB verb timeouts, enumerate converter,
pin and amplifier path; allocate aligned BDL/position/CORB/RIRB storage under
the controller DMA mask; configure one PCM output stream (format/tag, BDL,
CBL/LVI), ack BCIS/FIFOE/DESE, map LPIB to monotonic position; prove RUN clear
and stream reset before release. Host fixtures precede the QEMU `intel-hda`
and codec twin; physical analog routing is a separate gate. This is a plan,
not a claim of HDA support.

AC97 suite cases and ES1370/SB16 runner device selection already existed.
This change adds native ES1370/desktop-2002 and SB16/desktop-1998 suite cases,
with 44.1 kHz rate, backend identity, page-accounting, WAV and service-budget
checks. VM predicates use four periods/1328 frames and a 1440-frame nominal
maximum lead; the 2 ms service gate applies to all cases. These are unrun
QEMU specifications, checked on the host for argument/predicate consistency.
Host models cover lifecycle, adaptive lead, latency and recovery across these
engines; the lead runs the twins with the same image, then T23/E500 hardware.
The H page's Audio 10 s check records frames, underruns, IRQ delta/cadence and
lead range through the real backend, retaining the boot tone's ordinary master.

## Validation evidence

Commands, results and optimized before/after timings are recorded in
`build/codex/f4-31-report.md`. The 2501-case f3-40 DMA/IRQ waveform oracle is
unchanged in scope and retains its original seven-period test cushion for
40 ms preemptions; it passes. The nominal VM harness uses three queued periods
ahead of its current period. The shorter timeline offset changes the two
timed F4 callback capture digests to `a0d729c4` / `5f7e8482`; sample-by-sample
OPL/44.1-kHz references and the independent f3-40 waveform checks still pass.

The MP3's reported 248 ms open / 315 ms first decode precedes `write()`.
This directive fixes transport startup and output buffering, not that file
access/decoder initialization time. The player's decoder also converts to
the negotiated output rate; the kernel's cubic path applies to native streams
which actually submit 44.1 kHz to a 48-kHz backend. The status drain allowance
is a conservative 20 ms, not a measured codec propagation delay. It can make
the reported total exceed the nominal DAC-model latency; it must not be used
as evidence that the physical 60/30 ms gates passed.

No result here qualifies an untested physical
backend, proves analog DAC delay, or substitutes a host CPU for the T23.

## F4-32

Research before implementation (2026-10-19): Intel's
[ICH3-M datasheet 290716-001](https://datasheet.lcsc.com/datasheet/pdf/7d520883a0f21f1845e4892c5243848a.pdf?productCode=C3247608),
§13.2.2–7, specifies CIV, software LVI publication, fetch completion/DCH,
PICB and PIV. Preserve the existing ownership checks and halt/reset proof.
Linux v6.12 [intel8x0.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c),
`snd_intel8x0_pcm_pointer`, retries zero PICB and retains its last position
on a reload race; an uncertain position must never retire a period.

The directive's suggested QEMU ordering is not the upstream ordering:
[QEMU v9.2.0 ac97.c](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/audio/ac97.c),
`transfer_audio` lines 993–1010, advances CIV/PIV and fetches the next BDL
entry **before** `update_sr` raises completion IRQ. Its callback accepts
available output capacity, not a fixed 10 ms PCM quantum.
[audio.c](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/audio/audio.c),
`audio_validate_opts`, defaults the timer to 10000 µs. A host fixture holding
zero PICB for an entire timer interval is an adversarial handoff model,
not evidence that QEMU exposes that state to guest services.

Implementation decision: progress and publication are separate operations.
Keep the last committed retirement during `-EAGAIN`, permit publication into
provably free slots using independent ownership checks, and block the worker
for at most 1 ms before retrying. Keep the 2 ms cursor deadline and 100 ms
stuck-progress bound; do not accept a 10 ms persistent zero cursor as normal.
Recovery must preserve the next unrendered VM frame across silence priming.
Host replay results, lead arithmetic and stretch attribution follow below.

### Host evidence and lead decision

**Amendment 1 (lead decision, 2026-10-19): nominal lead 8, ≤50 ms at 48 kHz.**
`8 × 256 + 304 = 2352` frames / 48000 = **49.0 ms**, replacing the f4-31
≤30 ms target. The original missed-service budget remains covered:
`480 + 256 + 256 = 992` frames, or `256 + 256 + 256 = 768` at ICH3's
5.333 ms IRQ cadence. That budget alone cannot bound callback capacity.
One 1024-frame capacity callback exhausts four periods before any worker
turn; eight periods leave 1024 frames (21.333 ms) of transport margin.
The lead reports intact pcm8 on three QEMU profiles with image `2fc3c429`
(lead 8), and startup `published=4 retired=3` halts on all three with image
`1fa1e62e` (lead 4). This evidence selects eight, rather than extrapolating
a capacity bound from QEMU's nominal 10 ms timer.

Primary-source recheck for this amendment:
[QEMU v9.2.0 audio.c](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/audio/audio.c),
`audio_rate_peek_bytes`/`audio_rate_get_bytes` (lines 2123–2155), computes
elapsed virtual-clock demand minus bytes sent, bounded by available capacity;
[ac97.c](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/audio/ac97.c),
`po_callback`/`transfer_audio`, can consume several descriptors per call.
No register, ownership or deadline rule changes follow from this policy.
At 44.1 kHz, eight physical periods plus the 48 kHz source floor take
`2048/44100 + 304/48000 = 52.773 ms`; the ≤50 ms nominal target is for
48 kHz, and slower backends still need separate physical qualification.
The 113 ms T23 gap continues to require adaptive growth to 28 periods.
Measured gaps/recovery may exceed the nominal target and remain exposed.

The three VM suite transport predicates now require 8 periods / 2352 frames.
The 1024-frame increase shifts the existing latency-relative `lead_min`
window to 2164–2364 and the `lead_max` lower bound to 2264. Its maximum
uses the same nominal-time rule as before: `50 × 48 = 2400` frames
(formerly `30 × 48 = 1440`). Other predicates, including lag, deadlines,
fresh blocks and exact PCM frames, are unchanged.

| Host replay | Before | After |
| --- | --- | --- |
| 100 callbacks of 480 frames / 10 ms, coherent cursor | 187 IRQs, published 191, retired 187, halts/deadlines 0 | Lead 8: 187 IRQs, published 195, retired 187, halts/deadlines 0 |
| Free committed slot, transitional progress | Published 4, retired 1, fetched 256; no source consumed or publication | Lead 8: published 9, retired 1, fetched 256; exactly one quantum published |
| Worker after transitional progress | Blocks 4 ms | Blocks 1 ms; coherent progress restores 4 ms policy |
| Startup recovery, VM clock advances 127 frames before first render | 32 blocks, fresh 31, PCM output 12161, source frames 2027, late 21 | 32 blocks, fresh 32, PCM output 12288, source frames 2048, late 0 |

The source-loss replay uses the production renderer and a deterministic
f3-05 queue boundary; it reproduces the old counter pattern, not the original
QEMU execution schedule. Mid-stream, retained-quantum and 28-period adapted
lead recovery also pass;
48 kHz output matches the uninterrupted PCM sequence sample for sample after
excluding inserted silence. At 44.1 kHz every source frame/block is retained;
interpolation at inserted silence edges is not asserted sample-identical.
All cases retain exactly 12288 VM timeline frames; at 44.1 kHz the physical
nonzero-frame counts differ by at most two due to those interpolated edges.
Common cyclic-backend fixtures cover ES1370/1371, SB16 and VIA686/8233,
including refusal of possibly owned slots and a real 1 ms worker block.
Persistent uncertainty still expires after 2 ms and increments the deadline
counter. Neither a zero cursor nor a time bound is committed as retirement.

The **zero-at-IRQ hypothesis is not confirmed as the QEMU failure cause**.
Both coherent and adversarial short handoffs work at the 10 ms cadence.
A different legal callback input, 1024 frames requested in one burst, gave
exactly `published=4 retired=3 halts=1 deadlines=0` in the first delivery,
with no intervening publication or `-EAGAIN`. Amendment 1 adds two replays:
1024 frames at 21 ms with lead 8 must leave the engine running with no halt,
underrun or deadline; 2048 frames at 43 ms must exhaust the frontier once
and recover with silence, no source discard and no deadline miss. The
production-renderer companion checks full-lead startup recovery at 48/44.1
kHz: 32 fresh blocks, 2048 source frames, 12288 VM frames, late/skips zero,
and sample-exact 48 kHz PCM after excluding inserted silence. The transport
fixture and source-boundary fixture are separate host models. Neither runs
the full DOS scheduler or qualifies the original suite on a new image.

### Stretch attribution and qualification limits

The available old record has `eip=c017df83` on revision `287551c6`; the
neighboring retained ELF is revision `2277ee3a`, so resolving that address
against it would give an invalid attribution. `cpu_stretch_snapshot` returns
the historical maximum task/stretch: the transport's `task=probes` is not
the current worker's stack or a timestamp for that stretch. The old run's
15 µs maximum IRQ-off section excludes its stats/info copies as the source
of a single 66 ms IRQ-off wait; it does not exclude non-preemptible kernel
work with IRQs enabled.

F4-32 adds deferred `case=stretch_attribution` maxima for write enable,
VM create, boot call, each typed key, `dosvm_text`, stats/info copies and
block checks. These are wall times including callee sleeps, not exclusive
CPU costs. Prompt scans yield every 64 entries, block checks every 16,
typing after each completed key, and completed observations/transactions
have explicit preemption points. Existing wait loops still sleep.

The exact 76/66 ms section remains **unattributed** until that record runs.
Calls outside this directive's authority requiring lead review are
`src/kernel/vm/dosvm.c:388–419` (`dosvm_text`'s transfer/model loop) and
`src/kernel/vm/dosvm_xms.c:129–138` (eager pool allocation/zero/map without a
preemption point; the probe's LOW_CAP configuration makes that pool zero,
so it cannot explain this probe's stretch). `dosvm_create` already yields
after each structure/low-page initialization. Storage write enable reaches
the full-volume scan (`src/kernel/fs/mount.c:472`); its inner FAT/cache work
already calls `fs_service`, and timing evidence is needed before blaming it.
No assertion is made that the remaining paths stay below 10 ms on T23.

`tests/host/audio/run.sh` passes (ASan/UBSan, optimized costs, i686 codegen).
The full kernel host script passes the VM audio and unchanged 2501-case
DMA/waveform oracle, then fails at
`tests/host/dosvm_speed_test.inc:138`, `disk_records==disk_before+10`.
Relinking the same harness with the original `vm_source.c` reproduces that
failure. Kernel compilation/FPU audit, API generation and `make CAP= apps`
pass. Full commands and logs are in `build/codex/f4-32-report.md`.
There is no new image, QEMU result or hardware qualification.

Amendment 1 verification (2026-10-19): the audio host suite passes with
11217537 production checks, both new capacity bursts, full-lead startup
source recovery at 48/44.1 kHz and unchanged i686 codegen checks. The
1024-frame/21 ms burst has zero halts, underruns and deadlines. The
2048-frame/43 ms burst recovers once with zero deadlines and grows the lead
to 28 through the existing measured-gap policy. Kernel build/FPU audit,
API generation and `make CAP= apps` pass. A structured suite comparison
confirms only the authorized VM lead/latency values changed. The broader
host script again passes the 2501 DMA cases and unchanged timed digests,
then stops at the same lead-owned `disk_records` assertion; that assertion
is untouched. Logs and the before/after table are in the report's
"Amendment 1" section. All jobs ran sequentially at `nice -n 19`.
