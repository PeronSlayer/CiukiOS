# Independent VM clocks and frame delivery

The October 3 gameplay probe reached 33.30 game tics/s over ten seconds,
with frame-loop intervals up to 65 ms. This is a loop measurement, not a
display FPS measurement. The remaining work concerns timer delivery and
unnecessary VGA translation invalidations; the desktop/cache fixes remain.

## Sources checked before implementation

* [QEMU i8254](https://raw.githubusercontent.com/qemu/qemu/master/hw/timer/i8254.c):
  channel reload, latching and periodic deadlines are independent of delivery.
* [KVM PIT](https://github.com/torvalds/linux/blob/master/arch/x86/kvm/i8254.c):
  timer expiries accrue separately from an interrupt awaiting acknowledgement.
* [Intel 500 series PCH, 8254 section](https://cdrdv2-public.intel.com/635218/635218-008.pdf):
  modes 0, 2 and 3, count latch and reload semantics.
* [IBM VGA reference](https://bitsavers.org/pdf/ibm/pc/cards/IBM_VGA_XGA_Technical_Reference_Manual_May92.pdf):
  display-start registers form one address; hardware latches it at retrace.
* [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  software clearing accessed/dirty bits must invalidate cached translations.

## Implementation decision

While the VMM owns IRQ0, physical PIT channel 0 supplies a fixed 200 Hz host
service clock. Each VM instead has its own channel-0 programming and elapsed
TSC time, including time spent outside that VM. Timer debt is consumed only
after one virtual request has been queued, with no second request while IRQ0
is pending or in service. The Jemm virtual PIC owns guest arbitration and
EOI; the physical IRQ is acknowledged exactly once by its physical handler.
HDPMI uses the same held-device interrupt delivery path for virtual IRQ0.
Guest BIOS reflections no longer increment the physical scheduler clock.
Parking the sole desktop restores its channel-0 programming and releases
physical port access (the inert resident handlers are removed on unload). The two-host-tick scheduling quantum remains two ticks;
multiple VM progress is a required runtime check.

VGA direct mappings retain dirty bits until a mapping change, frame event,
or periodic harvest. Repeated writes that cannot change the mapping do not
walk its PTEs or flush the TLB. A display-start write requests presentation
only if its value changed. This removes duplicate unchanged-byte requests;
it is not a claim of full scanline/retrace emulation or atomic arbitrary
two-byte register updates. Damage polling remains for in-place drawing.

The changes have completed the bounded checks below. These measure the
game loop and VM behavior; they do not measure physical display FPS.

## Integration checks

The first bounded runtime booted and launched a new game, but measured only
4.6 render-loop iterations/s and failed normal exit. Diagnosis: upstream
HDPMI `I2FHDPMI.ASM` has `NUMTRAP equ 8`; the added PIT range was the ninth
range including VGA, so registration failed and the game changed physical
PIT0 to 140 Hz while receiving the model's default 18.2 Hz IRQ0. The source
now groups keyboard ports 60h–64h into one range (intervening ports still
use physical device I/O), registers PIT first, and treats failed PIT
registration as an attachment failure. This is a failed integration check,
not an accepted performance result. Evidence: `build/tests/game-performance-fix-2026-10-03/runtime-1`.

The slot limit is in the pinned [HDPMI I2FHDPMI.ASM](https://github.com/Baron-von-Riedesel/HX/blob/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/I2FHDPMI.ASM).
The correction also preserves the chosen VM slot before reading the creator's
CMOS index in `vmm_create`: the old sequence overwrote EAX before saving the
slot number. The second-VM check below exercises this path.

## Completed validation

All jobs ran sequentially. Full builds used a systemd scope capped at 3 GiB
RAM, 1 GiB swap and one CPU equivalent. QEMU used a separate scope capped at
768 MiB RAM, no swap, two CPU equivalents and 32 tasks, with a 180-second
probe deadline. The guest was the same Linux QEMU/KVM Pentium III profile,
one vCPU, 256 MiB RAM, standard VGA and a snapshot disk. No full guest-memory
dumps or unbounded traces were produced. QEMU is closed.

* PIT/device model: 1,215 assertions passed with ASan/UBSan, followed by
  freestanding OpenWatcom compile/link. Tests include multiple expiry debt,
  one-shot behavior, PIC ownership, count/status read-back, mode-3 count,
  interleaved read/write latches, keyboard/mouse and sound model coverage.
* Clock wrapper: 10 checks passed with ASan/UBSan, including independent VM
  time, fractional accumulation, a gap over 200 ms, reset and 64-bit elapsed
  time division. VGA mapping checks passed against the real VGA model.
* The second full build passed. CVSESSION's source manifest matches the final
  sources; the new model and wrapper link without C runtime dependencies.
* `runtime-2` started a new Doom game (`-warp 1 1`), measured ten seconds of
  rotation, opened a second COMMAND.COM VM, executed `echo FAIR VM2`, closed
  only that VM, restored game focus and exited Doom with F10/Y. Doom advanced
  from gametic 602 to 1199 during the 17.357-second second-VM sequence.
  `clock_enabled` was zero after the game exited: physical tick ownership was
  released back to the desktop.

| Measurement | Previous build | Final build |
| --- | ---: | ---: |
| Game simulation tics/s | 33.2984 | 34.3936 |
| Render-loop starts/s | 33.2984 | 34.3936 |
| Observed loop-counter interval, p95 | 42.27 ms | 36.10 ms |
| Single-increment intervals over 42 ms | 18 | 6 |

The game-rate increase is 3.29%. Original Doom's nominal simulation rate is
35 tics/s. These counters do not establish physical monitor FPS or prove that
all visual stutter has disappeared. The final probe missed one intermediate
counter value on two occasions: those observations contain two increments,
so their 56–62 ms gaps must not be reported as individual frame times. Among
single-increment observations, the maximum was 45.57 ms (previously 65.24 ms).
The polling interval is approximately 3 ms. Quantiles use inclusive linear
interpolation; small differences from the earlier rounded summary are below
the sampling interval.

`shared->live` stayed stale during protected-mode gameplay in this run;
its zero deltas and old text-mode geometry in the raw report are ignored.
Only the directly sampled game counters support the table. Input was sent
through the QEMU monitor, not a physical SDL window. AC97 was active with a
silent host backend; this was not a listening test or a Windows runtime test.

Evidence and derived comparison:
`build/tests/game-performance-fix-2026-10-03/{runtime-2,comparison.json,artifacts.json}`.
The unchanged original Doom core SHA-256 is
`799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
The final full image SHA-256 is
`398011d7525786f25e66ac5a60bd74263fa6f4714220df1e381dc3f8d55c13f8`.

The Windows portable ZIP was refreshed and its CRC integrity checked during
packaging. An independent read of its FAT16 image verified that all eight
private game/application paths are absent, all 28,091 free clusters are zero,
and CVSESSION/HDPMI match the tested Linux image. Windows execution remains
untested. ZIP SHA-256:
`6427c786cb3356b3e7354ad3a07ca22ebac543c9d0cb76ff6455b6deccbf1783`.
