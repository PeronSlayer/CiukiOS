# f5-10C2 — the PE-to-native font bridge (glyphs drawn in the guest), the f5-10B glyph baseline fix with re-pinned frames, and the `w32-dialogs-qemu-t23` guest probe

- **Phase:** F5 (USER/GDI). **Model:** Claude Opus agent (Codex out of credit until 2026-10-25). **Worktree:** `wt/f5-10c2` from main.
- **Context:** `docs/directives/f5-10c-dialogs-controls-menus-fonts.md` amendment 1 items 3 and 5; reports `build/codex/f5-10c-report.md`, `f5-10b-report.md`, `f5-20b-report.md`. In the guest, gdi32 lays text out with the f5-10C font selection but draws no glyphs until a bridge carries glyph bitmaps from the native font engine (libfont/FreeType, `docs/design/fonts-contract.md`) to the PE-side gdi32; the f5-10B provider path `w32_gdi_native_font_open` reads libfont's glyph top as height above the baseline, so its glyphs land below the line.
- **Authority:** `sdk/win32/gdi32/` (font backend, provider path, bridge client), the native side of the bridge (the existing Win32 service or kernel call the f5-10B/f5-10C reports name for font access; no new syscall without a recorded reason), `tests/host/win32/` (gdi32, dialogs), the pinned frames those tests own, `src/probes/` or `apps/` per the probe conventions for the guest probe, `tests/suites/f5-core.json` (the `w32-dialogs-qemu-t23` case predicates), `docs/design/fonts-contract.md` (bridge section).

## Required

1. **Bridge.** A bounded, cached path that gives the PE-side gdi32 rasterised glyphs (8-bit coverage, metrics, advance, ascent/descent) for the faces f5-10C selects (Liberation aliases, the dialog font plan), within the 2 MiB glyph/face budget f5-10C measured; no font file is mapped into a Win32 process beyond what the contract allows; refusals typed (W21) where a face or size is unsupported.
2. **Baseline.** Fix `w32_gdi_native_font_open` (and any other path with the same reading) so glyph tops are converted to the Win32 convention; re-pin every affected f5-10B/f5-10C text frame after a visual check, old and new PNGs in `build/codex/f5-10c2-frames/`.
3. **Guest probe.** A guest fixture that opens a dialog with each control class, a menu and a MessageBox through the production personality and emits trusted `CIUKI_TEST` records via the probe mechanism (fw_cfg selection; application stdout is not evidence), plus a dry-run of the `w32-dialogs-qemu-t23` predicates against the fixture's host run. The case stays unrun by you.
4. **Evidence.** Host tests for the bridge (cache hits, budget, eviction, refusals), the baseline (a pinned glyph-position test), the full Win32 suite, `make CAP= apps`, `python3 scripts/gen_api_reference.py --check`. Report `build/codex/f5-10c2-report.md`.

## Constraints

rg and sed, never semble; no QEMU, no systemd-run, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; never disable sanitizer leak detection where the repository script does not; no Microsoft font files; no new `panic()` paths; English.

## Lead review (2026-10-20 09:00): accepted, decisions

The delivery (worktree commit on `wt/f5-10c2`, Claude Opus session, report `build/codex/f5-10c2-report.md`) is accepted: w32srv rasterises glyphs with libfont/FreeType for every Win32 process and gdi32's bridge backend fetches them over the existing w32srv channel (three new w32srv requests, no syscall, no font file mapped into a Win32 process); `w32_gdi_native_font_open` uses the Win32 baseline convention, ten f5-10B frames re-pinned after a visual check, the 22 f5-10C frames byte-identical through the bridge; `dialogs-exercise.exe` and the `f5:w32-dialogs` kernel probe with eight predicates, dry-run passed. Decisions:

1. **System fonts directory.** Interim: gdi32 reads system faces from `C:\System\fonts`. The Windows-compatible `C:\WINDOWS\FONTS` view (installed and system faces together, the target of `AddFontResource` copies and of installers) is implemented with the installer flows in f5-15A.
2. Edits outside the listed authority are accepted (drop-down combo sort fix, w32srv loop yield, probe list, fixture build, `w32/font_stats.h`).
3. Font budget split accepted: 1,572,864 bytes for open faces, 512 KiB for the engine and FreeType's working memory.
4. Stale core hashes in `config/wine-upper.json` are refreshed automatically once the cached upper build joins `make build-full` (follow-up f5-20D, with msvcrt staging).
