# OPENGEM-011 — DOS extender readiness probe

## Context and goal
OPENGEM-010 landed the dispatch-target telemetry and reordered the probe list. Follow-up from its handoff: the real `GEM.EXE` is selected, but `shell_run()` cannot dispatch a 16-bit MZ binary without an extender layer (DPMI / DOS4GW).

OPENGEM-011 is the **observability baseline** for that extender layer. It wires a readiness probe that exercises the in-process INT 2Fh AX=1687h DPMI installation-check handler and publishes a frozen marker set, without yet attempting real protected-mode dispatch.

## Files touched
- `stage2/src/shell.c` — new `stage2_opengem_probe_extender()` + `shell_write_u16_hex()` helper; invocation in `shell_run_opengem_interactive()` after the OPENGEM-010 dispatch marker and before `shell_run()`.
- `scripts/test_opengem_extender.sh` — new gate (13 OK / 0 FAIL).
- `Makefile` — new target `test-opengem-extender` between `test-opengem-dispatch` and `test-doom-via-opengem`.
- `docs/opengem-extender-readiness.md` — contract.
- `documentation.md` — item 21.

## Decisions
1. **In-process probe, not a real interrupt.** The probe calls `shell_com_int2f(NULL, &regs)` directly. This avoids taking a real INT 2Fh trap yet still exercises the production handler. If the handler shape changes, the gate catches it.
2. **Frozen marker set, append-only.** Four markers, exactly named as in the contract doc. Downstream phases may add more but must never rename or reorder these.
3. **Flags word is compact and explicit.** Bits: 0=installed, 1=CX!=0 (host-data size), 2=ES!=0 (entry seg), 3=DI!=0 (entry off). Rendered as `0x` + 4 lowercase hex digits. Rationale: a test can assert well-formedness without depending on the internal stub register layout.
4. **Return value advisory.** `stage2_opengem_probe_extender()` returns 1/0 but `shell_run_opengem_interactive()` casts to `(void)`. OPENGEM-012+ will consume it to gate real dispatch.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — build OK.
- `make test-opengem-extender` — **13 OK / 0 FAIL**.
- Full regression (all PASS):
  - `test-opengem-dispatch`
  - `test-opengem-real-frame`
  - `test-opengem-full-runtime`
  - `test-opengem-smoke`
  - `test-opengem-launch`
  - `test-opengem-input`
  - `test-opengem-file-browser`
  - `test-bat-interp`
  - `test-doom-via-opengem`
  - `test-gui-desktop`
  - `test-mouse-smoke`
  - `test-opengem`

## Risks
- The DPMI INT 2Fh stub returns a skeleton descriptor; a real DPMI client would deref the `ES:DI` entry pointer and crash since the entry isn't real code. OPENGEM-012 must either install a real stub or short-circuit at dispatch time.
- Marker prefix `OpenGEM: extender dpmi installed=` is now a public contract — any change requires a documented deprecation path.

## Next step suggestion
- OPENGEM-012: promote readiness from observability to dispatch. Candidate work items:
  1. Add a dedicated `shell_run_from_fat_abs(absolute_path)` path so `GEM.EXE` can be loaded from `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` (today `shell_run_from_fat` only takes an 8.3 basename and prepends CWD/roots).
  2. Install a minimal real-mode DPMI entry at `ES:DI` that the probe currently advertises.
  3. Gate end-to-end: `desktop frame blitted` should fire on a real dispatch.

## Branch + commit
- Branch: `feature/opengem-011-extender-baseline` (from OPENGEM-010 tip `e3f220b`).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
