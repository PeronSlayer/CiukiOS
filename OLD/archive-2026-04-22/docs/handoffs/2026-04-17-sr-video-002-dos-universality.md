# Handoff — SR-VIDEO-002 DOS universality expansion (v0.8.3)

**Date:** 2026-04-17
**Scope:** Wider DOS compatibility surface on INT 10h + DOOM-class blit primitives + palette read-back.
**Version bump:** `v0.8.2` → `CiukiOS Alpha v0.8.3`.
**Branch:** `feature/copilot-sr-edit-001`

## Context and goal
Make stage2's video stack as universally DOS-compatible as practical without crossing into v0.9.x. Real-mode DOS programs lean on a long tail of INT 10h sub-functions (cursor control, teletype output, scroll) and on column-major bitmap blits for mode 0x13 scrollers/FPS engines. This round fills those gaps.

## Files touched

### Modified
- `stage2/include/gfx_modes.h` — declared `gfx_mode13_blit_indexed`, `gfx_mode13_draw_column`, `gfx_palette_get_raw`.
- `stage2/src/gfx_modes.c` — implemented the three new helpers and massively extended `gfx_int10_dispatch`.
- `stage2/include/video.h` — exposed `video_get_cursor`.
- `stage2/src/video.c` — implemented `video_get_cursor` (reads `g_cursor_col` / `g_cursor_row`).
- `boot/proto/services.h` — appended `mode13_blit_indexed`, `mode13_draw_column`, `palette_get_raw` to `ciuki_gfx_services_t` (append-only before `reserved[32]`).
- `stage2/src/shell.c` — wired the three new pointers into `g_gfx_services`.
- `stage2/include/version.h`, `README.md`, `CHANGELOG.md`, `documentation.md`, `CLAUDE.md` — v0.8.3 bump.

### New
- `docs/handoffs/2026-04-17-sr-video-002-dos-universality.md` (this file).

## INT 10h coverage after this change
| AH | Function | Status |
|----|----------|--------|
| 00 | Set video mode | Real (mode 0x03 / 0x13). |
| 01 | Set cursor shape | Accept (soft no-op). |
| 02 | Set cursor position | Real (`video_set_cursor`). |
| 03 | Get cursor pos + shape | Real (via `video_get_cursor`); returns shape `0x0607`. |
| 06 | Scroll up window | Soft stub: homes cursor on full-clear (`al==0`). |
| 07 | Scroll down window | Same as 06. |
| 08 | Read char+attr at cursor | Returns space with gray attr `0x0720`. |
| 09 | Write char+attr × CX | Real: calls `video_putchar` CX times (attr ignored). |
| 0A | Write char × CX | Real: calls `video_putchar` CX times. |
| 0B | Set bg / palette color | Accept (soft no-op). |
| 0C | Write pixel | Real (mode 0x13). |
| 0D | Read pixel | Real (mode 0x13). |
| 0E | Teletype output | Real (`video_putchar`). |
| 0F | Get current mode | Real. |
| 11 | Character generator | Accept (stub). |
| 12 | Alternate select | Accept (stub). |
| 1A | Get display combination code | Returns VGA color (`BX=0x0808`). |
| 4F | VESA VBE 00/01/02/03 | Real (from v0.8.1). |

Carry stays clear on soft-accepted functions so DOS code paths that check CF proceed normally.

## Decisions made
1. **Scroll as cursor-home, not full memory scroll.** The framebuffer console doesn't retain a character buffer; synthesizing real scroll would require either adding one or re-rendering from history. Cursor-home on `al==0` covers the common "clear screen then draw" idiom; partial scroll stays a soft accept.
2. **`read char at cursor` returns space.** No text buffer → safest neutral answer (space + gray attr) that most programs treat as "empty cell", preventing replay-over-same-attr to corrupt output.
3. **Attr in AH=09h is ignored.** The framebuffer console currently binds `video_set_colors` globally. Mapping per-call `BL` to fg/bg would change global state for every write; rejected in favor of simple compatibility until a proper attr-backed console lands.
4. **`mode13_blit_indexed` has explicit transparent flag.** Matches DOOM's masked patches exactly: a specific palette index acts as the transparent color, and opaque blits are a simple `use_transparent=0` path with a tighter inner loop.
5. **`mode13_draw_column` takes contiguous `src`**, walking the plane with `d += GFX_MODE13_W`. DOOM's R_DrawColumn interfaces ultimately materialize a column byte array; stride = 1 is the shortest and fastest mapping. Callers that need fractional/textured sampling stay in user space.
6. **`palette_get_raw` returns 6-bit VGA triples.** Exact inverse of `palette_set`, lossless for any palette authored through the official setter.
7. **Append-only ABI growth.** New slots go before `reserved[32]`; consumers compiled against older headers stop reading at the previous end-of-table and never dereference unknown slots.

## Validation performed
1. `make all` — clean build, zero warnings, zero errors.
2. ABI size: `ciuki_gfx_services_t` still has `reserved[32]` tail; appended three function-pointer slots only.
3. **Pending user validation (QEMU):**
   - `run FADEDMO.COM` still passes (no regression from v0.8.2).
   - `run DOSMD13.COM` / `run GFXSMK.COM` still pass.
   - Any DOS shell workload that calls AH=02/03/09/0A/0E continues to draw text correctly.

## Risks and mitigations
1. **AH=09/0A ignoring attr.** Programs that rely on per-character color will render with the current fg/bg. Mitigation path: add an attr-aware console buffer in a later round.
2. **Scroll stub.** Programs expecting real scroll semantics (e.g. dialog boxes that scroll a sub-window) may see screen left dirty. Most DOS text programs also call `cls` (AH=06 AL=0 on full window) which we honor as "home cursor" — visually the old content persists but cursor position is correct. Acceptable until M-V2.6 WM polish.
3. **ABI growth.** Three more function pointers consume part of the safety margin inherent in `reserved[32]`. Still 29 pointers of headroom if we stay pointer-width on x86-64 — plenty for near-term expansion.

## Next step
- Optional: attr-aware console (enables real AH=09h color). Would unlock proper DOS text-UI rendering.
- Optional: DOOM-specific `mode13_blit_scaled` (for HUD patches + mid-wall textures with fractional scale).
- M-V2.6 desktop WM polish → still 0.8.x.
- DOOM port → v0.9.x (user gate).

## References
- Subroadmap: `docs/subroadmap-sr-video-002.md`
- Prev handoff: `docs/handoffs/2026-04-17-sr-video-002-palette-fade.md`
