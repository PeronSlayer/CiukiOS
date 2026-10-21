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
  describes timed volatile memory work, first/second/third differences, stuck
  rejection and oversampling. Ciuki's small collector is inspired by that
  method; it is **not** the upstream jitterentropy implementation or its
  validated entropy source. Its noise workload retains Clang `optnone`, as
  required by [upstream noise.c](https://raw.githubusercontent.com/smuellerDD/jitterentropy-library/master/src/jitterentropy-noise.c).
  It requires a working TSC, rejects zero timing
  differences through third order, and credits one bit per 64 accepted samples
  after 1,024 startup samples. Initialization/reseed requires 256 credited bits.
  This conservative engineering estimate is conditional on actual CPU timing
  noise; host output tests cannot establish target min-entropy. Raw/restart
  assessment on the supported hardware remains a lead qualification item
  before claiming TLS entropy qualification.
* [NIST SP 800-90B](https://doi.org/10.6028/NIST.SP.800-90B)
  supplies continuous repetition-count and adaptive-proportion tests. Tests
  operate on raw delta low bytes before conditioning or stuck rejection.
  RCT cutoff 41 and APT cutoff 410 in a 512-sample nonbinary window are the
  stricter H=0.5, alpha=2^-20 settings. Passing these health tests does not
  establish the entropy estimate. Failure latches until reboot, erases the
  generator and prevents all output. Startup samples receive no credit.
* [NIST SP 800-90A Rev. 1](https://doi.org/10.6028/NIST.SP.800-90Ar1)
  distinguishes DRBG state, instantiation and reseeding. The requested ChaCha
  construction is not a 90A-approved Hash/HMAC/CTR DRBG; no NIST validation is
  claimed. [RFC 8439 section 2.3.2](https://www.rfc-editor.org/rfc/rfc8439#section-2.3.2)
  supplies the block-function known-answer test.
* [OpenBSD arc4random(3)](https://man.openbsd.org/arc4random.3) uses kernel
  entropy and has no error return. SDK arc4random functions request kernel
  output in <=256-byte chunks, wait on EAGAIN, and terminate on other errors;
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
The worker offers a scheduler safe point, with its mutex released, before
each bounded jitter measurement; it sleeps after at most 64 samples per turn.

Boot collection is finite and happens at the existing global stack-guard safe
point, before task stacks exist. A missing TSC, failed health test or exhausted
boot sample budget leaves entropy unavailable for this boot. The guard retains
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
Health failure also refuses output. Records contain only source flags,
`estimate_bits`, `initialised_ms`, `health=ok|fail`, and reseed count, never
raw samples, keys or output. A worker allocation failure is logged, not fatal.
Source flags are 1 for CPU jitter and 2 for drained IRQ input; `irq_mask`
identifies contributing PIC lines. After readiness, `estimate_bits=256`
describes the established seed; before readiness it is the pending estimate.

`getentropy(NULL, 0)` succeeds. Requests validate length/unused registers,
prepare the user range and copy at most 256 bytes; EFAULT never bypasses
readiness. The SDK `ciuki_getentropy_wait` retries only EAGAIN, sleeping 10 ms
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
