# T23 protected framebuffer and native SuperSavage validation

The connected T23 disk was inspected read-only before implementation. Its
driver log records S3VBEFIX installing its INT10 hook, and the native ICH3
AC97 log records all 245760 startup bytes played. The remaining fault is the
display path: attempted VBE previews return to VGA 640x480 with 16 colours.
The last CVB1 snapshot describes that rollback; it cannot establish the
failure stage of every startup candidate. The complete capture is retained
in `build/full/t23-next/physical-logs-20261007-204040/manifest.json`.

Research, source archive hashes and implementation decisions are recorded in
[the graphics recovery design](../design/t23-graphics-runtime.md),
[protected transport](../design/protected-desktop-lfb.md),
[upstream research](../design/savage-upstream-research.md),
[panel detection](../design/t23-panel-detection-research.md) and
[native BCI](../design/native-supersavage-bci.md).

The desktop now requests and verifies a genuine BIOS linear mode, then uses
CVSESSION's protected framebuffer mapping for rows, fills, pointer access and
page copies. AUTO uses valid EDID or the observed active SuperSavage panel
dimensions, ranking firmware modes by area and then colour depth. Native GPU
capabilities require independent private-VRAM pixel readback tests. The 3D
interface implements untextured Gouraud triangles; TinyGL remains software
rendered, and this change does not promise a complete accelerated OpenGL stack
or hardware rasterization of classic DOS Doom.

## Completed checks

Full HDD, full CD and portable Windows archive builds completed inside capped
systemd user scopes. Builds and QEMU ran sequentially with `MemoryMax=3G`,
`MemorySwapMax=1G` and one CPU. Setup's existing arena, stack and I/O buffer
limits remain unchanged; only its unused protected client is omitted.

- Actual monitor instructions: 77 framebuffer, guest-span, triangle, native
  dispatch and release checks; complete JWasm assembly without warnings.
- Actual VBE client instructions: 18 metadata groups and 38 AUTO groups,
  including native panel bounds and safe versus retained binding failures.
- Actual GUI teardown instructions: eight ownership/quarantine scenarios.
- Native SuperSavage C backend: 35 bounded protocol scenarios, including
  independent fill/copy/triangle proofs, resource bounds and state restoration.
- Actual `SAV3D.COM`: 14 client scenarios; diagnostic parser: ten tests.
- Actual streamed shell help: existing content, read boundaries and errors.
- Full HDD AUTO: 1280x800x32 protected LFB, exact firmware mode readback,
  Ciuk1 RGB samples, complete native AC97 audio, F4 help and safe unsupported
  native-client reporting on QEMU. Evidence: `qemu-final-auto/report.json`.
- Display Properties: preview rollback, Keep, persistence and restart on
  standard VGA and VirtIO. Evidence: `qemu-final-mode-profiles/*/result.json`.
- Framebuffer damage: standard/Cirrus protected LFB and NOVM real-mode LFB,
  including moved windows below scanline 512. Evidence:
  `qemu-final-aperture-checked/results.json`.
- Full CD-only boot: shipped shell/resources and actual Ciuk1 Fill pixels.
  Evidence: `qemu-final-cd/report.json`.
- Native DOSVM Doom: first level rendered; desktop pointer response 0.181 s
  before launch and 0.199 s during gameplay. Evidence:
  `qemu-final-doom-cursor-checked/report.json`.

QEMU evidence directories above are under `build/full/t23-next/`. QEMU does
not emulate SuperSavage and cannot qualify its engine or T23 performance.
The Windows archive receives integrity/content checks, not a Windows runtime
claim. The next physical boot must supply `SYSTEM/VIDEO/VBE.TRC`, `GPU.LOG`
and optionally `GPU3D.LOG` from F4 `sav3d` to establish the actual selected
mode, usable engine capabilities and completed hardware operations.
