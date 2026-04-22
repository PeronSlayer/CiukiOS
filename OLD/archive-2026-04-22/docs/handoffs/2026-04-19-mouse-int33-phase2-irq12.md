# 2026-04-19 — SR-MOUSE-001 Phase 2 (IRQ12 PS/2 + mode 13h cursor)

## Context and goal
Phase 1 (commit `1ba24c9` on `feature/sr-mouse-001-int33`) delivered the INT 33h
ABI skeleton and the state-only dispatcher. Phase 2 closes the two follow-ups
called out as risks in the original handoff:

1. Wire real pointer input via the PS/2 AUX device on IRQ12.
2. Provide a minimal mode 13h software cursor renderer so graphical COMs can
   surface the pointer without owning their own plane logic.

No version bump (baseline remains `CiukiOS Alpha v0.8.7`).

## Files touched
- `stage2/include/mouse.h` — new: low-level PS/2 driver API
  (`stage2_mouse_init`, `stage2_mouse_on_irq12`, `stage2_mouse_consume_deltas`,
  presence + irq-count accessors, button bit defines).
- `stage2/src/mouse.c` — new: IRQ12 handler, 3-byte packet assembler with
  sync/overflow guards, IRQ-safe delta drain, serial markers.
- `stage2/src/interrupt_stub.S` — added `stage2_irq12_stub` + extern for the
  C handler.
- `stage2/src/interrupts.c` — installed IDT vector 44 (0x20+12) →
  `stage2_irq12_stub`.
- `stage2/src/stage2.c` — includes `mouse.h`, calls `stage2_mouse_init()` after
  keyboard init, emits `[ ok ] ps/2 mouse driver ready (irq12)` / `[ warn ]`
  marker and the `[ compat ] INT33h mouse driver ready` banner.
- `stage2/src/shell.c` — AX=0000h now also drains pending hardware deltas;
  AX=0003h consumes IRQ12 deltas, clips against the active X/Y range, and
  returns the live button mask. Added `shell_mouse_draw_cursor_mode13` (6×6
  arrow bitmap via `gfx_mode13_put_pixel`) and wired it to a new
  `svc.mouse_draw_cursor_mode13` ABI slot.
- `boot/proto/services.h` — appended `mouse_draw_cursor_mode13` tail member
  (null-safe, append-only per ABI policy).

## Decisions made
- Keep the hardware driver strictly additive: if the AUX channel fails to ACK
  (virtualized controller without mouse, or hostile hardware), init returns 0
  and the INT 33h path falls back to the phase-1 state-only behavior. IRQ12
  remains unmasked only on successful init.
- Deltas are accumulated inside the ISR and drained atomically
  (`cli/sti`-bracketed snapshot) by the dispatcher; we do not apply absolute
  positions inside the ISR to avoid coupling range/clipping policy to the
  hardware path.
- The mode-13h cursor is rendered on explicit request (`svc.mouse_draw_cursor_mode13`)
  rather than auto-composited: this matches the DOS contract where the
  program owns the frame, and avoids double-buffer tearing surprises.
- Double EOI on IRQ12 (PIC2 then PIC1 cascade bit) per 8259 protocol.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → full pipeline build
  succeeds (stage2 + all COMs + FAT image + OVMF vars). Host QEMU launch
  skipped per macOS workflow.
- `bash scripts/test_mouse_smoke.sh` → PASS via static fallback (ABI + wiring
  + subset + marker assertions). Runtime tier requires a QEMU run with real
  PS/2 and remains unchanged.

## Risks and next step
- Runtime validation under QEMU with an emulated PS/2 mouse has not been
  executed on this host; the first boot on hardware/qemu should confirm the
  `[mouse] irq12 pkt ok irq#1` marker appears.
- Under UEFI boot paths that leave the legacy PS/2 controller disabled, init
  will silently degrade to state-only. A future task can add the UEFI
  absolute-pointer fallback.
- Cursor rendering is intentionally minimal (single-color, no save/restore
  of background). COMs that want a non-destructive cursor should draw into a
  scratch overlay before calling `present()`, or we can extend the ABI to
  expose save/restore in a follow-up.

Next milestone remains the ongoing DOOM-on-CiukiOS roadmap; mouse input is
now unblocked for any COM that needs pointer interaction in graphics modes.
