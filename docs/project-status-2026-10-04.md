# CiukiOS 0.8.3 status — 4 October 2026

This is the current status page; dated earlier reports remain historical evidence.
The active profile is the full FAT16 HDD image and its sanitized Windows bundle.

## Changes in this release

| Area | Implementation | Validation |
| --- | --- | --- |
| Presentation | Retained VirtIO staging, actual pixel damage, independent VM clocks; eliminate the desktop's periodic full-scene repaint. | 73 GPU protocol checks pass. Doom new game/exit pass; 34.78 loops/s, residual pacing spikes remain. |
| Mouse | IntelliMouse negotiation, physical and virtual four-byte PS/2 packets, INT 33h wheel ABI and native window scrolling. | 1,285 peripheral-model assertions pass with ASan/UBSan; QEMU Files scroll down/up pixel check passes. |
| Display | Native graphical preview, 12-second rollback, transactional profile save, adapter/driver/EDID tabs. | 32-bit timeout, 16-bit banked rollback, confirmation and saved mode across reset pass. |
| CiukWeb and network | Resident network bridge; cooperative HTTP streaming, redirects and browser renderer, plus bounded CSS, TLS 1.2 and mQuickJS script workers. | Google HTTP 301→200 renders 88,229 source bytes; browser→mode change→Doom new game/exit passes after the IRQ fix. Latest worker build and host tests pass; HTTPS/CSS/JS browser runtime validation remains pending. |
| Desktop | About replaces the startup library; separate About/Credits pages; persistent startup checkbox; fitted titles. | Focused Linux QEMU checks verify default About at startup, checkbox persistence across reset, and active title color after desktop focus returns. |

## New integration — build verified; browser runtime checks pending

- CiukWeb renders a bounded HTML subset: page titles, headings, links, tables,
  GET forms, inline color and bold text. PNG, GIF and baseline JPEG images
  decode into XMS-backed browser storage. Limits include 128-character URLs,
  24 images, 6,144 parser nodes, 2,048-pixel dimensions and 12 MiB decoded RGB.
  New worker code adds TLS 1.2 with trust-anchor/time/hostname checks and an
  mQuickJS classic-script subset with `document.write`, ID lookup,
  `textContent`/`innerHTML` and bounded page mutations. The separate CSS worker
  handles a bounded selector/property subset. Full CSS, a complete DOM, TLS
  1.3 and Internet Explorer equivalence are not claimed. The worker target
  build and JS/page host checks pass; end-to-end browser runtime validation is
  still pending. Adam7 PNG and progressive JPEG are rejected, and GIF displays
  only its first frame.
- About is shown at startup by default; **Don't show this at startup** suppresses
  it. Focused Linux QEMU checks verified disabling About, a real reset without
  auto-opening it, re-enabling the preference, and restoring the active title
  color after clicking the desktop and returning to the same window.
- AC'97 queue depth is now eight 256-frame descriptors, about 46 ms at 44.1 kHz
  compared with about 186 ms for the previous eight 1,024-frame descriptors.
  In the post-change Doom capture, playback continued with queue depth generally
  6–8; no new underruns were recorded after the initial diagnostic sample. The
  automated WAV run does not assess perceived latency through the host speaker
  backend, and the first sample contained one underrun after a diagnostic pause.
- The full image build succeeds with the browser module using 63,040 bytes and
  `WEBIMG.APP` using 43,648 bytes. These are build measurements, not runtime
  verification of the browser's HTTPS/CSS/JS behavior.

## Boundaries

Original DOS Doom runs its software renderer on the guest CPU at a nominal
35 game tics per second. Average rate alone does not establish smooth frame
spacing. The October 4 baseline reproduced software presentation gaps above
100 ms while the game averaged 34.78 loop starts/s. Measurements distinguish
engine timing, capture, GPU commands and actual monitor presentation.
At matched 800×600, the desktop-damage correction reduced submission gaps over
42 ms from six in the earlier 9.9-second sample to one and two in the two corrected
samples. The final maximum remained 67.73 ms, so zero visual stutter is not claimed.

VirtIO uses the 2D resource protocol. Host GL presents its texture; there is
no general guest accelerated OpenGL/Direct3D implementation. ATI/NVIDIA base
backends support a checked list of older adapters; detection is not physical
hardware qualification. HTTPS/TLS, CSS, script behavior and DOM mutations have
implementation code but still need browser runtime qualification. TLS 1.3,
full CSS/DOM, general PE32/Win32 execution and broader DOS compatibility remain
unsupported or open.

All runtime evidence in this release is Linux QEMU/KVM on the supported
single-vCPU profile. Windows portable packaging is checked for integrity and
absence of private payloads; Windows execution is not claimed.

## Evidence

- [Post-release freeze, About and large-page fixes](validation/2026-10-04-post-release-fixes/README.md)
- [Final runtime results and cadence](validation/2026-10-04-release-runtime/README.md)
- [Actual release screenshots](screenshots/0.8.3/README.md)
- [Graphical transitions and presentation](validation/2026-10-04-desktop-transitions.md)
- [Mouse wheel](validation/2026-10-04-mouse-wheel.md)
- [Startup and window titles](validation/2026-10-04-desktop-startup.md)
- [About startup default and focus repair](validation/2026-10-04-about-startup-focus.md)
- [About/Credits compact layout](validation/2026-10-04-about-layout.md)
- [AC'97 queue latency change](validation/2026-10-04-ac97-audio-latency.md)
- [CiukWeb image decoder design and limits](validation/2026-10-04-webimg-codec.md)
- [Web and network](validation/2026-10-04-web-network.md)
- [Native GPU architecture](validation/2026-10-03-native-gpu-presentation.md)
- [Build and run](build-and-run.md) · [Windows release](windows-portable-release.md)
