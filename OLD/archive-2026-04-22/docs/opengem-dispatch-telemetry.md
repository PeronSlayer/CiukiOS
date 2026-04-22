# OpenGEM dispatch-target telemetry (OPENGEM-010)

## Context
The original probe order in `shell_run_opengem_interactive()` tried
`/FREEDOS/OPENGEM/GEM.BAT` first. In the bundled FreeDOS OpenGEM
payload, `GEM.BAT` checks for `\GEMAPPS\GEMSYS\GEMVDI.EXE` at the
**drive root** and prints install instructions otherwise — so when
OpenGEM is mounted at `/FREEDOS/OPENGEM/…` (CiukiOS layout), the BAT
never reaches the real binary. Result: session duration (OPENGEM-009)
reads `0 ms` because the BAT prints a few lines and returns without
entering mode 13.

## Change summary

1. **Probe reorder**: `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` is now
   the first entry. When the OpenGEM payload is installed, this
   resolves to the real GEM binary and bypasses the drive-root check
   in `GEM.BAT`. `GEM.BAT` remains in the list as a secondary fallback
   for compatibility.

2. **New telemetry marker**: immediately before `shell_run()` dispatch,
   the runtime emits

   ```
   OpenGEM: dispatch target=<absolute path> kind=<bat|exe|com|app>
   ```

   where `kind` is inferred from the resolved path's trailing 3
   characters, ASCII-folded to lowercase. This lets a runtime gate
   correlate the ms duration (OPENGEM-009) and the `desktop frame
   blitted` marker (OPENGEM-008) with the **actual** binary selected
   by the probe order, which was previously opaque.

## Files touched

- `stage2/src/shell.c` — probe list grew from 5 to 6 entries with the
  nested GEM.EXE at position 0; new dispatch-target marker emitted
  between the OPENGEM-008/009 arm+baseline block and `shell_run()`.
- `scripts/test_opengem_dispatch.sh` — new static gate (7 OK / 0
  FAIL). Static checks + opt-in runtime probe.
- `Makefile` — target `test-opengem-dispatch` inserted between
  `test-opengem-real-frame` and `test-doom-via-opengem`.

## Gate assertions

Static (always):

1. `OPENGEM-010` sentinel in shell.c.
2. Nested `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` present in probe
   list.
3. Dispatch marker prefix (`OpenGEM: dispatch target=`) present.
4. `kind=` token present.
5. Probe list ordering: nested GEM.EXE precedes GEM.BAT (AWK probe).
6. Emission ordering: arm → dispatch marker → shell_run call
   (AWK probe).
7. Makefile target declared.

Runtime (opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`):

- Presence of `OpenGEM: dispatch target=… kind=(bat|exe|com|app)`.
- Advisory: preferred resolution to nested GEM.EXE when payload
  is installed.

## Risks

- If a user replaces the bundled OpenGEM with one that uses a
  different entry layout (e.g. custom `AUTOEXEC.BAT` orchestrator),
  the dispatch may still hit GEM.BAT and no-op. This is documented
  and surfaced by the telemetry marker (`kind=bat` tells you exactly
  that).
- The trailing-3-char kind inference does not handle uppercase paths
  with non-ASCII characters. Paths in this probe set are all ASCII.

## Next step

- OPENGEM-011: once GEM.EXE dispatches, implement the DOS extender
  baseline (DPMI / DOS4GW) path that GEM.EXE will need to reach mode
  13 and trigger `OpenGEM: desktop frame blitted` with non-zero
  duration.
