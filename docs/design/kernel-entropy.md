# Kernel entropy and native status (f6-16, 2026-10-21)

The ABI addition is `getentropy` (91). Per lead Amendment 1, syscall 90
remains reserved ENOSYS; status uses the existing read-only `/dev/net` query.
No browser or network configuration policy changes belong to this delivery.

## Sources and decisions

* [Linux random.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/char/random.c)
  separates IRQ fast pools, conditioning, readiness, reseeding and ChaCha key
  erasure. Ciuki uses its existing SHA-256 implementation to condition a pool,
  and a new RFC 8439 ChaCha20 block implementation. Device identity, RTC, PIT
  ticks and the old lwIP SplitMix state never receive cryptographic credit.
* [Müller, CPU Time Jitter Based NPTRNG](https://www.chronox.de/jent/CPU-Jitter-NPTRNG.pdf)
  and [upstream v3.7.0](https://github.com/smuellerDD/jitterentropy-library/tree/v3.7.0)
  replace the original homemade collector per Amendment 2. The pinned archive
  and imported source inventory are recorded in `config/jitterentropy.json`.
  The 256 KiB memory workload and 8 KiB GCD history use owned PMM runs:
  the existing small kernel heap is limited to 2 KiB. Allocation failure
  returns unavailable; no heap/memory limit is raised. Runs are erased by
  the adapter and returned on collector retirement.
  Compile the upstream core, noise, GCD, health and SHA3 units with `-O0`.
  Preserve the pinned memory-access workload, GCD normalization, startup
  self-tests and RCT/APT/LAG tests with forced health enforcement (OSR >=3).
  This release uses SHA3-256 in its noise workload and SHAKE-256/XDRBG
  conditioning; retain the exact pinned implementation rather than reverting
  it to the earlier SHA3-only conditioner. Amendment 3 accepts this release's
  fidelity: [v3.7.0 CHANGES.md](https://github.com/smuellerDD/jitterentropy-library/blob/v3.7.0/CHANGES.md)
  records XDRBG-256 output generation and removal of loop-count shuffling.
  Do not reintroduce loop-count randomization. The upstream platform header is
  replaced by freestanding allocation, erase, TSC and scheduler hooks.
* [NIST SP 800-90B](https://doi.org/10.6028/NIST.SP.800-90B) and the
  [upstream assessment instructions](https://github.com/smuellerDD/jitterentropy-library/blob/v3.7.0/tests/raw-entropy/README.md)
  distinguish online health tests from target entropy assessment. Use the
  library's 1/OSR raw-delta rule, without changing its health cutoffs or
  safety factor. Additionally, Amendment 2 limits Ciuki pool credit to one
  bit per successfully delivered output byte: 256 library output bytes are
  required for the 256-bit pool seed, after successful `jent_entropy_init`.
  Failed/partial reads, startup data and IRQ mixing receive zero credit.
  T23 non-IID runtime and restart assessment remains **pending**; an upstream
  assessment or a host histogram does not qualify this port on hardware.
* [NIST SP 800-90A Rev. 1](https://doi.org/10.6028/NIST.SP.800-90Ar1)
  distinguishes DRBG state, instantiation and reseeding. The requested ChaCha
  construction is not a 90A-approved Hash/HMAC/CTR DRBG; no NIST validation is
  claimed. [RFC 8439 section 2.3.2](https://www.rfc-editor.org/rfc/rfc8439#section-2.3.2)
  supplies the block-function known-answer test.
* [OpenBSD arc4random(3)](https://man.openbsd.org/arc4random.3) uses kernel
  entropy and has no error return. SDK arc4random functions request kernel
  output in <=256-byte chunks, allow at most five seconds of EAGAIN per
  API call, and abort with a diagnostic on timeout or other errors;
  they never return weak fallback bytes. [getentropy(2)](https://man.openbsd.org/getentropy.2)
  specifies the 256-byte ceiling. Ciuki deliberately returns EINVAL for an
  oversized request and EAGAIN while unavailable, as required by f6-16.

## Execution and lifecycle

The current kernel is uniprocessor. One CPU-local fast buffer receives a
single TSC read and fixed integer mixing per genuine PIC interrupt, including
timer, PS/2, disk and NIC IRQs. There is no hashing, allocation, logging, health
test or worker submission there. A normal-priority entropy task drains the
buffer and conditions it outside IRQ exclusion. IRQ input receives zero
credit: timer drift, human input and disk seek/receive timing diversify the
pool without assuming independent or attacker-inaccessible events. An SMP
port must provide one buffer per CPU.
The worker offers a scheduler safe point before each upstream measurement,
with no pool mutex held. Combined-noise deltas span consecutive measurement
calls, so the yield and dispatched work contribute to the next measured delta.
Normally it collects only while fresh pool credit is below
256 bits, then stops collection until a reseed consumes that credit. Collection
errors latch failure, release the collector and retire the worker. IRQ input
can continue to be mixed without granting credit. The explicit raw-capture
mode below also collects while credit is full, only until its sample quota.

Boot collection is finite and happens at the existing global stack-guard safe
point, before task stacks exist. A missing TSC, failed health test or exhausted
boot time/round budget leaves entropy unavailable for this boot. The guard retains
its existing non-cryptographic fallback in that case. On success it is seeded
once from the DRBG; changing it later would invalidate suspended protected
frames. Initialisation occurs before PIT starts (`initialised_ms=0` means that
boot phase). Runtime workers never rewrite the guard. lwIP switches to DRBG
seed material once available; its legacy per-boot mixture remains only while
entropy is unavailable. No state flows from lwIP or the guard into this pool.

Each output block reserves its first 32 bytes for the next key and returns
only the other 32 bytes, erasing the old key/block. Fresh 256-bit estimated
input is required at 60 seconds or 1 MiB of output, whichever occurs first.
If unavailable, reads return EAGAIN without modifying the caller's buffer.
Health failure also refuses output. Normal records contain source flags,
`estimate_bits`, `initialised_ms`, `health=ok|fail`, and reseed count, never
keys or output. Raw records are explicitly opt-in (see below). A worker
allocation failure is logged, not fatal.
Source flags are 1 for CPU jitter and 2 for drained IRQ input; `irq_mask`
identifies contributing PIC lines. After readiness, `estimate_bits=256`
describes the established seed; before readiness it is the pending estimate.

`getentropy(NULL, 0)` succeeds. Requests validate length/unused registers,
prepare the user range and copy at most 256 bytes; EFAULT never bypasses
readiness. The SDK `ciuki_entropy_wait` retries only EAGAIN, sleeping 10 ms
between calls; it is an explicit potentially indefinite wait, interruptible
through the existing nanosleep API. `getentropy` itself never waits.

The SDK `ciuki_net_status(struct ciuki_net_state *)` helper uses the existing
read-only `/dev/net` NET_GET_STATE operation. It initializes version/size and
zero reserved fields, obtains the existing 96-byte snapshot, and closes the
fd on success or failure, preserving the query errno. `present`, `link`,
`state`, `address_configured` and `ipv4` permit offline classification before
DNS. No new status record, syscall operation, DNS-server ABI, mutation,
persistence, applet or probe is introduced. These local observations do not
promise Internet reachability.


## Amendment 2: bounds, capture and verification

Boot runs before PIT interrupts and the normal TSC calibration. A private
10 ms PIT channel-2 one-shot measurement establishes a boot-only TSC rate
([Linux PIT calibration](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/tsc.c)).
It leaves the speaker disabled during measurement and restores port 0x61;
channel 0 and the global calibration remain untouched. Calibration has a
finite polling bound and rejects a stopped TSC or invalid PIT completion.
This is a boot-local measurement and requires lead hardware confirmation;
it does not assert calibration accuracy across a CPU power-state change.
Collection is capped at 5,000 ms and 131,072 upstream measurements, including
startup/retries; budget checks occur inside library loops. Failure closes the
source for this boot. Runtime collection uses the existing monotonic ticks
and the same per-attempt bounds. `[entropy] boot rounds=... cycles=... ms=...`
records the attempt count and elapsed collection cost (including startup).

## Amendment 3: post-boot raw timing stream

The exact `BOOT.CFG` token `entropyraw=1` requests **1,048,576** post-boot
samples from the normal collector. Without it there is no capture allocation
or raw serial output. Boot startup/guard seeding never streams raw samples.
The worker allocates a 131,072-byte buffer (33 owned PMM pages including its
allocator header), enough for every measurement in one bounded attempt; there
is no whole-corpus allocation. Allocation failure stops only capture.

The existing observation hook saves the low eight bits of each GCD-normalized
combined-noise timing delta, before health tests, stuck rejection and
conditioning. Zero and rejected samples are retained in order. After filling
fresh credit (or one read when already full), the worker streams the buffer
in serial-only records:

```text
[entropy] raw index=0 count=64 data=<128 lowercase hexadecimal digits>
[entropy] raw index=64 count=64 data=<128 lowercase hexadecimal digits>
...
[entropy] raw complete samples=1048576 bits=8
```

`index` is the zero-based sample offset; `count` is 1–64 bytes and `data`
contains exactly two hex digits per byte. Frames are shorter than 224 bytes
including the newline. They bypass console and disk log sinks. Publication
uses existing evidence queue backpressure, without the pool mutex held; the
existing UART IRQ transmits. The worker sleeps until its batch's final ticket
has been submitted to the UART, then starts a new bounded collection attempt.
Wire submission does not prove remote receipt: the lead must verify every
offset, count and the completion record in the captured file.

The sample quota truncates only the final batch, with no wraparound/restart.
After the completion record drains, the raw buffer is erased/freed and normal
saturation/pause behavior resumes. Serial refusal/disconnection frees capture
storage and reports an incomplete stream without failing the DRBG. Collector
failure retains the existing fail-closed policy, retires the worker and frees
capture storage. The DRBG, credit ceiling, reseed demand and syscall path stay
active throughout capture; full fresh credit grants no additional entropy.

Serial waits and other scheduled work fall between consecutive timer readings
and therefore affect subsequent deltas; the capture has that measurement
perturbation. No serial I/O occurs in the observation hook. The 5 s/round
collector bounds apply independently to each attempt, excluding the later serial
drain. A complete corpus takes minutes at 38400 baud. This exposed-noise
diagnostic mode makes no TLS qualification claim. The lead's capture/parser
and NIST assessment procedure, including remaining runtime/restart evidence,
is [pending hardware qualification](../validation/f6-16-entropy.md).

`ciuki_entropy_wait()` is the explicitly indefinite, interruptible helper.
The existing `ciuki_getentropy_wait()` name is retained as a compatibility
alias. `arc4random*` uses a single monotonic 5 s EAGAIN deadline per API call,
including all chunks/rejection draws, then prints a diagnostic and aborts.
Interrupted sleeps retry against that deadline; permanent syscall/clock errors
also abort. No timeout path returns weak bytes.

The kernel host gate uses deterministic ChaCha, upstream health, synthetic
clock/collector, pool lifecycle, deadline and syscall fixtures. Measured host
jitter and output histograms run separately with
`tests/host/entropy/run_qualification.sh`; source availability is reported,
never a prerequisite for the deterministic host gate.

## Amendment 4 (f6-19, 2026-10-22): two credited sources on the T23

The 1.0.0 alpha T23 boot logged `[entropy] boot rounds=2644 cycles=335678959
ms=335` and `health=fail`: the jitter source failed and, being the only
credited source, latched the pool failed, so `getentropy` never succeeded.
The diagnosis and the expected T23 records are in
[f6-19 validation](../validation/f6-19-entropy-t23.md). This amendment
supersedes the f6-16 rules "IRQ input receives zero credit" and "collection
errors latch failure" (Amendment 2); every other rule above stands.

### Adapter: the library's documented use

* [jitterentropy(3), v3.7.0](https://github.com/smuellerDD/jitterentropy-library/blob/v3.7.0/doc/jitterentropy.3):
  `jent_entropy_init_ex(osr, flags)` takes "the osr and flags parameters which
  should be identical to the subsequent invocation of
  `jent_entropy_collector_alloc()`"; after a health failure "the entropy
  collector instance MUST be deallocated and a fresh instance must be
  allocated", increasing the OSR "at least by one"; under FIPS 140-3
  considerations "it is recommended that the function
  `jent_read_entropy_safe()` API call is used". The adapter now calls
  `jent_entropy_init_ex(osr, JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER |
  JENT_MAX_MEMSIZE_256kB)`, allocates with the identical arguments, retries a
  failed startup with a fresh instance at OSR+1 (up to the caller's bound,
  never above upstream `JENT_MAX_OSR` 20), stops at once for results an OSR
  cannot change (`ENOTIME`, `ECOARSETIME`, `ENOMONOTONIC`, `ESTUCK`, `EHASH`,
  `EMEM`, `EGCD`), and reads with `jent_read_entropy_safe`.
* Why the port's own recovery: with `JENT_FORCE_FIPS` the collector runs the
  NTG.1-style startup (memory-access stage, then SHA-3 hash-loop stage, each
  sampled alone, `jitterentropy-noise.c` `jent_random_data`). A startup alarm
  calls `jent_health_failure_reset`, which, because `max_mem_set` is only
  recorded after `_jent_entropy_collector_alloc` returns, asks for the next
  memory step (512 KiB). The 256 KiB adapter bound (unchanged; no memory-limit
  change) refuses it, so the library's reset fails without running a round and
  the allocation returns NULL. The adapter's OSR+1 loop keeps the pinned
  buffer size.
* The health parameters, cutoffs, safety factor, memory workload and the
  pinned source are unchanged; no source patch is added. The adapter registers
  `jent_set_fips_failure_callback` (before the first `jent_entropy_init*`, as
  the man page requires) only to record the first alarm of an attempt, and
  counts rounds per stage (power-up test, memory, SHA-3, output; the stage is
  read from `rct_mem_nosr` and `startup_state`).

### Kernel: jitter is optional, interrupt timing is credited

* Boot runs one attempt at the default OSR 3 (no boot delay beyond today's
  attempt). A failure retires the collector, not the pool: `health=ok`, and
  `[entropy] jitter attempt=... result=... osr=... alarm=... stage=...
  alarm_osr=... round=... rounds=P/M/S/O refused=... expired=... state=...`
  records the attempt.
* The worker retries 1 s after it starts (the PIT and device interrupts then
  perturb the measured intervals), resuming above the last OSR tried, then
  after 30 s and 120 s; four failed attempts, or a permanent health failure
  reported by the library (not this adapter's 5 s/131,072-round budget),
  retire jitter for the boot. Each attempt keeps the existing bounds. Runtime
  attempts run in the worker without the pool mutex and offer a preemption
  point every round.
* Interrupt timing credit follows
  [Linux random.c v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/char/random.c)
  (`add_interrupt_randomness`, `mix_interrupt_randomness`: the fast pool
  "feeds the input pool roughly once a second or after 64 interrupts" and
  credits `(count & U16_MAX) / 64` bits, clamped to the pool size): one bit per
  64 interrupt edges, without Linux's one-bit minimum per mix. The worker drains
  the CPU-local buffer every 10 ms and counts at most 128 edges per drain (four
  32-bit words, 32 rotation positions each), carrying the remainder below 64.
  The drained words are conditioned through the existing SHA-256 pool before
  any credit. No credit is granted before a TSC validated against the PIT at
  boot, while the fresh credit is full (edges are still mixed), after a pool
  failure, or for a drain whose newest edge carries the same TSC low word as
  the previous drain (a stopped counter). The IRQ path gains one store
  (`fast.last`); it still performs no hashing, allocation, logging or health
  test. Every PIC line contributes, PIT included, as in Linux.
* With the PIT alone (1 kHz) the 256-bit seed needs 16,384 edges, about 16 s;
  device interrupts (ATA, USB, AC'97, PS/2, NIC, UART) shorten it. The pool is
  `failed` only without a validated TSC (no credited source), as before.
* `source_ready` now means "a credited source is available" (validated TSC).
  Jitter credit stays one bit per delivered output byte; both sources share the
  conditioner, the 256-bit estimate and the reseed rules.
* The status record gains two trailing fields:
  `... health=ok reseeds=N jitter=off|ready|retry|failed irq_bits=N`
  (`irq_bits`: total credited interrupt bits). It is printed at boot, on each
  reseed, on a health change and on a jitter state change. Source flags remain
  1 jitter, 2 interrupts.
* The raw capture (`entropyraw=1`) waits through retries and stops with
  `reason=collector` only when jitter is retired.

## Amendment 5 (f6-21, 2026-10-22): jitter collection never starves anyone

QEMU (qemu-t23 profile, image 6b586080) failed f0-core `preempt` with
`max_wait_a=59 max_wait_b=59` ticks while `[cpu] top` showed
`entropy#10:375k375s57`: the f6-19 runtime retry ran inside the P_NORMAL
worker, beside the probe's two P_NORMAL spinners, and one of its kernel
stretches lasted 57 ms without a preemption point (the `s` field is the
longest stretch in ms). The worker yields only at round boundaries; the
kernel is not preemptible, so a long stretch of adapter work (buffer
zeroing/erasure, allocation) blocks every task of its level and below. This
amendment supersedes Amendment 4's "the worker retries 1 s after it starts"
and "Runtime attempts run in the worker"; every other rule stands.

Sources:

* Windows: priority 0 is reserved for "the zero-page thread ... responsible
  for zeroing any free pages when there are no other threads that need to
  run"; idle-class threads exist so they do not interfere with higher
  priority threads
  ([Scheduling Priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)).
  Dynamic boosts favour responsiveness
  ([Priority Boosts](https://learn.microsoft.com/en-us/windows/win32/procthread/priority-boosts)).
* Linux: `SCHED_IDLE` is "intended for running jobs at extremely low
  priority (lower even than a +19 nice value)"
  ([sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html)); the
  kernel's own jitter collector `try_to_generate_entropy()` calls
  `schedule()` after every sample
  ([random.c v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/char/random.c), line 1350).

Decisions:

* Two tasks. `entropy` (P_NORMAL) only drains interrupt timing every 10 ms
  and records state changes; it never waits for jitter, so interrupt credit
  keeps its cadence however long a jitter attempt takes. `jitter` is the
  collector: P_IDLE class with no configured level, i.e. scheduler level 0,
  the Windows zero-page-thread position. It is dispatched only when no other
  task is runnable; the scheduler arms the starvation timer only for P_NORMAL
  and grants no input/I/O boost to P_IDLE, so it is never boosted. A ready
  task above it (the audio refill, the desktop, any foreground task) sets the
  reschedule flag and is dispatched at the collector's next boundary.
  Priority inheritance through the pool mutex still applies (the mutex is held
  only to credit 32 bytes).
* Turns. At runtime every measurement round, and every 16 KiB chunk of the
  adapter's own zeroing and erasure of the 256 KiB buffer (plus one boundary
  before the pool allocation), is a boundary. The collector measures its turn
  in TSC cycles and sleeps one tick when the next round, predicted as the
  longest recent round plus an eighth, would take the turn past 1 ms
  (`ENTROPY_TURN_US`); otherwise it offers a preemption point. The prediction
  rises at once to a longer round and decays by a quarter of the gap per
  shorter round. A turn restarts after every pause or preemption. With the
  T23's 127 us rounds a turn holds 7 rounds (889 us); a round alone longer
  than 1 ms (QEMU TCG) makes one round per turn, the bound being then the
  round itself. The adapter's `point` callback is new and optional; the
  library, its patches and the health parameters are unchanged.
* Budget. The attempt's 5 s/131,072-round bound now counts the collector's
  own collection time at runtime (TSC cycles accumulated at boundaries,
  divided by the boot PIT calibration), not wall time: pauses and other
  tasks' work cannot expire an attempt, and the total wall time grows with
  the load. Boot collection (no scheduler) is unchanged: TSC since
  `entropy_boot`, no turns.
* First retry. A runtime retry also waits for the desktop: the first full
  frame the display owner presents (`grant_present`, the event that already
  completes the pager's bootstrap) calls `entropy_desktop_presented()`; so do
  the supervisor's stand-in, desktop-unavailable and desktop-stopped paths.
  Test and hardware-check boots launch no desktop and are ready at once. The
  1 s minimum after the worker starts remains.
* Evidence. The jitter attempt record gains four trailing fields for the
  collection session: `... state=<s> turns=N pauses=N turn_max_us=N
  round_max_us=N` (zeros at boot). The collector's CPU is idle-class time:
  it counts in `[cpu] idle_pm` and is not listed in `[cpu] top`, like any
  P_IDLE task; `turn_max_us` is its stretch evidence.
* Without the collector task (allocation failure) jitter is retired at
  start (`[entropy] collector=unavailable`), interrupt timing stays credited.

Host evidence: `tests/host/entropy/kernel_test.c` (turn policy, pauses,
budget clock, desktop gate, collector refusal), `jent_test.c` (adapter
boundaries), and `tests/host/jitter_collector_latency_test.c` in
`test_kernel_latency.py`, which runs the extracted turn policy on the
production scheduler and CPU sampler: the f6-19 shape (P_NORMAL, one point
per round, one 57 ms stretch) reproduces spinner `max_wait=58` ticks; the
collector keeps both spinners at `max_wait<=2` and runs no round beside
them; on a T23-like desktop (AC'97 refill IRQ every 10 ms, 2 ms desktop
frames every 16 ms) there are no audio-refill overruns and no boosts, the
refill waits at most one round (91 us measured) and turns stay under 1 ms.
