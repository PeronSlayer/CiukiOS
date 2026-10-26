# DOOM full-screen sound load (fx-doomfs2)

Research and implementation record, 2026-10-26. Directive:
`docs/directives/fx-doomfs2.md`. Host evidence only.

## Sources and implementation decision

- Microsoft Windows 98 Resource Kit, chapter 28, "Virtual Devices"
  (`wrkc28_3.htm`) and "Process Scheduling and Multitasking"
  (`wrkc28_4.htm`), read from the owner's local reference: each virtual
  device manages its own state and the scheduler shares the processor.
  Full-screen display ownership does not imply exclusive MS-DOS mode.
- Microsoft [Windows Virtual Device Adaptation Guide, section 16.5.5 and
  VPICD_Set_Int_Request](https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/):
  I/O hooks are per port; virtual IRQ delivery respects IF, masks and
  priority. The [Windows 95 DDK IRQ descriptor](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S34DA.HTM)
  retains EOI and IRET callbacks. These are VMM/VxD references, not NTVDM.
- Creative [Sound Blaster Series Hardware Programming Guide](https://www.scs.stanford.edu/05au-cs240c/lab/hardware/SoundBlaster.pdf),
  chapter 3 and command 1Ch: auto-init DMA generates an interrupt per DSP
  block; the application refills consumed data and acknowledges the DSP.
- id Software [DOOM doomdef.h](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/doomdef.h)
  defines 35 game tics per second. The DMX hardware timer can run faster;
  its IRQ rate must not be confused with the game tic rate.
- DOSBox Staging [OPL implementation](https://github.com/dosbox-staging/dosbox-staging/blob/main/src/hardware/audio/opl.cpp)
  models timer/status reads and their I/O delay separately from synthesis.
  Keep the existing device handlers and all reads/writes, rather than
  eliding the game's OPL delay accesses.

Repository comparison: `dosvm.c:lean_before` gates **every** lean exit on
`dosvm_vga_ports_modeled`. In a VGA full screen this is false, although
OPL, PIC, PIT and DMA remain emulated. A display-ownership condition thus
forces sound-related accesses and PM CLI/STI through the full monitor
housekeeping. Move that condition to the VGA branch of `lean_port`.
Keep the existing interrupt, single-step, stack-image, DMA-pin, scheduling
and full-screen transition obligations. No device timing or scheduler
policy change is justified by these logs.

## Trial attribution

Private evidence was read in place and is not copied here. T23 image
`5bb73dba`, DOOM VM generation 12642: serial L8091 identifies the launch;
L8327 enters full screen, first frame L8328, L8702 ends the session.
BOOT.LOG banks n=6 through n=12 cover steady full-screen play.

| Five-second bank | Guest us | Monitor us | Off CPU us |
| --- | ---: | ---: | ---: |
| n=6 (5001 ms) | 2539499 | 969195 | 1492306 |
| n=8 (5001 ms) | 2094167 | 1395683 | 1511150 |
| n=12 (5021 ms) | 1631794 | 1845759 | 1543447 |

All three banks have `lean=0`. OPL address/data at n=6: 13900/3892 exits,
278479/77267 us; n=8: 31206/8743 exits, 624968/174637 us. Serial L8559-8562
reaches 18150/5082 OPL exits and 483345 us for all I/O in a two-second
window. More sound traffic consumes time otherwise available to the guest.

Steady SB16: 430-431 blocks per five seconds (about 86 Hz, 128 units at
11025 Hz in the documented DMX profile; the rate inference alone does
not prove this session's DSP programming), no merged blocks, late acknowledgements or DMA underruns;
average IRQ latency 839-923 us, maximum 5.8-7.3 ms. PIT reinjections rise
from 29 to 32 over the session and `irq0_lost=0`; clock gaps are 1-4 ms.
This session does not demonstrate lost game timer ticks. The other games'
generations must not be attributed to DOOM. Host transport reports no
steady underruns; guest CPU starvation under growing monitor cost is the
supported diagnosis, not proven hardware mixer overload.

At full-screen entry the 670 ms park has 1482 DMA underruns and a 670733 us
IRQ maximum, followed by a source drop of 122 late frames. Those belong to
the transition, not to rocket spam in steady play. Earlier image c42f556e
shows the same transition effect (649 ms park, 1738 underruns), then clean
steady cadence. Scheduler snapshot: VM 676 permille, storage writer 72,
audio refill 55, desktop 51 and device work 48; maximum recorded stretch
2 ms, cumulative IRQ-off maximum 4240 us. BOOT.LOG n=6-12 keeps off-CPU
share near 30%; the changing cost is inside the VM monitor.

VM device-clock work does not grow with the sound-load rise: n=6 has
38001 us PIT / 30403 us audio work for 5001 turns; n=8 38189 / 30496 us;
n=12 37339 / 29767 us. Audio refill is about 55 permille in the CPU
snapshot. Per-buffer mixing time is not logged, so there is no measured
mixing-cost regression to justify changing the synthesizer or mixer.
`CIUKIST.LOG` adds no matching DOOM cadence records. The 2ca3b0b6 comparison
log has no matching DOOM cadence/session markers; it supplies no additional
DOOM timing evidence.

## Host replays

`dosvm_doomfs2_test.inc` replays the n=8 selected exit mix (42280 I/O,
4945 PIC/PIT, 7770 PM flags), with the production trap and handlers. It
prices ordinary exits at 19.946 / 17.728 / 18.072 us and lean exits at
6.6 us, the DOOM window's observed 6.0-7.4 us range. This is a fixed cost
model, not a host wall-time benchmark. The same test fails before the fix:

| Selected exits per 5 s | Before | After |
| --- | ---: | ---: |
| Total | 54995 | 54995 |
| Lean | 0 | 54995 |
| Selected monitor us | 1071401 | 362967 |

The scalar control remains 1071401 us after the fix. The test also checks
V86 and PM OPL register/status values and SB aliases, physical VGA routing,
full-screen transition holds, TF and reschedule refusal, DMA settling and
an IRQ arriving at an emulated OUT. The ordinary tail delivers that IRQ
exactly once.

The instruction runner adds `GUEST_T23_MODEL=doomfs2`, using the trial's
fixed hot-exit costs and about 30% off-CPU time with 4 ms stretches.
With `GUEST_T23_FULLSCREEN=1`, non-display port/flags traps see the real
full-screen flag, so this replay exposes the former blanket restriction.
VGA still uses the host fake card, charged as direct hardware access;
there is no physical aperture or native display in this runner.
`GUEST_ISR_ROCKETS=1` types IDDQD, IDKFA and 5 through the virtual keyboard
before holding fire and walking. The canonical sound config requests eight
SFX channels. The guest mixes those channels into one DMA stream; the
host does not create one audio source per SFX.

| Unified DOOM, 5 s rocket-fire replay | Before | After |
| --- | ---: | ---: |
| Model monitor ms | 1656.8 | 1389.8 |
| Executed guest instructions, whole run | 619512457 | 699665613 |
| Flips in measurement window | 142 | 148 |
| IRQ0 delivered / PIT edges | 700 / 700 | 700 / 700 |
| SB blocks / IRQ entries | 431 / 431 | 431 / 431 |
| Merged / late acknowledgements / lost PIT edges | 0 / 0 / 0 | 0 / 0 / 0 |
| SB entry average / maximum us | 876 / 19309 | 541 / 14764 |

Both runs pass the existing ISR cadence targets. IRQ0 is DMX's 140 Hz
timer, not 140 game tics per second (DOOM is 35 Hz). The game replay is
supplementary: its return-checkpoint count differs from the physical
trial, variable DOS services retain host-cost scaling, and it does not
render host PCM or prove audible hardware continuity. Use the fixed trace
for the measured exit-mix comparison, and the hardware for the final
subjective sound/game-speed verdict.

The existing unified shareware image was read in place for the 12 s audio
regression. Before and after are identical: 229323 traps, 24 flips,
1794 PM IRQs, 166146 SB16 units, zero unexpected CPU events or guest
faults. These are the current-base numbers; the directive's historical
233710 / 24 / 1806 / 167359 counters were not reproduced with the available
image and documented profile. They are not claimed as a passing exact
historical-counter check.

Host replay results and exact commands are recorded in
`build/codex/fx-doomfs2-report.md`.
