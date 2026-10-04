# Native GPU presentation, 2026-10-03

## Research and implementation decision

The existing desktop composes damaged rectangles in software and transfers rows
through CVSESSION into a VBE framebuffer. A different QEMU display flag alone
does not install a guest GPU driver. Linux DRM modules and Xorg DDX binaries do
not implement this operating system's V86/native interface.

Primary references consulted before implementation:

* [VirtIO 1.2, PCI transport and GPU device](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
  specifies modern PCI capabilities, split queues, resource backing, transfer,
  scanout, flush and fence completion.
* [QEMU VirtIO GPU documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
  distinguishes the software 2D device from the VirGL backend.
* [QEMU 11.1.1 VGA wrapper](https://github.com/qemu/qemu/blob/v11.1.1/hw/display/virtio-vga.c)
  retains the VGA/Bochs VBE boot interface, while exposing the modern VirtIO
  transport. Reset restores VGA ownership.
* [QEMU 11.1.1 VirGL dispatcher](https://github.com/qemu/qemu/blob/v11.1.1/hw/display/virtio-gpu-virgl.c)
  handles the base 2D resource/transfer/scanout commands using renderer textures.
  This path does not require guest Mesa, SUBMIT_3D, or negotiating the optional
  VIRGL feature. It accelerates host presentation, not Doom's software renderer.

Implement a freestanding 32-bit VirtIO GPU backend inside CVSESSION, retaining
VBE as the fallback when no supported VirtIO VGA device is present. The desktop
continues using its validated row/page transport, targeting ordinary RAM when
the GPU backend owns presentation. Capture a completed page into separate DMA
backing, submit bounded asynchronous commands and reuse that backing only after
completion. Idle cursor changes use the same presentation boundary. Do not poll
a host GPU fence with interrupts disabled on every frame. Restrict allocations,
validate capabilities and page addresses, and reset/acknowledge the device
before releasing DMA memory. A failed reset retains ownership instead of
allowing writes into freed pages.

Use an explicit Linux `virtio-gl` launch profile and memory-capped sequential
validation. The ordinary VGA profile remains available, and Windows portability
must not be represented as a tested Windows GL configuration. A display flag or
successful build alone is insufficient evidence of native-driver activation.

## Physical ATI and NVIDIA scope

[QEMU's ATI model](https://github.com/qemu/qemu/blob/v11.1.1/hw/display/ati.c)
implements incomplete Rage 128 Pro/RV100 emulation, not host acceleration. The
installed QEMU exposes no NVIDIA graphics device. Physical card drivers use
different register interfaces from VirtIO and cannot accelerate this QEMU path.

[FreeBE/AF 1.2](https://shawnhargreaves.com/freebe/) contains actual hardware 2D
drivers for ATI mach64 and NVIDIA Riva 128/TNT. Its DJGPP relocatable VBE/AF
modules require a loader, mapping and BIOS/DPMI services absent from CiukiOS;
these sources do not cover Radeon R100/R200 or GeForce 2/3/4. The historical
[Xorg nv driver](https://xorg.freedesktop.org/archive/X11R6.8.1/doc/nv.4.html)
and [r128 driver](https://xorg.freedesktop.org/archive/X11R7.5/doc/r128.html)
provide implementation references, not compatible DOS binaries. Do not label
PCI detection or firmware VBE output as vendor hardware acceleration.

## Implemented drivers and integration

`session_gpu.c` and `session_gpu_legacy.c` are freestanding OpenWatcom 32-bit
objects linked into `CVSESS.DLL`, built by the canonical full-image build.
They are CiukiOS drivers; Linux modules were not copied into the DOS image.

The VirtIO implementation negotiates VERSION_1, discovers modern PCI BAR
capabilities and uses a split queue with page-bounded DMA descriptors. It owns
ordinary RAM desktop pages plus a separate tightly packed staging image.
Initialization is asynchronous. A completed desktop page or cursor update
submits TRANSFER_TO_HOST_2D followed by RESOURCE_FLUSH, with one fence on the
flush; all replies must complete before the staging memory is reused. SET_SCANOUT
is established once during initialization. Response descriptor IDs, sizes and
fence values are checked; failures/timeouts are reported. Device reset must be
acknowledged before freeing DMA buffers or unloading the module.

The base ATI/NVIDIA backend scans PCI and discovered bridges, checks the exact
framebuffer BAR, validates the active firmware geometry/pitch and programs the
primary scanout offset with readback. It preserves/restores the initial mode
state and refuses incongruent, tiled or unsupported modes. The current whitelist
covers selected Rage 128, Radeon R100/R200, TNT and GeForce 256/2/3/4 devices;
it does **not** promise every card sold before 2003. The authoritative exact PCI
IDs are in `ati_supported` and `nvidia_supported`. Physical cards were not
available for validation. Firmware still establishes the mode and the CPU
renders pixels; vendor 2D/3D acceleration is not implemented.

Base-register references consulted before implementing this backend:

* [ATI CRTC registers](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/radeon/radeon_reg.h)
  and [Radeon firmware-mode pitch interpretation](https://cos.googlesource.com/third_party/kernel/+/d27d96ddb1dbeefe5da2e15e83cfcfa9ee27e3cb/drivers/video/fbdev/aty/radeon_base.c).
* [NVIDIA scanout register implementation](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/nvidia/nv_hw.c)
  and [Envytools MMIO layout](https://envytools.readthedocs.io/en/latest/hw/mmio.html).
* [VBE 3.0 mode/window versus linear-framebuffer contract](https://courses.cs.washington.edu/courses/cse451/24wi/documentation/vbe3.pdf)
  and [SeaBIOS framebuffer BAR selection](https://qemu.googlesource.com/seabios/+/refs/tags/rel-1.8.0/vgasrc/bochsvga.c).

The optional 24-byte framebuffer bind packet adds width, height, pitch and BPP;
bit 15 of BPP marks a banked firmware mode. The original 16-byte packet remains
accepted. Banked clients can reach verified ATI/NVIDIA modes; otherwise only
known QEMU standard-VGA BAR0 or VirtIO-VGA BAR2 aliases qualify. Arbitrary banked
VBE modes retain their BIOS path. Native page switching has a new PRESENT call;
only the asynchronous VirtIO backend is polled during idle desktop iterations.
The existing large HELP string was losslessly RLE-packed to keep SHELL.COM
inside its segment limit while adding this interface.

## Validation, failures and final result

All builds and QEMU runs were sequential. Builds used a systemd user scope with
3 GiB memory, 1 GiB swap and a one-core quota, with one build job. QEMU used
256 MiB guest RAM, one KVM Pentium III CPU, snapshot disk writes, silent AC97,
a 768 MiB host memory cap, no swap, a two-core quota and a 190-second hard limit.
No continuous video/audio recording or full guest RAM dump was created.

The first GL trials did not retire their fence. The diagnostic scope's original
`TasksMax=32` was too low: the GTK comparison exposed `EAGAIN` when creating a
process. With the same guest and SDL GL backend, increasing **only the task
limit to 128** restored fence completion. QEMU was observed with 31 threads and
about 185 MiB RSS. This supports task exhaustion as the trigger; the precise
failed `pthread_create` inside the earlier SDL run was not traced. The source
path is consistent: [QEMU enables asynchronous thread-backed fences with EGL](https://github.com/qemu/qemu/blob/v11.1.1/hw/display/virtio-gpu-virgl.c),
and [virglrenderer creates a sync worker to retire fences](https://android.googlesource.com/platform/external/virglrenderer/+/e2d45bd07834e5a4a8e93cedb863f8eb4cf7c39c/src/vrend_renderer.c).
No memory/CPU cap was raised. The launcher now uses TasksMax=128.

A second diagnostic issue was capture: QEMU HMP screendump cannot read this
GL-only scanout, and X11 `import -window` returned the same stale Xwayland
pixmap. Those captures do not establish that the guest display froze. Final
validation uses Spectacle's KWin compositor capture of the active QEMU window.
It also starts a new game through the actual menu. The earlier diagnostic
used `-warp 1 1`, but its stale capture could not independently confirm entry
to a newly started game, so it was not accepted as visual proof.

Final evidence is in `build/tests/native-gpu-2026-10-03/`:

* `build-full-final.log`: full-image build and sanitized Windows package passed.
* `protocol-test.log`: the final host mock passed 66 checks, including async
  initialization, reversed completion order, reserved-header fields, DMA backing
  and copied pixels, PCI restore, teardown and rejected feature negotiation.
* `runtime-final/results.json`: native driver enabled at 800×600×32 with zero
  error status. A new game was started from Doom's menu; a ten-second right turn
  changed the rendered scene (`new-game.png`, `game-after-turn.png`). Doom then
  exited normally (`desktop-after-game.png`). F4 released native video for DOS:
  enabled/status/counters returned to zero.
* Over 10.00054 seconds, both `gametic` and `frameon` advanced by 349:
  **34.8981 game tics/render-loop iterations per second**. These offsets refer
  to the unchanged local Doom binary, documented in the earlier cadence audit.
  GPU command completions advanced by 1,408, representing 704 transfer/flush
  batches; this includes desktop/cursor work and is not Doom FPS or a physical
  display scanout measurement. There was no high-frequency screenshot polling.
* `artifacts.json`: image/module correspondence, ZIP manifest/digest and private
  payload exclusions passed. The package contains no local commercial-game
  payloads; 28,088 free FAT16 clusters were zeroed. ZIP integrity is also checked
  by the packager. Windows execution was not tested.

Full image SHA-256:
`5062edeb7a3a2875f09058cc32eb71269e9cd96e2dac2615e8194edf95e4b8e1`.
Windows ZIP SHA-256:
`c4459faa4e3125459bc6ac26d26e65da610610a26a793e7a96c2e75f8bfa2afd`.
The local Doom core remained unchanged:
`799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.

## Remaining limits

The new-game probe injected keys through the QEMU monitor; it is not a new
physical SDL-input test. AC97 was attached to a silent backend, so this is not
a listening test. Physical ATI/NVIDIA hardware, other display resolutions,
GTK GL and Windows runtime are not validated by this run. Native VirtIO uses
the QEMU VGA boot mode's 32-bit BGRX format and is scoped to that known profile.

The original Doom loop runs at 35 tics per second. The result is close to that
ceiling, but neither a driver nor 70 completed desktop presentation batches per
second makes the original game produce 70 unique frames. Removing its 35 Hz
visual cadence would require an engine with decoupled/interpolated rendering
and measurement of actual display timing. This work does not establish total
absence of visual stutter.
