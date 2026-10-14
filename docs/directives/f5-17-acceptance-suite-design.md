# f5-17 (milestone A) — Acceptance suite design: workload manifest, fixture media builder, input player and evidence records

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f5-17` from `main`. Host-only deliverable.

## Input

`docs/design/win32-personality-contract.md`: the directive paragraph "f5-17 — Acceptance suite" (line 1448), the acceptance set and gates sections (lines 1162-1275), the Codex cross-review section 4.2 row "f5-17A/B" and 4.4 (three labelled corpora: self-authored fixtures, freely distributable period demos, owner-local retail titles never committed), `docs/design/test-architecture.md` (runner, fw_cfg selection, overlays, one QEMU at a time) and `scripts/test/run.py` (probes, predicates, screendump heuristics).

## Task

1. **Workload manifest** (`tests/win32/workloads.json` + `scripts/win32/workloads.py`): for each approved title and reserve: provenance (URL, size, SHA-256, licence class: free demo / shareware / owner-local), installer entry point and silent/scripted install parameters where documented, the files expected after installation (names and sizes), the gate scene (what to start, how long, what to measure), disc requirement (none / ISO / track-aware image). Downloads go to `build/downloads/win32/` only; owner-local titles are referenced by path pattern and never fetched.
2. **Fixture media builder**: ISO 9660/Joliet images (and track-aware images with a Red Book audio track for the MCI cdaudio gate) built from a directory with Python only (extend the existing ISO fixture generator of f1-58's tests), deterministic, with host tests.
3. **Input player and screen checks**: the runner model for a scripted session (keyboard/mouse events with timing through QMP, screen-region digests and tolerance like `desktop_mark_visible`), selected by fw_cfg; design the `win32-*` probe records the kernel/personality will emit (process start, loader stages, DirectDraw mode, frames per second, audio underruns, input latency) and write their parser in `scripts/test/run.py` style as a separate module with tests (no runner behaviour change for existing suites).
4. **Evidence template** in `docs/validation/` for a title gate run (image SHA, title/version, scene, fps/underrun/latency, screenshots list, verdict) and the per-title operator script format for the owner's hardware tests.
5. Host tests for the manifest schema, the media builder and the record parser.

## Acceptance

Host tests green; manifest with at least the ten approved titles' provenance rows (unknown fields explicit); media builder produces a reproducible ISO the existing `test_optical.py` can mount.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search` is enabled), record links and decisions in `docs/design/` (amend `win32-personality-contract.md` only by appending a dated subsection to its review area, never rewriting earlier text), then implement and test. Report `build/codex/f5-17-report.md`: what was found, what changed, which host tests prove it, what the lead must decide or verify.
