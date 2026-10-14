# f2-63 — FreeType replaces stb_truetype for every text path (owner decision 2026-10-14)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f2-63` from `main`. Host-testable; the lead reviews the goldens and the T23.

## Input

`docs/design/fonts-contract.md` (stb_truetype 1.26 today, Liberation fonts, trusted store, no hinting), `docs/design/win32-personality-contract.md` Q10 and the owner's decision of 2026-10-14 ("FreeType everywhere"), the desktop/toolkit text rendering (`apps/libdeskui`, `apps/desktop`, the fonts SDK module), the font host tests (`tests/host/font`, coverage goldens), the Terminal and CiukNote text paths.

## Task

1. **Pin FreeType** (latest 2.13.x release, archive URL and SHA-256 in `config/sdk-pins.json`, FTL licence recorded with the notice shipped in `/system/licenses`), build it as part of the SDK for the i686 target with the minimal module set (TrueType, smooth rasteriser, autofit/TrueType bytecode hinting, no bzip2/png/harfbuzz), reproducible.
2. **Backend**: replace the stb rasteriser behind the existing fonts API without changing the API for applications; hinting on at UI sizes (light/normal: pick the mode that matches Windows 98 metrics best for the Win32 GDI path and document the choice), metrics (ascent/descent/line gap/advance) exposed with the precision GDI needs (26.6 fixed), glyph cache sizing bounded as today, kerning from GPOS/kern if cheap.
3. **Goldens**: re-record the font coverage goldens and every desktop/application golden that changes because of rasterisation differences (compositor, demo, Terminal, CiukNote, Control Panel), with before/after PNGs under `build/host/f2-63/` for the lead's review; keep the identity assets untouched.
4. **Host tests**: FreeType build reproducibility, rendering parity thresholds versus the previous rasteriser (per-glyph difference budget), metric tests for the Liberation faces at 8-16 px, memory budget of the cache.
5. Amend `fonts-contract.md` (dated subsection superseding the stb decision; the native fonts store stays trusted and immutable).

## Acceptance

Font, desktop and application host suites green with re-recorded goldens; image size growth recorded; the lead checks the PNGs and the T23 rendering. Files: SDK fonts module, `sdk/` pins and build scripts, `apps/libdeskui` text, tests/goldens. Not the kernel.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`), record links and decisions in the relevant `docs/design/` document (append dated subsections; never rewrite earlier text), then implement and test. Report `build/codex/f2-63-report.md`: findings, changes, host tests, what the lead must verify on QEMU and the T23.
