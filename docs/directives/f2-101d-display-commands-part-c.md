# f2-101d — Display driver model part C: the new drawing commands, clip lists, batches, fences, colour cursor

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f2-101d` from `wt/f2-101` (part A, not yet on main).
- **Contracts:**
  - `docs/design/display-driver-model.md` (DM2–DM4 and Amendment 1);
  - `docs/design/display-command-semantics.md` (f2-101c, with the lead decisions at its end). This is the exact specification, golden vectors included.

## Required

1. **Software engine.** Implement in `swdib` MONO, INVERT, KEYED, SCALED (nearest and bilinear) and LINE, with clip lists, exactly as specified. The golden vectors of the specification become host tests at 16, 24 and 32 bpp.
2. **Batch ABI.** The v1 synchronous batch request with fences, admission limits, staging outside display locks, prefix completion and lock lifetime, as specified.
   - Allocate the negotiated request and capability numbers.
   - Add ABI oracle rows, one field per line.
   - Update the SDK headers and libciuki wrappers, and regenerate the API reference.
3. **Colour cursor.** The ARGB cursor path (premultiplied, up to 64×64) with the software overlay fallback owned by the desktop. Drivers advertise colour cursor support through capabilities. The f2-99 cursor set uses it where available.
4. **Clients.** libdeskui and the desktop compositor use batches and the new commands for text (MONO), focus and drag outlines (INVERT) and icons (KEYED) where the capability exists. Otherwise they keep the current paths, so the output stays identical; check the goldens.
5. **Punting.** `-EOPNOTSUPP` means no submission and no pixel change, then the software path draws. Overlapping and non-idempotent operations are never replayed blindly. Fake-driver integration tests cover every command, punting, quarantine during a batch, sliced waits and mode races.
6. **Out of scope.** Native implementations in the Savage and other drivers belong to the family directives.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, register references, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f2-101d-report.md`.
