# OPENGEM-010 — Dispatch-target telemetry + probe reorder

## Context and goal
Runtime test of OPENGEM-009 showed `OpenGEM: runtime session duration=0 ms` because the probe list picked `/FREEDOS/OPENGEM/GEM.BAT`, and the bundled BAT only runs real GEM when OpenGEM is at the **drive root** (`\GEMAPPS\GEMSYS\GEMVDI.EXE`). CiukiOS ships OpenGEM under `/FREEDOS/OPENGEM/…`, so GEM.BAT short-circuits to an install-instructions stub. Downstream: no mode-13 entry, no `desktop frame blitted` marker, 0 ms duration.

OPENGEM-010 fixes the dispatch path and adds explicit telemetry so the selected binary is observable in the boot log.

## Files touched
- `stage2/src/shell.c` — probe list grew to 6 entries with `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` at position 0; introduced `paths_count` to decouple loop bound from hard-coded `5`. New telemetry marker `OpenGEM: dispatch target=<path> kind=<bat|exe|com|app>` emitted between arm+baseline and `shell_run()`.
- `scripts/test_opengem_dispatch.sh` — new gate (7 OK / 0 FAIL).
- `Makefile` — new target `test-opengem-dispatch`.
- `docs/opengem-dispatch-telemetry.md` — contract.
- `documentation.md` — item 20 added.

## Decisions made
1. **Nested GEM.EXE first, BAT second.** Non-destructive: BAT still in the list as secondary fallback for alternative payload shapes. Gives the real GEM binary precedence under the CiukiOS layout.
2. **Kind inferred from trailing 3 chars.** No filesystem/MZ inspection needed; the path has already been FAT-resolved by `fat_find_file`. ASCII-fold is sufficient because all probe paths are ASCII 8.3.
3. **Marker emitted before `shell_run()`, after arm/baseline.** Keeps the ordering: arm → dispatch-target → `shell_run` → disarm + duration → `session_ended`. A runtime gate reading the log can map duration/ms back to the path that caused it.
4. **`paths_count` constant** instead of `sizeof(paths)/sizeof(*paths)` — consistent with rest of shell.c style and avoids a macro expansion inside a function with no `stddef.h`.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — OpenGEM IMAGE build OK.
- `make test-opengem-dispatch` — **7 OK / 0 FAIL**.
- Regression sweep (all PASS):
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
- Custom OpenGEM payloads with different entry layouts may still hit GEM.BAT. The `kind=bat` marker makes this explicit.
- GEM.EXE is a 16-bit DOS MZ binary; dispatching it does not guarantee mode-13 entry without a working DOS extender path. The dispatch marker surfaces the attempt; the downstream `desktop frame blitted` marker stays the source of truth for "a real frame happened".

## Next step suggestion
- OPENGEM-011: wire the DPMI / DOS4GW path so GEM.EXE can actually run to mode-13 entry, turning `duration=0 ms` into a non-zero wall-clock. This is the start of the real "boot to OpenGEM desktop" milestone.

## Branch + commit
- Branch: `feature/opengem-010-gem-bat-dispatch` (from `feature/opengem-009-pit-duration` @ `c8770ba`).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
