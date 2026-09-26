# CiukiOS renderer audit — 2026-09-25

This report covers the banked VBE renderer and compositor. It does not claim physical ATI Radeon 9200 validation, whole-system compatibility, or a measured Pentium III animation frame rate. Integration screenshots and QEMU interaction tests are maintained separately by the UI qualification work.

## Findings and repairs

- The compositor previously read every destination dword from VRAM before deciding whether to write it. It now copies completed damaged bands sequentially using `REP MOVSD/MOVSB`, without destination reads. The CPU regression test asserts that the compositor performs zero VRAM reads; real GPU bandwidth/latency still requires measurement.
- Empty, off-screen, and overflowing damage rectangles could reach a band copy with invalid extents. Damage now clamps dimensions before addition and rejects zero or off-screen rectangles; presentation independently rejects empty horizontal/vertical ranges.
- Band allocation now validates its stride before allocating DOS memory. It retains a fixed 60 KiB buffer, independent of resolution, and avoids allocating a second band buffer on duplicate initialization.
- A band stores only the damaged horizontal span. At 2560×1440×32, a 540-pixel damaged window fits 28 scratch rows instead of six. This reduces scene replay count; it is a checked buffer-capacity improvement, not a measured frame-rate multiplier. Rectangles and glyphs translate absolute screen coordinates into this packed scratch origin.
- Painting no longer reads `BOOT.SND` on every click, drag, or animation step. The root UI refreshes that preference when entering the video session, including return from a settings command.
- Optional hardware page presentation renders complete damaged scenes on a second VBE page, then switches the visible page. It requires two reported image pages, sufficient active scanlines, enough physical video memory and representable bank numbers; initialization also verifies both page starts through VBE set/get calls. Previous-frame damage is carried forward into the next back page, avoiding framebuffer readback or a second scene pass. Cursor pixels belong only to the visible page and are removed before that page becomes the back page.
- A failed page capability probe restores page zero and keeps ordinary banked presentation. A runtime bank/page failure exits VBE, requests a fresh UI video session, and disables further paging attempts for that shell process. The root must check `ui_comp_recover` after video initialization and drawing; pointer save/restore must use `ui_front_base`.
- Video validation now checks framebuffer bytes against the controller's reported VRAM when available and verifies that bank number times granularity fits VBE's 16-bit bank register. Bank mapping refuses offsets beyond the validated surface and stops drawing after a BIOS failure.
- The console's framebuffer clear now handles an odd final byte; queued text cells beyond the console geometry are rejected. Text rendering does not prefetch a nonexistent bank after the last pixel of the last cell and checks bank-switch failure before subsequent stores.

The implementation retains ordinary real-mode segments, the existing VBE bank window, and the caller's machine state. There is no new protected/unreal mode transition, resident hook, kernel, filesystem, audio, or application-launch change in this part of the work.

## Native high-resolution profiles

`vc_resolve_mode` accepts width in AX and height in DX, preserves registers, and sets `vc_mode` to the highest-depth validated BIOS mode with exactly that geometry, or zero if none exists. It does not invent VBE mode IDs. AUTO still follows EDID preferred dimensions when supplied; explicit profiles can request another firmware-supported geometry.

The common four-byte `DISPLAY.CFG` reader accepts these additional profiles:

| Token | Native geometry |
|---|---|
| `1280` | 1280 × 1024 |
| `1600` | 1600 × 1200 |
| `1920` | 1920 × 1080 |
| `2048` | 2048 × 1152 |
| `2560` | 2560 × 1440 |

Existing `0800`, `1024`, `AUTO`, `0640`, and text/recovery behavior remain compatible. The CLI/UI which persists these values must probe a selected mode before saving it. Firmware mode availability and monitor support remain hardware-dependent.

2560 × 1440 at 32 bits per pixel requires 14,745,600 framebuffer bytes (14.0625 MiB), plus any firmware pitch padding. Two hardware pages require 29,491,200 bytes (28.125 MiB), so a 32 MiB QEMU VGA is required for that presentation path at this depth. The compositor uses 61,440 conventional-memory bytes; the 320 × 90 console cell surface uses 57,600 bytes. Rendering is native pixel addressing with 32-bit byte offsets, not an enlarged low-resolution framebuffer. Those buffer sizes do not establish a real-machine performance guarantee.

Page switching uses immediate VBE display-start requests, not firmware's potentially unbounded retrace wait. This prevents presenting a framebuffer midway through its band composition on supported hardware; it does not guarantee vertical-blank synchronization on a physical monitor. Single-page firmware retains the completed-band fallback and can expose a long full-screen repaint in progress. QEMU/physical timing qualification remains necessary before claiming the user's no-glitch completion criterion.

## Executed deterministic checks

Command:

```sh
uv run --with unicorn python scripts/test_ui_rendering.py
```

The test assembles `scripts/fixtures/ui_rendering_cpu.asm`, which includes the production source unchanged, then runs those x86 instructions in Unicorn. The fixture supplies a deterministic VBE bank BIOS; it does not replace the production algorithms with Python implementations. Expected framebuffer bytes and guard regions are independently calculated in Python.

Result: **fourteen groups passed**:

1. Six native geometries (800×600, 1024×768, 1920×1080, 2048×1152, 2048×1536, 2560×1440), each at 8/15/16/24/32 bits per pixel, including padded pitches.
2. Insufficient VRAM and excessive console allocation rejected.
3. Exact-mode enumeration selects the greatest valid depth and returns unsupported for an absent geometry.
4. Zero-sized/off-screen damage ignored, overflowing extents clipped, successive rectangles unioned.
5. Partial band copies across video-bank boundaries match every expected framebuffer byte and preserve every byte outside the damaged region, at all five depths, without reading destination VRAM.
6. Rectangle fills clip against all four band edges and preserve all allocation guard bytes at all five depths.
7. Proportional glyphs crossing all four packed-band edges preserve the expected pixels and every guard byte at five depths.
8. A 540-pixel damaged window on a 2560×1440×32 surface fits 28 rows in the same scratch buffer versus six rows for full width.
9. A 24-bit span beginning two bytes before a bank boundary produces the correct RGB byte sequence; a zero-length span changes nothing.
10. An odd-sized surface clears its final byte while preserving all following VRAM.
11. The final text cell of a 2560×1440×32 surface renders correctly when the final pixel ends exactly at a bank/framebuffer boundary, without disabling video.
12. Double-page initialization accepts a validated 32 MiB 2560×1440×32 allocation, exposes exactly two framebuffers to bank mapping, and declines insufficient video memory/scanlines.
13. Rejected display-start probing restores page zero before disabling the optional path.
14. Four successive moving/resizing window scenes present complete, independently checked framebuffers at each page switch, preserve all VRAM beyond both pages, and perform no destination VRAM reads. An injected runtime flip failure requests video recovery and prevents another paging attempt.

Evidence: `build/full/ui-redesign-2026-09-25/rendering-cpu/results.json` and the assembled `renderer.bin`. The production shell and VGA Setup also assembled successfully with NASM after these backend edits. These CPU-level checks supplement actual QEMU interaction, visual inspection, and physical-machine validation; they do not replace them.

## Primary references

- [VESA VBE Core Functions 3.0 specification, preserved by University of Texas](https://www.cs.utexas.edu/~dahlin/Classes/439/ref/hardware/vbe3.pdf): controller memory, bank granularity, mode geometry, and scanline fields.
- [QEMU Standard VGA specification](https://www.qemu.org/docs/master/specs/standard-vga.html): emulated VGA resources and configurable video memory. A QEMU result qualifies that emulated device, not the ATI firmware.
