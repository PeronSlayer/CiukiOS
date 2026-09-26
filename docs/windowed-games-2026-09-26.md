# Windowed games and DOS responsiveness — 26 September 2026

The DOS command window and two cooperative graphics source ports work in the
native CiukiOS desktop. This is a qualified increment, **not completion of the
requested hardware-accelerated, audible, general DOS windowing platform**.

## What changed

The original idle COMMAND window polled the desktop from IRQ0 at approximately
18.2 Hz. Its CPU was mostly idle, but the cursor could wait nearly one BIOS tick.
The blocking virtual INT16 reader now services the desktop after every interrupt
wakeup, including mouse IRQ12. Busy text applications retain the IRQ0 fallback.
The BIOS PIT frequency, RTC and keyboard/mouse drivers are not reprogrammed.

Text cells now use an opaque 8x16 blitter with native pixel stores and one clip
calculation per cell. Its code resides in the external DOSWIN module, keeping
SHELL within the existing COM memory bound. Font, attributes, cursor, clipping
and all pixel formats are independently compared against actual instructions.

`Programs > Games` includes **Windowed Doom** and **Windowed Wolf3D**, alongside
the existing full-screen launchers. `Run > Window` accepts `DWIN` or `WWIN`.
The wrappers restore the original working directory. Wolf's windowed settings
and saves go in `APPS\WOLF3D\WINCFG`, separate from the full-screen version.

The new binaries are GPL source ports of Doomgeneric and Wolf4SDL, not wrapped
original game EXEs. Their CPUs render 320x200 indexed pixels. A retained native
window displays an exact 2x image at 640x400, with keyboard make/break input,
focus changes, moving, minimizing, restoring and cooperative close. Unchanged
framebuffers do not repaint the desktop or increment the game-frame counter.

Source archives, upstream commits, licenses, modifications and build scripts
are pinned in `third_party/doomgeneric`, `third_party/wolf4sdl` and `src/ports`.
The image includes corresponding sources in `SYSTEM\GAMES\GAMESRC.TGZ` and
credits/licenses beside it. Game data remains separately licensed.

## Runtime contract

DOSWIN ABI3 has a 256-byte header. One parent-owned 64,000-byte conventional
allocation holds the indexed frame; the module holds its palette, converted
colors, input queue and separate stacks. Text and graphics share the existing
desktop compositor and its clip/damage rules. Presentation does not call DOS,
allocate memory or change the physical video mode.

