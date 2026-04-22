# OpenGEM DOS extender readiness probe (OPENGEM-011)

## Context
OPENGEM-010 added dispatch-target telemetry showing the nested
`GEM.EXE` is correctly selected by the probe order. However,
`shell_run()` cannot actually load a 16-bit MZ binary like the
OpenGEM `GEM.EXE` without a DOS extender surface (DPMI / DOS4GW).

OPENGEM-011 is the **observability baseline** for that extender
layer: it establishes the readiness probe, publishes a frozen
marker set, and prepares the ground for the actual protected-mode
dispatch work that lands in OPENGEM-012+.

## Change summary

1. **New helper** `stage2_opengem_probe_extender()` in
   `stage2/src/shell.c`:
   - Synthesizes the DPMI installation-check register file
     (`AX=1687h`, carry set).
   - Invokes the in-process `shell_com_int2f()` handler directly
     (no real interrupt dispatch — this is a probe, not a client).
   - Captures the result: `installed` flag (from carry clear) and
     a compact `flags` word packing installed / nonzero CX
     (host-data size) / nonzero ES (entry segment) / nonzero DI
     (entry offset).
   - Returns 1 when the stub responded cleanly, 0 otherwise.

2. **Frozen marker set** (append-only):
   ```
   OpenGEM: extender probe begin
   OpenGEM: extender dpmi installed=<0|1> flags=0x<hex16>
   OpenGEM: extender mode=<dpmi-stub|none>
   OpenGEM: extender probe complete
   ```

3. **Invocation**: `shell_run_opengem_interactive()` calls the
   probe immediately after the OPENGEM-010 dispatch marker and
   before `shell_run()`. Ordering is thus:
   `dispatch target` → `extender probe (begin…complete)` → `shell_run`.

4. **Small helper** `shell_write_u16_hex()` — 4-digit hex
   formatter used to render the flags word.

## Files touched
- `stage2/src/shell.c` — probe helper + invocation + hex formatter.
- `scripts/test_opengem_extender.sh` — new gate (13 OK / 0 FAIL).
- `Makefile` — target `test-opengem-extender`.

## Gate assertions (static)
1. `OPENGEM-011` sentinel present.
2. Probe function `stage2_opengem_probe_extender` declared.
3. Probe synthesizes `regs.ax = 0x1687U`.
4. Probe invokes `shell_com_int2f((ciuki_dos_context_t *)0, &regs)`.
5. All four markers present.
6. Both mode branches (`dpmi-stub`, `none`) emitted.
7. Invocation ordering: `dispatch → probe → shell_run`.
8. Marker ordering: `begin < installed < mode < complete` (first
   occurrence AWK probe; robust against the header comment).
9. Makefile target declared.

## Runtime (opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`)
- Probe begin + complete observable.
- Mode published (`dpmi-stub` or `none`).
- Installed + flags word well-formed (`installed=[01] flags=0x[0-9a-f]{4}`).

## Risks
- The probe uses an in-process direct call to the INT 2Fh handler
  rather than a real interrupt. If the handler signature drifts,
  the probe must drift with it. Tracked by the static check on
  `shell_com_int2f((ciuki_dos_context_t *)0, &regs)`.
- The flags word is an ad-hoc packing. OPENGEM-012+ may replace
  it with a structured readiness record; the marker **prefix**
  `OpenGEM: extender dpmi installed=` is the stable contract.

## Next step
- OPENGEM-012: promote the probe from observability to actual
  protected-mode dispatch — load GEM.EXE from the absolute path,
  relocate MZ, hand control via the DPMI-stub entry point.
