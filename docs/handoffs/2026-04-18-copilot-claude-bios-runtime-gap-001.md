# Handoff: BIOS-RUNTIME-GAP-001 — INT 16h + INT 1Ah Dispatch Wiring

**Date**: 2026-04-18
**Agent**: Copilot (Claude)
**Branch**: `feature/copilot-claude-bios-runtime-gap-001`

## Context and Goal

DOOM startup requires COM/EXE programs to call INT 16h (keyboard) and INT 1Ah (timer) through the services ABI. The stage2 keyboard ringbuffer (IRQ1, 128-entry, scancode-to-ASCII decode) and timer tick counter (PIT, IRQ0) were fully implemented but had **no dispatch path** from `ciuki_services_t` to running programs.

This task wires both interrupts end-to-end so loaded COM/EXE binaries can call `svc->int16()` and `svc->int1a()`.

## Files Touched

| File | Change |
|------|--------|
| `boot/proto/services.h` | Added `int16` and `int1a` function pointers to `ciuki_services_t` |
| `stage2/include/keyboard.h` | Added `stage2_keyboard_read_key()`, `stage2_keyboard_peek_key()`, `stage2_keyboard_shift_flags()` |
| `stage2/src/keyboard.c` | Added parallel scancode ringbuffer (`g_scanbuf[]`); implemented 3 new API functions for INT 16h-compatible key access |
| `stage2/src/shell.c` | Implemented `shell_com_int16()` (AH=00/01/02/10/11/12) and `shell_com_int1a()` (AH=00); wired into services init |
| `stage2/src/stage2.c` | Added dispatch-wired markers for INT 16h and INT 1Ah |
| `scripts/test_doom_boot_harness.sh` | Fixed pre-existing marker mismatch (scaffold → checkpoint v1) |

## Decisions Made

1. **Parallel scancode buffer**: Rather than widening the existing ASCII ringbuffer to u16 entries (which would break all existing callers), added a paired `g_scanbuf[]` array. Each push stores both ASCII and the raw scancode. Existing `getc_nonblocking()`/`getc_blocking()` APIs remain unchanged.

2. **INT 16h AH=01h uses carry for ZF**: Real INT 16h returns ZF=1 for no key. Since our register struct uses `carry` as the flag convention, carry=1 encodes "no key available" (ZF equivalent).

3. **INT 1Ah tick scaling**: PIT runs at 100 Hz; real BIOS ticks at ~18.2 Hz. Applied `ticks * 18 / 100` scaling to approximate BIOS-compatible tick count. This is close enough for frame pacing (DOOM uses ~35 fps / ~70 tick targets).

4. **Extended functions**: INT 16h AH=10h/11h/12h (enhanced keyboard) map to the same implementations as AH=00h/01h/02h. This covers DOS programs that use the enhanced keyboard API.

5. **DOOM harness fix**: The `test_doom_boot_harness.sh` checked for `(320x200x8 scaffold)` but the actual source marker was already `(320x200x8 checkpoint v1)`. Fixed the harness to match.

## Validation Performed

- `make clean all` — 0 warnings, 0 errors
- `bash scripts/test_vga13_baseline.sh` — PASS (static tier; runtime skipped on CachyOS Wayland)
- `bash scripts/test_doom_boot_harness.sh` — all 4 active stages PASS (binary_found, wad_found, extender_init, video_init)
- Source-level verification: dispatch markers present, services wiring confirmed

## Risks and Next Steps

1. **No runtime smoke test COM**: A small COM binary that exercises `svc->int16()` and `svc->int1a()` would add confidence. Consider creating `com/bios_smoke/` in a follow-up.
2. **Tick scaling precision**: The `*18/100` approximation loses fractional ticks. If DOOM timing loops are sensitive to sub-tick accuracy, a higher-precision fixed-point scaler may be needed.
3. **INT 16h extended scancodes**: Arrow keys, F-keys, etc. are decoded but stored with high-bit ASCII markers (0x80–0x86). Real BIOS returns 0x00 in AL for extended keys with the scancode in AH. This may need alignment for programs that check `AL==0` to detect extended keys.
4. **Protected-mode dispatch**: INT 16h/1Ah are now available in real-mode COM dispatch. When DPMI programs need these via INT 31h AX=0300h (simulate real-mode interrupt), the reflection path will need to route through these handlers.
