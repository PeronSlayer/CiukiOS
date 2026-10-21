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
