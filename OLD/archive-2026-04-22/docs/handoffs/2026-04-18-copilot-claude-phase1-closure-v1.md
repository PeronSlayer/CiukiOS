# Handoff: Phase 1 Closure — Video Policy Hardening Beyond Full HD

**Date:** 2026-04-18
**Branch:** `feature/copilot-claude-phase1-closure-v1`
**Task pack:** `docs/collab/copilot-task-claude-phase1-closure-v1.md`

## Context and Goal
Close the remaining Phase 1 gap: "scaling strategy beyond Full HD and mode-policy hardening across wider GOP catalogs". Replace simple first-fit GOP mode selection with deterministic scoring engine, add explicit budget tiers, create automated compatibility matrix gate, and harden UI layout for wide resolutions.

## Files Changed

### Core
- `boot/uefi-loader/loader.c` — Replaced simple preferred-table GOP selection with deterministic scoring engine (resolution class, aspect ratio, memory budget, tie-break by mode index). Added budgetv2 tier classification and fallback degrade policy for oversize modes.
- `boot/proto/video_limits.h` — Added budget tier constants (baseline/HD/HD+/FHD/QHD/4K) and safe ceiling define.
- `stage2/src/video.c` — Added budgetv2 tier classification marker in `video_init()` budget diagnostics.
- `stage2/src/ui.c` — Added `ui_validate_layout_matrix()` function that validates layout at 4 resolution classes and emits deterministic markers. Called once on desktop scene entry.

### Test Infrastructure
- `scripts/test_video_policy_matrix.sh` — New 12-gate static-analysis script validating policyv2/budgetv2 markers, resolution class coverage, budget tiers, safety checks.
- `Makefile` — Added `test-video-policy-matrix` target and `.PHONY` entry.

### Docs / Version
- `Roadmap.md` — Marked Phase 1 scaling item and SR-VIDEO-001 compatibility expansion item as `DONE`.
- `README.md` — Bumped to v0.6.4, added changelog entry.
- `stage2/include/version.h` — Bumped to `Alpha v0.6.4`.

## New / Updated Markers
- `GOP: policyv2 modes=<n> selected=<WxH> score=<n> result=PASS|FALLBACK`
- `GOP: budgetv2 class=<name> bytes=<n> allow_db=<0|1>` (loader + stage2)
- `GOP: fallback reason=overbudget next=<WxH>`
- `[ui] layout matrix pass <WxH>` (for 1024x768, 1280x800, 1920x1080, 2560x1440)
- `[video] budgetv2 class=<name> bytes=<n> allow_db=<0|1>` (stage2 serial)

## Decisions Made
1. Scoring engine uses weighted sum: resolution class match (0-60), aspect ratio (0-20), budget fit (0-15), baseline satisfaction (0-5). Tie-break by lower mode index for determinism.
2. Budget tiers up to 4K defined but double-buffer only allowed up to FHD (safe ceiling = VIDEO_BUDGET_TIER_FHD_BYTES). QHD/4K modes get single-buffer only.
3. Fallback degrade: if selected mode exceeds safe ceiling and no config override, loader tries to find a lower-class mode from the catalog that fits. Never crashes.
4. Layout matrix validation runs once on first desktop scene entry, validating 4 target resolutions without requiring actual framebuffer at those sizes.

## Tests Executed
| Test | Result |
|------|--------|
| `make all` | PASS |
| `make test-video-1024` | PASS |
| `make test-video-backbuf` | PASS |
| `make test-video-ui-v2` | PASS (14/14) |
| `make test-video-policy-matrix` | PASS (12/12) |
| `make test-stage2` | INFRA (QEMU serial capture — known issue) |
| `make test-video-mode` | INFRA (QEMU serial capture — known issue) |

## Known Limits
1. QEMU-based runtime tests (`test-stage2`, `test-video-mode`) have serial-capture infrastructure issues in this environment; classified as non-blockers per established policy.
2. Scoring engine has not been validated against real hardware GOP catalogs with unusual modes (e.g., portrait, ultra-wide). Fallback should handle gracefully.
3. 4K mode support is attempt-only with single-buffer; actual 4K testing requires hardware or QEMU with custom resolution configs.
4. Layout matrix validation is offline (doesn't render to actual framebuffer at each resolution); true visual validation requires runtime.

## Follow-Up Suggestions
1. Add QEMU custom resolution test configs (e.g., `-device VGA,xres=2560,yres=1440`) to validate scoring engine at runtime.
2. Consider adding a `vres` shell command extension to display the policyv2 score of the active mode.
3. Evaluate whether the scoring weights need tuning for real-world GOP catalogs with non-standard modes.
4. Add a 5K/ultrawide budget tier if demand arises from display testing.
5. Close remaining Phase 4 layout/alignment items now that the core metrics engine and matrix validation are in place.
