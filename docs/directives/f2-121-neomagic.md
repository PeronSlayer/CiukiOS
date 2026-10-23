# f2-121 — Native 2D driver: NeoMagic MagicGraph (laptops)

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f2-121` from `wt/f2-101`.
- **Owner (2026-10-22):** universal acceleration on the common cards of 1998–2004. Every licence is allowed; keep notices, origin lines and credits.
- **Contracts:**
  - `docs/design/video-driver-families.md`, the section "NeoMagic — choose L core, X cursor optional" and sections 5, 6 and 10 (lead decisions and the owner's licence decision);
  - `docs/design/display-driver-model.md` (registry DM1, required commands DM2, punting DM4, hardware check DM9, Amendment 1);
  - `docs/design/display-command-semantics.md` for the optional commands.

## Required

1. **Driver.** Port the chosen donor into `src/kernel/drivers/video/<id>/`, registered through the part A registry with the PCI match table of the contract section.
   - Keep the donor's origin and licence header in every imported file.
   - The driver provides FILL, COPY and SYNC.
   - Optional commands and the hardware cursor follow when the donor supports them; until f2-101d merges, implement them behind the part A hook signatures and capability bits.
   - Mode setting stays VBE unless the contract section names a native mode provider.
2. **Safety.** Probe without engine commands, verify ownership of the active frame buffer, use bounded waits, and never let the engine hold the bus.
   - A fault quarantines the driver, and the software engine takes over after the idle proof of Amendment 1.
   - The DM9 hardware check at activation is the runtime guard. A transport the lead must review is described in the report, but activation is enabled behind DM9 unless the contract section forbids it.
3. **Tests.** Host fake-register tests cover the register sequences of every command (fill, copy in both overlap directions, sync and timeout, cursor), the probe and refusal paths, and quarantine. Add golden comparisons against the software engine where the model allows. Record the documented small-mono failure as a punt to software.
4. **Records.** Device Manager shows the driver's display name, and the Control Panel shows it as the 2D driver. Add a row to the contract's family table with the result.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, register references, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f2-121-report.md`.
