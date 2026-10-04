# Doom gameplay performance after framebuffer repair

The owner confirms on the same Linux QEMU profile that pointer lag during
Doom is now small and the desktop becomes responsive again after Doom exits.
The remaining complaint is gameplay fluidity. This follow-up investigates
that complaint; no OS code or build artifacts were modified in this turn.

## Sources and actual execution path

- [id Software's game timing definition](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/doomdef.h)
  sets 35 simulation tics per second.
- [The original game loop](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/d_main.c)
  calls `TryRunTics` before `D_Display` in normal play.
- [TryRunTics](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/d_net.c)
  waits for available simulation tics and increments `frameon` once per
  outer call. Its wait loop and 20-tic timeout also match the sampled DOS
  binary's instructions, although the published source is the Linux port.
- [QEMU's PC device documentation](https://www.qemu.org/docs/master/system/i386/pc.html)
  describes its emulated VGA/Bochs VESA device. The host's physical GPU is
  not directly exposed by the tested standard-VGA configuration.
- [QEMU VirtIO GPU](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
  and [Mesa VirGL](https://docs.mesa3d.org/drivers/virgl.html) describe a
  separate guest-driver/rendering stack for GPU acceleration. Adding that
  stack would not automatically accelerate an original DOS VGA renderer.

`src/com/testgame_launch.asm` executes `\APPS\DOOM\DOOMCORE.EXE` after
changing to its data directory. The window is hosted by DOSVM; its VGA state
is captured and converted through CVSESSION's retained BAND presenter.
The current scheduler uses `VMM_DESK_MS=8`; the 15 ms comment in `dosvm.c`
is stale. Native WC framebuffer copies and protected-mode direct VGA-plane
mapping are already active in this profile.

## Bounded observations

Each run used one QEMU/KVM vCPU, a 256 MiB guest, a snapshot disk, silent AC97
output and a systemd scope with MemoryMax=768M, MemorySwapMax=0 and
CPUQuota=200%. Runs were sequential, with a 180-second hard deadline. No
builds, broad filesystem searches, full-RAM dumps or unbounded traces ran.

Evidence directories under `build/tests/`:

- `game-performance-2026-10-03`: initial diagnostic read failed because
  the active HDPMI page table did not map the Jemm instance pointer. The
  VM closed in cleanup. Subsequent reads walk the saved Jemm CR3 explicitly.
- `game-performance-2026-10-03-2`: completed 10-second stationary sample.
- `game-performance-2026-10-03-motion`: completed rotating-gameplay sample
  and normal exit. A subsequent `-timedemo demo3` attempt did not produce
  completion within 70 seconds. It supplies no timedemo score and is not
  interpreted as a frame-rate result.
- `game-performance-2026-10-03-engine`: completed rotating-gameplay sample,
  direct engine-counter reads and normal exit. `artifacts.json` identifies
  the exact image, resident DLL and original Doom binary used.

The stationary interval observed approximately 32.2 retained captures/s and
61,416 protected-mode VGA port writes/s. Recorded BAND conversion/blit
cycles total about 0.231 seconds over a 10.001-second interval (2.3%). This
timer excludes DAMAGE capture, validation, other desktop composition and
native framebuffer copies; it is not the cost of the entire graphics stack.
The 10-second rotation interval gives about 2.2% for the same timed section.
These results do not identify it as the remaining dominant bottleneck.

Important counter limits:

- `display_changes` counts changed VGA display registers/modes, not game
  frames. The initial observed rate of roughly 35/s must not be called FPS.
- `shared->live` is refreshed by the V86 INT08 callback. Its one-second
  deltas can cluster because of refresh age; they cannot prove frame jitter.
- `source_rows_converted` resets at each retained capture; it is a gauge
  for the current captured image, not a cumulative throughput counter.

To avoid those ambiguities, the last run reads Doom's own counters from
the protected-mode page table. A 4 KiB code-page capture confirms the
`TryRunTics` loop at 0024F988h–0024FA29h. Its instructions identify
`gametic` at 002A23BCh and `frameon` at 002A1748h for this exact binary/layout.
These addresses are diagnostic observations, not OS assumptions or patches.

Over 10.000484 seconds of rotation:

| Counter | Start | End | Rate |
| --- | ---: | ---: | ---: |
| Simulation `gametic` | 214 | 547 | 33.30 tics/s |
| Render-loop `frameon` | 207 | 540 | 33.30 iterations/s |

Polling `frameon` every 3 ms observed all 333 increments individually.
Intervals between observations: median 29.36 ms, 95th percentile 42.26 ms,
maximum 65.24 ms; 18 exceed 42 ms and two exceed 57 ms. These include host
polling overhead and measure loop starts before waiting/drawing, not physical
monitor presentation. They show near-35-Hz progress with imperfect regularity.

In the preceding rotation sample, 22 of 48 instruction samples land inside
the same `TryRunTics` wait loop; another group lands in its `NetUpdate` path.
Thus the engine often waits for timing instead of continuously rendering.
This sampling is supporting evidence, not a complete CPU-time profile.

## Resulting implementation priorities

1. Give each DOS VM an independent PIT/IRQ0 timebase and deadlines based on
   elapsed time. Current slices are two physical IRQ0 callbacks, and missed
   IRQ debt counts delivered callbacks rather than reconstructing elapsed
   guest time. Audit late/coalesced timer delivery against Doom's 35-Hz
   clock before changing it; do not shorten the slice blindly or speed up
   simulation to conceal delayed delivery.
2. Measure and align completed game scanout, retained capture and visible
   page flip. Desktop wake-up polling and the engine are currently separate;
   near-35-Hz average production does not guarantee evenly shown frames.
   Preserve the repaired cursor ownership and fast post-exit behavior.
3. Reduce repeated VGA I/O/trap and direct-plane remapping work where measured
   useful. Tens of thousands of VGA writes per second are a concrete cost,
   but the present samples do not prove they dominate normal play.

There is no evidence here that a missing downloadable graphics driver is
the immediate blocker. Sustained original-game timing and frame regularity
come first. Rendering at 60/120 Hz with interpolation would additionally
require a suitable Doom engine path; the original DOS loop's 35-tic pacing
cannot be removed by installing a display driver.

## Implemented performance follow-up

The independent VM PIT, changed-only frame notification and VGA mapping work
was subsequently implemented and validated. See
[the integrated result](2026-10-03-independent-vm-clock.md) for the final image,
34.39 game tics/s measurement, concurrent second-VM check, successful game exit,
and measurement limits. This does not retroactively change the earlier
physical-input evidence or claim a new physical SDL/Windows test.
