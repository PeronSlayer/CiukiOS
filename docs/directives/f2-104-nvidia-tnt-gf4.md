# f2-104 — Native 2D driver: NVIDIA TNT/TNT2, GeForce 256/2/3/4 (pre-NV40)

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f2-104` from `wt/f2-101` (part A registry, not yet on main).
- **Owner (2026-10-22):** universal acceleration on the common cards of 1998–2004. Every licence is allowed; keep notices, origin lines and credits.
- **Contracts:**
  - `docs/design/video-driver-families.md`, the section "NVIDIA TNT/TNT2, GeForce256/2/3/4, FX and GeForce6 — choose X nv" and sections 5, 6 and 10;
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
   - The DM9 hardware check proves native execution against the software engine.
3. **Tests.** Host fake-register tests cover the register sequences of every command (fill, copy in both overlap directions, sync and timeout, cursor), the probe and refusal paths, and quarantine. Add golden comparisons against the software engine where the model allows. **First, write the transport contract** (lead decision 2): the DMA pushbuffer or FIFO transport, object setup, VRAM placement and wrap, and fence or notifier rules, into the contract section. The lead reviews it from the report; implement the engine behind it, with NV40 and later excluded (f2-105).
4. **Records.** Device Manager shows the driver's display name, and the Control Panel shows it as the 2D driver. Add a row to the contract's family table with the result.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, register references, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f2-104-report.md`.

## Amendment 1 (lead, 2026-10-23 01:19): transport approved

The lead reviewed the f2-104 transport subsection in `video-driver-families.md` and approves it as written.
- **Transport:** channel 0 PIO, no bus mastering, host DMA, GART or command-fetch DMA object.
- **Credits:** method groups reserve FIFO credits.
- **Completion:** an empty FIFO plus PGRAPH idle with posted-read ordering.
- **Scope:** NV4–NV2x only, with the stated refusals.

1. **Enable activation.** Set `transport_reviewed` from this approval, with a comment citing this amendment. The DM9 hardware check at activation is the runtime guard: a mismatch against the software engine quarantines the driver and keeps software rendering after the idle proof.
2. **Tests.** Update `nv2d_adapter_test.c` so the approved path is tested: activation, the DM9 pass and a DM9 mismatch leading to quarantine. Keep the refusal paths (UMA, layouts, heads, scanout start) tested through their own predicates rather than through the review constant.