`graphics_bridge_abi.inc` and `graphics_bridge.h` define CG ABI1. Discovery uses
an exact private INT2F AX/BX pair. DOS/4GW 1.97 rejected INT31/0301 on the actual
smoke test, so the host advertises INT2F transport through working INT31/0300.
The original far-procedure interface remains available to hosts implementing
[DPMI 0301](https://www.delorie.com/djgpp/doc/dpmi/api/310301.html).

Frame submissions and clock/input polls are separate operations. Only a
submitted frame whose native window was painted increments the completed
visible-frame counter. Minimized frames, IRQ callbacks, idle polls and text
paints do not count as game FPS. Tests keep the guest running and use real
keyboard/mouse events; guest memory is read for observations, never injected.

The clock latches existing PIT0 status and count, combining phase with BIOS
ticks without changing the divisor. Mode3's count decreases by two and OUT
identifies its half-period; this differs from mode2. Compare the primary
[QEMU PIT implementation](https://github.com/qemu/qemu/blob/master/hw/timer/i8254.c).
An acknowledged IRQ can precede its reflection in BIOS time under DOS/4GW:
one-period stale samples are deferred, instead of turning an unsigned negative
delta into an hour-long clock jump. The QEMU frame gate also checks guest time
against host elapsed time. Long unattended sessions crossing the BIOS midnight
reset are not qualified yet.

The BIOS AT keyboard intercept preserves the previous handler's incoming and
returned flags and captures accepted make/break events. Graphics clients drain
the duplicate BIOS ring to avoid firmware beeps from a full ring. Closing a
window asks the engine to quit normally; it does not kill an arbitrary DOS
process or leave a protected-mode host resident deliberately.

## Evidence and boundaries

Selected build: `build/full/window-games-2026-09-26/final-r3/ciukios-window-games.img`
(134,217,728bytes), SHA256
`8354a833028b67ae8dc06f12718aa264b0bc2f254ad9123e6e727b0936fd3587`.
`final-r3/manifest.json` and `source-freeze.json` bind image, binaries, reports
and sources. All final QEMU gates use copies of this exact image.

| Final QEMU check | Observed result |
| --- | --- |
| Doom, continuous turning,10.002s |334 changed visible frames;33.39/s |
| Wolf3D, continuous turning,10.002s |651 changed visible frames;65.09/s |
| Game-clock / host elapsed-time ratio |0.999994 Doom;1.000096 Wolf |
| Game controls |Input, About drag/focus, minimize/restore, title close passed |
| Library launch |Both real Games tiles launch and close their engines |
| DOS text |COMMAND DIR/ECHO/EXIT, COM/MZ BIOS fixtures, nested EXEC, file bytes and text pixels passed |
| Interrupt teardown |Original08/10/15/16/2F/33 vectors restored |
| Native utilities |Copy, rename, move, protected/cancelled deletion, Tasks and3 full-screen COM returns passed |

These measurements use QEMU KVM, a PentiumIII CPU model,128MiB and std-VGA
1280x800x32. The two source ports suppress unchanged frame submissions and
produce no audio. Final text input trials have a13.28ms median completion upper
bound across idle and active workloads, versus42.38ms in the instrumented
baseline; active text workloads still use18.2Hz fallback. Full text-paint elapsed
TSC fell about12–13%; idle COMMAND performed zero scene paints during10s.

The kernel, COMMAND and selected original full-screen launchers are byte-identical
to the preceding qualified image. Three rebuilt audio/HDPMI files differ only in
their four-byte COFF link timestamp; normalized code/data equality is recorded
in `preserved-payloads.json`. This comparison is not a new physical-audio test.

Authoritative images and reports are under
`build/full/window-games-2026-09-26/`; the final manifest identifies the selected
image and completed checks. Earlier failed candidates are retained. In
particular `doom-first` is excluded from FPS claims: its initial harness missed
an invalid guest clock, subsequently caught and repaired. The harness now
requires a clock ratio between 0.9 and 1.1 and continuously turns the player
while counting changed-frame submissions for at least ten seconds.

The controlled text comparison uses an instrumented original renderer/schedule
and the same QEMU configuration. It records idle CPU time, completed paints,
callback/paint elapsed TSC and observed input latency separately. A 72% change
in cell instruction count must not be reported as a 72% application speedup.

Current limits:

- The windowed source ports are silent. Original full-screen audio launchers
  remain separate; they were not replaced by silent versions.
- No GPU/2D acceleration backend is implemented or claimed here. Writing an
  LFB is software presentation. QEMU std-VGA is not a host-GPU rendering API;
  Cirrus BitBLT would require a separate device driver and is still emulated by
  QEMU. See its [primary implementation](https://github.com/qemu/qemu/blob/master/hw/display/cirrus_vga.c).
- Graphics bridge requires native LFB, at least800x600, 15/16/24/32-bit color.
  Shared 8-bit desktop palette changes are refused. Game client resolution is
  320x200 with fixed2x presentation, not a general resizable virtual VGA.
- No guest mouse capture, joystick, multiple concurrent DOS processes, direct
  VGA/B800/port virtualization or general Windows-in-a-window support.
- QEMU KVM with a PentiumIII CPU model and128MiB does not emulate PentiumIII
  execution speed. It establishes these emulator results, not a30FPS guarantee
  on T23/E500/ATI9200. Physical hardware needs its own measurements.

## Three parallel follow-up prompts

Each prompt must preserve the frozen image and failing evidence, follow
AGENTS.md/Semble, avoid physical disk/CD writes and keep the session's8GB limit.
Coordinate shared ABI changes before integration; do not claim completion from
compilation, callback counters or a silent program.

### 1 — Audio and frame pacing

> Own `src/ports/doomgeneric`, `src/ports/wolfwindow` and a new isolated audio
> backend/module. Add real PCM and music for the two windowed source ports,
> reusing verified open-source components and documented CiukiOS/T23 audio
> behavior. Preserve existing full-screen Doom/Wolf/Windows launchers. Do not
> install conflicting TSRs, take IRQ ownership without restoring it, or call
> DOS from the presentation callback. Define an audio-ring ABI separately and
> coordinate it with the host. Implement background/minimized pacing without
> modifying BIOS tick cadence or corrupting the game clock. Verify actual
> non-silent audio capture, game/input progression, normal close, repeated
> launches and subsequent full-screen audio. Measure changed visible frames
> with audio active; retain failures and report real-hardware limits honestly.

### 2 — Device acceleration

> Own a new `src/video` driver area and capability probes. Implement optional
> hardware-backed blit/presentation for supported vintage adapters using
> primary register documentation or compatible licensed source. Prioritize
> T23 S3 Savage, Armada E500 ATI hardware and ATI9200 after exact PCI detection.
> Expose an explicit capability ABI, VRAM ownership, clipping, synchronization,
> timeout/recovery and software fallback. Do not equate a framebuffer copy or
> QEMU emulated BitBLT with host-GPU acceleration. Preserve high-resolution
> modes up to2560-wide displays where firmware/VRAM supports them; do not
> globally force a Cirrus adapter that loses required modes. Compare actual
> output pixels and measured transfers; qualify physical hardware separately.
> Integrate only through an agreed presentation hook, not concurrent edits to
> DOSWIN or source-port input/audio code.

### 3 — General DOS compatibility and hardware qualification

> Own a new isolated DOS virtualization layer and qualification scripts. Audit
> the existing BIOS-text and cooperative CG contracts before adding support
> for unmodified graphics DOS executables. Design proper protected-mode/V86,
> DPMI, VGA memory/ports, timer and input ownership, process isolation and normal
> teardown; source ports are not proof that arbitrary original EXEs work in a
> window. Preserve FAT and existing native EXEC paths. Cover Windows3.1,
> original Doom/Wolf, CapsLock, nested EXEC, file operations and subsequent
> audio. Validate cold boot and installed-HDD behavior on T23/E500 through
> non-destructive logs supplied from hardware, and explicitly leave the
> PentiumIII/128MiB/30FPS claim open until measured there. Work independently
> of the GPU/audio modules and document the required integration boundary.
