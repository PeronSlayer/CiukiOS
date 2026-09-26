# V86, video and DPMI qualification snapshot

This archive contains unchanged JSON reports and build manifests for the
[VM foundation](../../vm-session-foundation-2026-09-26.md).
[`report-index.json`](report-index.json) records their original local paths
and SHA-256 values. Raw VM disks, PCM, screenshots and memory dumps stay in
ignored `build/`; no proprietary application payload is distributed here.
[`source-freeze.json`](source-freeze.json) records the implementation hashes,
base commit and kernel assembly defines used for this increment.

The final monitor check uses a private copy of baseline image
`08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`,
the corrected 43,217-byte kernel and these exact components:

| Component | SHA-256 |
| --- | --- |
| CIUKIDOS.SYS | `2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af` |
| JEMM386.EXE | `c30b70098d49e4e15748a3cac7384e998b199fa1360a8311d6cca70775cc4f07` |
| JLOAD.EXE | `42599b3cef6d5d2a9c2ec6d117a47a88e6c4c42fd1cd3a5ac60f5ddac20f326f` |
| CVSESSION.DLL | `694670f44074f2b73857d21493cac35dbbf2751529d2147bc9ee99f4ea41dbd2` |
| DPMIVGA.EXE | `f20f9ef92f9a74111050cc89ca46f62df6759305e8b6708337ac58bdadcf4577` |

## Passing evidence

- [Final combined QEMU session](monitor-final.json): V86 DOS child, private
  VGA aperture, protected-mode writes under a fresh VCPI-backed HDPMI host,
  real IN/OUT exceptions, exact restoration of all 32 VGA PTEs, unchanged
  physical text surface, protected physical framebuffer copy, negative
  lifecycle/string-I/O/reset checks, unload and desktop return. The report
  includes all fixture hashes and the actual QEMU command.
- [A20/XMS old/new regression](jemm-a20-xms-regression.json), with
  [old failure](jemm-a20-xms-before.json) and [corrected execution](jemm-a20-xms-after.json):
  actual partial-register preservation, XMS queries, allocation/lock/unlock/free
  and restored accounting under Jemm.
- [Extended XMS](xms-extended.json) and [persistent SYSVARS](sysvars-lifetime.json):
  CPU tests execute the assembled kernel. External state is modeled; they do
  not independently establish device integration.
- [Device query adapter](jemm-device-query.json), [HDPMI range allocator](hdpmi-io-range.json)
  and [VGA model](virtual-vga-model.json): bounded instruction/model checks.
- [Standalone actual HDPMI port exceptions](dpmi-ports-standalone.json):
  32-bit CPU instructions and cleanup without Jemm, separately from the
  combined final session.
- [Windows regression](windows-regression.json): two sessions, applications,
  resize/repaint, measured WAV/MIDI and return on the corrected kernel.
- [Classic Doom regression](doom-regression.json) and
  [audio/kernel qualification](doom-qualification.json): existing fullscreen
  launch, menus/gameplay/movement, audio, quit and desktop return. This VM does
  not contain the experimental monitor.
- [Pinned Jemm build](jemm-build-manifest.json),
  [module build](session-build-manifest.json) and
  [unmodified upstream repeatability](jemm-unmodified-reproducibility.json).
  A build manifest's `runtime_tested: false` describes the build script alone;
  the separate final QEMU report supplies runtime evidence.

## Retained failures and limits

[Old XMS](xms-old-kernel-failure.json) and
[old SYSVARS](sysvars-old-kernel-failure.json) reproduce the fixed kernel bugs.
The [first combined DPMI attempt](vm-before-vcpi-selection-failure.json) fails
before selecting VCPI. The [TSR-status attempt](vm-before-tsr-status-fix-failure.json)
successfully starts HDPMI but the old probe mistakes its documented VCPI TSR
return value for an error. Neither is relabelled PASS. The final probe uses
the documented status range and independently confirms host discovery.

Two earlier legacy Doom taxonomy attempts timed out **before launching Doom**
because that harness waited for a DOS prompt while the image opened its
desktop. Their raw evidence remains in `build/full/vm-session-2026-09-26/doom-regression/`.
The passing replacement uses the existing installed-HDD test's actual F4
route; no product code was changed to satisfy the harness.

All runtime claims here concern QEMU Pentium III/128 MiB. They do not prove
physical T23/E500 operation, original DOS graphics/audio in native windows,
hardware acceleration or 30 fps. The `graphics_virtualized: false` and
`dpmi_virtualized: false` fields intentionally reserve the complete feature
claim; narrower checks have their own passing fields.
