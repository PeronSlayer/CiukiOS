# f6-19: entropy on the T23, NetSurf launch and menu record

Status (2026-10-22): host fixtures pass; **T23 confirmation pending** (no
image, QEMU or hardware run in this delivery). A host model reproduces the
recorded numbers; it does not qualify the T23's entropy.

## Evidence (1.0.0 alpha, image `8f59e2217bca`, serial capture)

```text
[entropy] boot rounds=2644 cycles=335678959 ms=335
[entropy] sources=0 irq_mask=0000 estimate_bits=0 initialised_ms=0 health=fail reseeds=0
[console] Unable to initialise framebuffer        (after netsurf#39/#40 start)
```

TSC rate: 335,678,959 cycles / 335 ms = 1,002,026 cycles/ms (the boot-only
PIT measurement); 126,959 cycles per measurement round on average.

## Diagnosis: entropy

Round arithmetic for the pinned jitterentropy 3.7.0 (sources under
`build/external/jitterentropy-library-3.7.0`, `src/jitterentropy-base.c`,
`-noise.c`, `-health.c`):

* `jent_entropy_init` power-up test: 1 priming + 100 cache-clearing + 1,024
  (`JENT_POWERUP_TESTLOOPCOUNT`) = **1,125** rounds of the combined noise
  source. A failure there stops at <= 1,125 rounds (no internal timer is
  compiled), so this test **passed**.
* With `JENT_FORCE_FIPS` the collector then runs the NTG.1-style startup: the
  memory-access stage, then the SHA-3 hash-loop stage, each sampled alone and
  each needing `roundup3((256 + 65) x OSR)` = 963 non-stuck samples at OSR 3,
  plus one extra round per stuck sample. A complete startup is >= 1,125 +
  1,926 = 3,051 rounds; **2,644 < 3,051**, so a startup health alarm ended the
  allocation after 1,519 startup rounds.
* On that alarm `jent_health_failure_reset` raises OSR, hash loops and, since
  `max_mem_set` is not yet recorded during startup, the memory size to the next
  step (512 KiB). The adapter refused any allocation above 256 KiB, so every
  re-initialisation for OSR 4..20 returned `EMEM` before measuring and the
  collector allocation returned NULL: one alarm was fatal, without the
  documented OSR+1 recovery. Boot then latched `health=fail` because jitter was
  the only credited source.
* Most consistent split (the T23 cannot be re-run here): memory stage 963 +
  465 stuck = 1,428 rounds (a third of the deterministic stride-walk intervals
  repeat with interrupts off), then a constant SHA-3 interval: every sample
  after the first is stuck and the RCT intermittent cutoff (30 x OSR = 90
  consecutive stuck samples) fires on round 91; 1,125 + 1,428 + 91 = 2,644.
  A Pentium III executes an L1-resident hash loop in a fixed number of cycles
  when no interrupt can arrive (boot runs before the PIT starts); the
  combined source passes because its pseudo-random memory walk varies. The new
  `[entropy] jitter` record (stage, alarm, rounds per stage, refused size)
  confirms or corrects this split on the next T23 boot.

`tests/host/entropy/t23_test.c` drives the actual library and adapter with a
clock calibrated to these numbers and reproduces them exactly:
`rounds=2644`, `ms=335`, stages 1,125/1,428/91, RCT alarm in the SHA-3 stage at
OSR 3, a refused 524,288-byte request. Raising the OSR with interrupts still
off keeps failing until the 5 s budget (OSR 12 reached), which is why boot no
longer escalates. With interrupt edges (about 2 kHz, 1.5-7.5 us service)
landing inside the measured intervals, the runtime attempt passes at OSR 4 in
883 ms and eight reads seed the pool in 1,303 ms of model time.

## Change

See [kernel entropy Amendment 4](../design/kernel-entropy.md#amendment-4-f6-19-2026-10-22-two-credited-sources-on-the-t23):
the adapter follows jitterentropy(3) (`jent_entropy_init_ex` with the
collector's osr/flags, OSR+1 re-allocation, `jent_read_entropy_safe`);
a jitter failure retires the collector, not the pool; the worker retries
with interrupts live; interrupt timing is credited at Linux's rate (one bit
per 64 edges, at most 128 edges per 10 ms drain).

## Expected T23 records

Boot (if the split above holds; the jitter record states the actual stage):

```text
[entropy] jitter attempt=boot result=-1 osr=3 alarm=0001 stage=sha3 alarm_osr=3 round=2644 rounds=1125/1428/91/0 refused=524288 expired=0 state=retry
[entropy] boot rounds=2644 cycles=<about 335.7 M> ms=<about 335>
[entropy] sources=0 irq_mask=0000 estimate_bits=0 initialised_ms=0 health=ok reseeds=0 jitter=retry irq_bits=0
```

Runtime, interrupt credit first (PIT alone: 16,384 edges, about 16 s; with
device interrupts sooner). `irq_mask` lists the PIC lines that fired, at least
bit 0 (PIT), typically also 1 (keyboard), 4 (UART), 11 (AC'97/USB/NIC), 12
(mouse) and 14 (ATA):

```text
[entropy] jitter attempt=runtime result=<0 or error> osr=<4..> ... state=<ready|retry>
[entropy] sources=2 irq_mask=<nonzero, e.g. 5813> estimate_bits=256 initialised_ms=<about 5000-17000> health=ok reseeds=1 jitter=<retry|ready|failed> irq_bits=256
```

If the runtime attempt succeeds first, the first reseed record shows
`sources=3` (both sources) and an earlier `initialised_ms`. Acceptance on the
T23: `health=ok`, `sources>=1`, `irq_mask!=0000`, `reseeds>=1`, and HTTPS in
NetSurf no longer shows "Secure connections are not ready yet" after that
record. `health=fail` remains only for a machine without a validated TSC.

## NetSurf launch and menu record

`[console] Unable to initialise framebuffer` is NetSurf's framebuffer
frontend dying when `framebuffer_initialise` gets -1 from `nsfb_init`
(NetSurf 3.11 `frontends/framebuffer/gui.c` `main` and `framebuffer.c`
`framebuffer_initialise`, in the pinned
[netsurf-all-3.11 release](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz)
of `config/netsurf.json`; its `docs/using-framebuffer.md` describes the
libnsfb surface chosen with `-f`, which here is the Ciuki surface). The Ciuki
surface's `initialise` refused when no `--channel-fd` had been parsed, and the
desktop passed that argument only to programs it knew as graphical;
`/Programs/netsurf` was graphical only on the GATE_BROWSER test route.

* The NetSurf record (`apps/desktop/catalog.h` `desk_applications`: name
  `netsurf`, label "NetSurf", description "Web browser", Tango 0.8.90
  `internet-web-browser`) makes it graphical on every route; the gate-only
  flag is removed.
* The surface uses the launcher's fd 3 when no `--channel-fd` is given (the
  SDK convention of `sdk/samples/HelloWindow`); a non-channel fd fails HELLO.
* NetSurf is in the Programs root, not Developer; its window keeps the
  browser icon when the page title replaces the caption (by launch identity).
  The icon store already holds the archive's 16, 22 and 32 px PNGs and the
  derived 48 px image; no artwork was added or redrawn.

Expected on the T23: one click on Programs > NetSurf opens the window within
3 s, no `Unable to initialise framebuffer` line.
