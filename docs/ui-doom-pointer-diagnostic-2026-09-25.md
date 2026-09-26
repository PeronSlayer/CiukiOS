# Doom return: pointer responsiveness diagnostic

The original 300 ms pointer-movement check **failed and remains failed**. A separate extended observation on the unchanged candidate-5 image demonstrated that the original mouse packet was delivered later; it did not establish acceptable responsiveness.

Configuration: Pentium III QEMU guest, 128 MiB RAM, standard VGA, SB16/AdLib, native 1280×800×32 desktop, platinum palette. Candidate image SHA-256: `f34ea704d16250897cf8e8301ff031c2fd1b81b732d040a36344d7c0d677e854`. Shell SHA-256: `4449cd0384077f5e81251dde24d87a79cbc0f0124419a3fcb1d6a067c6313d19`.

The diagnostic reran the real classic Doom sequence (menu, episode/skill, gameplay, movement, shot, quit), checked the subsequent COM program, and returned to the desktop. It sent exactly one `mouse_move 30 20 0` command. The extended observer did not inject another packet.

| Observation | Cursor position |
|---|---|
| Before motion | (640, 400) |
| First observation, after the original 300 ms wait plus approximately 52 ms screenshot matching | (640, 400) |
| Later observation, approximately 777 ms after the command, including screenshot/diagnostic overhead | (700, 440) |

The final displacement is exactly the shell's doubled relative motion. Captured CPU samples were inside the VGA BIOS bank-selection path (`CS=C000`, instruction offsets `37E4` and `37C6`), rather than stopped in the mouse driver. After delivery, F4 and the DOS `ECHO DOOM RETURN READY` command also worked. These observations establish eventual input delivery and continued system operation; they do not turn the earlier response-time failure into a pass.

The code provides a matching explanation: immediately after the first complete desktop presentation, focus animation can require bringing the other hardware page up to date using retained full-frame damage. Unlike the former visible-band renderer, hardware-page presentation keeps an intact front-page cursor visible while composition proceeds. Therefore, finding an intact cursor no longer implies that the event loop has resumed. The synchronous banked repaint delays event processing. A faster coherent framebuffer backend is still required for the requested fluid high-resolution UI.

Evidence:

- Original failed check: `build/full/ui-redesign-2026-09-25/candidate-5/classic-doom-sb16/`.
- Independent diagnostic runner: `build/full/ui-redesign-2026-09-25/doom-pointer-diagnostic.py`.
- Timing, screen captures, CPU samples, audio and immutable-image hashes: `build/full/ui-redesign-2026-09-25/doom-pointer-diagnostic/`.
- Diagnostic `results.json` explicitly records `original_300ms_pointer_gate_passed: false`, `responsiveness_passed: false`, and `pointer_eventually_moved: true`. Its game-completion result refers to the extended functional observation, not the original responsiveness requirement.

No production assembly was changed for this diagnostic.
