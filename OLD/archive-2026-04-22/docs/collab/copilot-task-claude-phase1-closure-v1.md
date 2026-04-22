# Copilot Claude Task Pack - Phase 1 Closure (Video Policy Hardening > Full HD)

## Mandatory Branch Isolation
Claude must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-claude-phase1-closure-v1 origin/main
```

No direct commits on `main`. No force-push on shared branches.

## Mission
Close remaining `Phase 1` gap in `Roadmap.md`:
- `IN PROGRESS` scaling strategy beyond Full HD and mode-policy hardening across wider GOP catalogs

Target: move this item to `DONE` with deterministic runtime markers and automated gates.

## Scope (5 heavy tasks)

### P1-V1) GOP Mode Scoring Engine v2 (Wide Catalogs)
1. Replace simple first-fit selection with deterministic scoring:
- valid mode filter (pixel format, sane dimensions, pitch bounds)
- score by preferred resolution class, aspect-ratio proximity, and memory budget fit
- tie-breaker by stable mode index (deterministic)
2. Must support at least these target classes when available:
- 1024x768 (baseline)
- 1280x720 / 1280x800
- 1600x900
- 1920x1080
- 2560x1440
- 3840x2160 (attempt only if budget permits)

Markers:
- `GOP: policyv2 modes=<n> selected=<WxH> score=<n> result=PASS|FALLBACK`

### P1-V2) Backbuffer Budget + Safety Policy v2
1. Introduce explicit budget tiers by resolution class.
2. For oversized modes, keep `double buffer` if budget allows; otherwise deterministic degrade policy:
- try next lower mode class
- never crash; never allocate beyond safe ceiling
3. Emit clear reason markers for each fallback path.

Markers:
- `GOP: budgetv2 class=<name> bytes=<n> allow_db=<0|1>`
- `GOP: fallback reason=<...> next=<WxH>`

### P1-V3) Wide-Mode Compatibility Matrix Gate
1. Add script: `scripts/test_video_policy_matrix.sh`
2. It must validate in serial log the presence of policyv2 markers and acceptance/fallback behavior.
3. It must fail if:
- panic/#UD/Invalid Opcode appears
- policy marker missing
- selected mode is outside supported policy set
4. Add Make target: `test-video-policy-matrix`.

### P1-V4) Desktop/UI Resolution Hardening for Wide Modes
1. Ensure top bar, dock, shell viewport, info panes remain clipped/aligned at:
- 1024x768, 1280x800, 1920x1080, 2560x1440
2. Remove any hardcoded values that break at larger widths.
3. Keep text readable (font profile selection must still work).

Markers:
- `[ui] layout matrix pass 1024x768`
- `[ui] layout matrix pass 1280x800`
- `[ui] layout matrix pass 1920x1080`
- `[ui] layout matrix pass 2560x1440`

### P1-V5) Phase 1 Closure Wiring + Docs Sync
1. Update `Roadmap.md`:
- mark Phase 1 scaling/policy item as `DONE`
- mark SR-VIDEO-001 item `compatibility expansion above Full HD...` as `DONE`
2. Update README changelog with software-only entry.
3. Keep wording public-facing (no internal orchestration references).

## Constraints
1. Keep backward compatibility with existing `vmode` / `vres` behavior.
2. Do not remove existing markers used by current tests.
3. Keep deterministic output; no timestamps/random text.
4. Freestanding-safe code only.

## Validation (must run)
1. `make all`
2. `make test-stage2`
3. `make test-video-mode`
4. `make test-video-1024`
5. `make test-video-backbuf`
6. `make test-video-ui-v2`
7. `make test-video-policy-matrix`

## Final Handoff
Create:
- `docs/handoffs/YYYY-MM-DD-copilot-claude-phase1-closure-v1.md`

Include:
1. changed files
2. new/updated markers
3. tests executed + outcomes
4. known limits
5. follow-up suggestions (max 5)
