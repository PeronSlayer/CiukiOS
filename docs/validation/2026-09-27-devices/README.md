# Guest devices, IF negotiation and resizable window — 27 September 2026

Exact copies of the local reports under `build/tests/vm-final-2026-09-27/`,
unless noted. All QEMU runs use `-cpu pentium3 -m 128` with KVM. Nothing here
qualifies physical T23/E500 hardware or hardware acceleration. See
[input and sound devices](../../vm-input-audio-devices-2026-09-27.md),
[V86 interrupt profile](../../vm-v86-interrupt-profile-2026-09-27.md) and
[video session](../../vm-video-session-2026-09-27.md#resizable-dos-window).

## Artifacts under test

| Artifact | SHA-256 |
| --- | --- |
| JEMM386 (single build, profile negotiated at runtime) | `486d1453850f75f17124707ee25e1e7230e23ce81ef42bfc1d8ccefc2b99890a` |
| JLOAD | `b76e057a545d6560e1d56fd1d5343bbe79d9a382bafab4fb10eb8e3b44125512` |
| CVSESSION.DLL (135,168 bytes, [manifest](module-final.json)) | `102dc1ee1834a51d94b8f6db76973181a273e658c9c22974117f71553a361ddd` |
| SHELL.COM (60,928 bytes = the 0xEF00 arena) | `ad6beaa6cd494301b7df5a901e17f432d66c156413e71b5eb4d16ed2a0af07d3` |
| DOSWIN.DRV (36,384 bytes) | `f13e44d0e672cc80ad9a370235382c44a08e8a44003ad8dbc907cd17aa1e2c5f` |
| HDPMI32I (sources unchanged; differs from the qualified `3af7ae37…` only in the 2 PE timestamp bytes at 1EF8h) | `f8b949ff6a852967a10565955f09b6743234729a67f9e792bc0e256301021830` |
| Kernel `ciukidos.sys` | `2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af` |

The Jemm build manifest is [jemm-build-final.json](jemm-build-final.json)
(patch `de747a75…`, build script `a2c40490…`).

## Results

| File | Gate | Result |
| --- | --- | --- |
| `v86cli.json` | V86 CLI/IRQ contract plus runtime allow/decline/release negotiation, flags `0100000F` | PASS |
| `devtest.json` | DEVTEST.COM: raw keys with focus, SB16 DMA/IRQ7 ×6, DSP 4.05, square 2.5 s and OPL FM 1.0 s heard on the AC'97 | PASS |
| `window.json` | Native DOS window: VGASEM checkpoints, focus, cover/minimize, FIRE (18.24 paints/s) with frame present, resize 500×360 and 760×496 with rescaled content, GUESTIO keyboard/mouse (also at a resized client)/SB/OPL, audio in the window | PASS |
| `vga.json` | Full-screen VGA acceptance | PASS |
| `legacy-v86.json`, `legacy-dpmi.json` | Earlier session gates | PASS |
| `lifetime-1.json`, `lifetime-2.json` | HDPMI lifetime with doom-vanille and the deliberate-fault client, profile negotiated | PASS, 2 of 2 |
| `dos-window-text.json` | Text-mode DOS window (BIOS child, COM and MZ) with the new SHELL/DOSWIN | PASS |
| `unit-*.json`, `unit-console.json` | Peripheral model (912 assertions), device link, scheduler (Unicorn), DOS-window lifecycle (13 cases), virtual VGA (168,060), x86 differential (20,000 + 20,000), Jemm device query | PASS |

## Failures preserved

- `failure-run-field-stale-bios-key.json` (from
  `build/tests/vm-devices-2026-09-27/window19`): after FIRE was closed, the
  next Run dialog closed itself. A key left in the shared BIOS ring by the
  exited guest reached the desktop. Fixed by discarding the ring when the VGA
  session ends.
- `preexisting-native-windows-{final,baseline}.json`:
  `qemu_test_native_windows.py --profile 0800` fails at "Display panel did not
  open" both with the new SHELL and with the unmodified
  `ciukios-native-desktop.img` (`08bc6df6…`). The harness predates the current
  desktop layout. This is recorded, not fixed here.
- The launch-scene page bug (see the video record) is visible in local
  captures `build/tests/vm-devices-2026-09-27/window20/fire-in-window.png` and
  in the earlier baseline `build/tests/vm-completion-2026-09-27/final-window-ordinary/window-checkpoint-3.png`
  (no DOS task button). No report failed on it, because the old harness did
  not check the frame.

## Not covered

Protected-mode (HDPMI) clients do not use the device model. No original DOS
game's audio through the model was run (Doom/doom-vanille are DPMI).
