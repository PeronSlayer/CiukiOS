# Copilot Task Pack - Video Mode Stack v1

## Branch Setup (mandatory)
Work only on dedicated branch:

```bash
git fetch origin
git switch -c feature/copilot-video-vmode-stack-v1 origin/main
```

## Mission
Implement a heavy video-stack milestone for CiukiOS:
1. Improve rendering pipeline beyond the current minimal double-buffer.
2. Add a shell-launchable resolution utility (`vmode`) to select resolution up to current driver-supported max.
3. Make resolution selection deterministic and persistent across reboot.

## Context
- Runtime is post-ExitBootServices. Real GOP `SetMode` must happen in loader before ExitBootServices.
- Runtime "resolution change" must therefore be implemented as:
  - persist selection
  - reboot
  - loader applies mode at next boot.

## Strict Constraints
1. Do not break existing boot path, shell visibility, OpenGEM flow, INT21 tests.
2. Keep deterministic behavior in CI/headless QEMU.
3. No destructive git operations.
4. Preserve freestanding/kernel-safe coding style.
5. If ABI changes between loader and stage2, version and validate properly.

## Deliverables (all required)

### A) Video pipeline hardening (Stage2)
1. Introduce dirty-rect tracking in video driver.
2. Implement APIs:
   - `video_mark_dirty_rect(x,y,w,h)`
   - `video_present_dirty()`
   - `video_present_full()` (or keep `video_present()` as full and add dirty variant)
3. Ensure write paths mark dirty:
   - pixel write
   - fill rect
   - blit row
   - text draw/scroll paths
4. Use dirty present where safe in shell/desktop loops; keep force-present for critical transitions.
5. Add serial markers:
   - `[video] mode=double-buffer|direct`
   - `[video] present=dirty|full`
   - dirty rect bounds stats (lightweight debug)

### B) Driver limits + mode policy
1. Centralize driver-supported max resolution constants in one shared header:
   - `VIDEO_DRIVER_MAX_W`, `VIDEO_DRIVER_MAX_H`
2. Enforce policy:
   - loader prefers highest compatible mode within driver limits
   - if none, fallback to safe default/current mode
3. Keep robust fallback to direct rendering if backbuffer cannot fit.

### C) GOP mode catalog handoff (Loader -> Stage2)
1. Add compact mode catalog structure in handoff:
   - mode id
   - width
   - height
   - bpp
   - pixels per scanline
2. Cap entries to fixed max (e.g. 64).
3. Include active mode id/index in handoff.
4. Validate in stage2 and print markers:
   - `[video] gop modes=N`
   - `[video] active mode=...`
5. If ABI updated:
   - bump/version safely
   - strict compatibility checks
   - graceful fail with explicit panic reason on mismatch

### D) Persistent resolution config (Loader applies at boot)
1. Add config support:
   - path: `/EFI/CIUKIOS/VMODE.CFG`
2. Parse format:
   - `mode=<id>`
   - `width=<w>`
   - `height=<h>`
3. Selection precedence:
   - explicit mode id if valid
   - else width/height exact match
   - else fallback preferred policy
4. Validate against available GOP modes and driver limits.
5. Print loader diagnostics:
   - config found/parsed/invalid
   - selected mode source (config vs default)

### E) Shell utility: `vmode`
Implement shell command with subcommands:
1. `vmode help`
2. `vmode current`
3. `vmode list`
4. `vmode max`
5. `vmode set <mode_id>`
6. `vmode set <width>x<height>`
7. `vmode clear`
8. `vmode apply` (prints reboot required)

Behavior requirements:
1. `vmode list` shows mode_id, resolution, bpp, compatibility flag.
2. `vmode max` selects highest compatible mode and writes config.
3. `vmode set` validates and writes `VMODE.CFG`.
4. `vmode clear` removes/resets config.
5. UX message always explicit:
   - `Resolution change will apply after reboot.`
6. Add command to shell help and parser cleanly.

### F) Optional (preferred): tiny alias
1. Add shell alias:
   - `vres` -> forwards to `vmode`
2. Do not add fake COM wrapper unless truly functional with current runtime ABI.

### G) Tests and CI gates
1. Add script: `scripts/test_video_mode_pipeline.sh`
2. Add/extend tests for:
   - loader logs include mode catalog markers
   - stage2 logs include active mode markers
   - `vmode` command surface present in shell help
   - VMODE.CFG parsing valid/invalid behavior
3. Keep existing tests green:
   - `make test-stage2`
   - `make test-freedos-pipeline`
   - `make test-opengem`
   - `make test-gui-desktop`
4. Add Makefile target:
   - `test-video-mode`

### H) Documentation
1. Update README changelog with this milestone.
2. Add `docs/video-mode-management.md`:
   - architecture
   - config format
   - rationale for pre-EBS SetMode
   - troubleshooting
3. Update `Roadmap.md`:
   - mark video sub-roadmap progress
   - add next tasks for dynamic allocator/larger backbuffer strategy

## Implementation Quality Checklist
1. No unresolved TODO markers.
2. No dead code.
3. No warnings in normal build.
4. Safe behavior when `VMODE.CFG` absent.
5. Clear error output for invalid config/input.
6. Deterministic behavior in QEMU.

## Commit Strategy (mandatory)
Split into logical commits:
1. video dirty/present
2. handoff mode catalog
3. loader config parse/apply
4. shell `vmode` command
5. tests
6. docs

After each commit, run relevant tests.
Push branch at the end.

## Final Handoff (mandatory)
Create:
- `docs/handoffs/YYYY-MM-DD-copilot-video-vmode-stack-v1.md`

Include:
1. what changed
2. files changed
3. ABI changes
4. test results
5. known limits
6. next 5 recommended tasks
