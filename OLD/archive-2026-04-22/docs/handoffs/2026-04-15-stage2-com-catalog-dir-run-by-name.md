# HANDOFF - stage2 COM catalog + dir + run by name

## Date
`2026-04-15`

## Context
User asked to proceed after initial COM bootstrap, with a DOS-like flow where the shell can list available COM programs and execute them by name.

## Completed scope
1. Extended handoff ABI with a COM catalog:
   - `HANDOFF_COM_MAX`
   - `HANDOFF_COM_NAME_MAX`
   - `handoff_com_entry_t { name, phys_base, size }`
   - `handoff_v0_t.com_count` + `handoff_v0_t.com_entries[]`
2. Reworked UEFI COM loading from single hardcoded `INIT.COM` to directory catalog loading:
   - Enumerates `\EFI\CiukiOS`
   - Filters `*.COM`
   - Loads each COM below 4 GiB with `AllocatePages(AllocateMaxAddress)`
   - Fills handoff catalog entries
   - Keeps backward compatibility via legacy `com_phys_base/com_phys_size` (defaults to first COM, prefers `INIT.COM` when present)
3. Added DOS-like `dir` command in stage2 shell to list loaded COM programs.
4. Upgraded `run` command:
   - `run` (no args) executes default/legacy COM
   - `run <name>` resolves COM by name (case-insensitive, auto-appends `.COM` if omitted)
5. Updated serial readiness marker in stage2 to include new commands.
6. Updated stage2 boot test required marker accordingly.

## Touched files
1. `boot/proto/handoff.h`
2. `boot/uefi-loader/loader.c`
3. `stage2/src/shell.c`
4. `stage2/src/stage2.c`
5. `scripts/test_stage2_boot.sh`

## Technical decisions
1. Decision: COM filesystem discovery remains in UEFI loader (pre-ExitBootServices), not in stage2.
   Reason: stage2 currently has no AHCI/IDE block driver; UEFI file protocols are already available and stable.
   Impact: immediate `dir` and `run <name>` usability without adding low-level disk drivers yet.

2. Decision: Keep legacy fields `com_phys_base/com_phys_size` active.
   Reason: backward compatibility with existing `run` behavior and older assumptions.
   Impact: `run` with no argument remains functional.

3. Decision: Normalize `run` target names to uppercase with optional implicit `.COM`.
   Reason: DOS-like UX and simple matching against catalog entries.
   Impact: `run init`, `run INIT`, and `run init.com` all resolve consistently.

4. Decision: COM catalog load failures are warnings, not fatal boot errors.
   Reason: COM programs are optional at this stage; shell and kernel fallback should remain bootable.
   Impact: robust boot path even with missing/corrupt COM files.

## ABI/contract changes
1. `handoff_v0_t` now includes:
   - `uint64_t com_count`
   - `handoff_com_entry_t com_entries[HANDOFF_COM_MAX]`
2. Existing fields `com_phys_base/com_phys_size` are still populated for compatibility.
3. `HANDOFF_V0_VERSION` intentionally kept at `0` for now (same policy used in project so far).

## Tests executed
1. `make test-boot`
   Result: PASS (stage2 + fallback).
2. `make test-stage2`
   Result: PASS (required patterns found, forbidden patterns absent).

## Current status
1. Shell now supports `dir` and `run <name>` over COM programs discovered at boot.
2. `INIT.COM` still works with plain `run`.
3. Boot and fallback automation remain green.

## Risks / technical debt
1. COM names longer than `HANDOFF_COM_NAME_MAX` are skipped.
2. COM catalog is populated at boot-time only; no runtime disk refresh yet.
3. This is not yet a real stage2 FAT reader; discovery currently depends on UEFI filesystem services.

## Next steps (recommended order)
1. Add richer `dir` output (count summary, DOS-like formatting, optional sorting).
2. Add first disk I/O abstraction in stage2 (block read API) to prepare true runtime FAT support.
3. Introduce staged FAT parser in stage2 (start with FAT directory walk), then decouple from loader-side discovery.

## Notes for Claude Code
- Keep loader COM discovery before `ExitBootServices` and before handoff jump.
- Preserve legacy `com_phys_base/com_phys_size` while introducing new catalog fields.
- If extending the catalog ABI again, append fields and avoid reordering existing ones.
