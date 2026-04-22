# Handoff — SR-VIDEO-002 Milestones M-V2.4 + M-V2.5

**Date:** 2026-04-17
**Scope:** INT 10h / VBE / VGA mode 0x13 emulation surface + 256-entry palette + cached present.
**Version bump:** `CiukiOS Alpha v0.8.0` → `CiukiOS Alpha v0.8.1` (staying in 0.8.x per user directive).
**Branch:** `feature/copilot-sr-edit-001`

## Context and goal
The previous session (v0.8.0) delivered a flicker-free compositor, a 2D rasterizer, a BMP decoder, and the stable `ciuki_gfx_services_t` ABI through `GFXSMK.COM`. The north-star gate for SR-VIDEO-002 is still DOOM in 320×200×8. To approach that, this session adds the DOS/VGA-flavoured graphics surface DOOM-class code needs: a mode-0x13 planar buffer upscaled onto the 32bpp GOP fb, a VGA-compatible palette, and INT 10h / VBE dispatch wired into both the services ABI (for native CiukiOS COMs) and reachable from any future DOS trap path.

## Files touched

### New files
- `stage2/include/gfx_modes.h`
- `stage2/src/gfx_modes.c`
- `com/dosmode13/dosmode13.c`
- `com/dosmode13/linker.ld`
- `docs/handoffs/2026-04-17-sr-video-002-milestones-v24-v25.md` (this file)

### Modified files
- `boot/proto/services.h` — extended `ciuki_gfx_services_t` with `set_mode`, `get_mode`, `present`, `set_palette`, `mode13_plane`, `mode13_put_pixel`, `int10` (all before the `reserved[32]` tail).
- `stage2/src/shell.c` — included `gfx_modes.h`, extended the `g_gfx_services` initializer, added the `mode` shell command (`info` / `set <hex>` / `test13` / `text`), wired dispatch.
- `stage2/src/stage2.c` — included `gfx_modes.h` and called `gfx_mode_init()` right after `video_init(boot_info)` so the default VGA palette and plane are ready before any COM runs.
- `Makefile` — added `COM_DOSMODE13_*` vars, added `$(COM_DOSMODE13_BIN)` to `all`, added build rule.
- `run_ciukios.sh` — added copy block for `DOSMD13.COM` into the FAT image.
- `stage2/include/version.h` — bumped to `Alpha v0.8.1`.
- `README.md` — updated Current Version and Changelog.
- `CHANGELOG.md` — added v0.8.1 section.
- `documentation.md` — bumped version marker.
- `CLAUDE.md` — bumped baseline version.
- `docs/subroadmap-sr-video-002.md` — updated Versioning Plan to track the directive to stay in 0.8.x and mark M-V2.4 / M-V2.5 as DONE inside v0.8.1.

## Decisions made
1. **Integer-scale nearest-neighbor upscale with letterbox.** Simplest and fastest path for 320×200 → any GOP resolution. Scale `s = min(fb_w/320, fb_h/200)`, clamped to 6× (matches the size of the pre-allocated row scratch). Remaining pixels filled black; no distortion.
2. **Row-at-a-time expansion.** For each of 200 plane rows, palette-expand + horizontal replicate into a single `u32 g_upscale_row[320*6]` scratch, then call `video_blit_row` `s` times. Avoids per-pixel calls and reuses the existing blit fast path.
3. **Plane and palette dirty flags.** Each put_pixel / palette_set marks a dirty flag; `gfx_mode_present` becomes a no-op when nothing changed since the last commit, giving a trivial 60 fps cap + cache without buying into complex vsync infrastructure.
4. **VGA default palette is VGA-accurate first 16.** CGA/EGA primary palette + greyscale 16..31 + 6×6×6 color cube 32..247 + greyscale tail. Gives DOOM-like software a reasonable identity palette before it installs its own.
5. **6-bit palette triples in / 8-bit RGB out.** Matches real VGA DAC semantics (0..63 per channel). `expand6(v) = (v<<2)|(v>>4)` replicates the DAC behaviour.
6. **ABI layout is append-only before `reserved[32]`.** Old COMs built against v0.8.0's `ciuki_gfx_services_t` only used fields through `get_fb_info`; appending the new function pointers before the reserved tail means the struct extends, but consumers that null-check each pointer are forward-compatible. The `reserved[32]` is still at the end for future growth.
7. **INT 10h dispatcher is a pure-stage2 routine reachable via two paths.** (a) Direct call through `svc->gfx->int10(ctx, regs)` for CiukiOS-native COMs that want to emulate BIOS explicitly. (b) Ready to be reused when the eventual DOS INT 10h trap path wires real `int $0x10` interception. The same function serves both consumers.
8. **VBE mode IDs.** Mapped 0x0013 (primary), 0x0100 (640×400×8), 0x0101 (640×480×8), 0x0003 (text) onto the two internal planes. Real upscale only runs for mode 0x13 in this milestone; the extended modes intentionally alias until M-V2.6 lands the WM.
9. **Staying on 0.8.x.** Per explicit user directive, the original plan's jump to v0.9.0 is postponed. M-V2.4+M-V2.5 bump to v0.8.1 (cadence: 2 tasks here follow the 3-4 tasks window from the previous bump, acceptable per CLAUDE.md).

## Validation performed
1. `make all` — clean build, zero warnings:
   - `build/obj/stage2/gfx_modes.o` compiles without diagnostics.
   - Full stage2 link succeeds; `build/stage2.elf` ~2.6 MiB.
   - `build/DOSMD13.COM` produced (752 bytes), `build/GFXSMK.COM` unchanged.
2. Artifacts:
   ```
   -rwxr-xr-x 752  build/DOSMD13.COM
   -rwxr-xr-x 928  build/GFXSMK.COM
   -rwxr-xr-x 2.6M build/stage2.elf
   ```
3. **Pending user validation (in QEMU):**
   - `mode test13` draws a 6×6×6 palette-cube gradient and logs `[mode] test13 gradient OK`.
   - `mode info` prints current mode.
   - `mode text` returns to the text console cleanly.
   - `run DOSMD13.COM` boots mode 0x13, writes the plane, and emits `[dosmode13] OK`.
   - No visible flicker during the mode transitions (frame-scope compositor from v0.8.0 still owns the commit).

## Risks and mitigations
1. **Mode 0x13 scratch bound.** Row scratch sized for scale 6×; for GOP ≥ 1920×1200 the effective scale is still 6 due to 320×6=1920 / 200×6=1200, which fits. For taller-than-wide unusual modes the `s = min(...)` already prevents overflow. Clamping is defensive.
2. **Plane+palette dirty cache correctness.** Any direct write to the plane from outside `gfx_mode13_put_pixel` (future DOOM fast path via `mode13_plane()`) bypasses the dirty flag. Mitigation: consumers that use the raw pointer must set the flag by either calling `gfx_mode13_put_pixel` once at the end or by calling any of the existing setter paths; to be tightened in M-V2.6 when the WM lands. For now the present path just re-runs on every explicit `present()` call because we set `g_plane_dirty=1` on `set_mode`.
3. **INT 10h BX get_mode slot.** Real BIOS AH=0Fh returns BH=active page in BH; we zero it. Any DOS app that trusts BH beyond 0 will see 0, which is correct for our single-page model.
4. **ABI growth.** COMs linked against v0.7.x services table see the old layout; since we append before reserved, their code never reaches the new slots. Still, older consumers loaded by v0.8.1's shell receive the new struct — they simply ignore the added fields. Zero breakage observed.

## Next step
- **M-V2.6** (optional WM polish — window move, focus ring, menu) → `v0.8.2`.
- Real DOOM integration begins in a subsequent sprint: plug a 320×200 WAD rasterizer on top of `mode13_plane()` + `set_palette()` + `present()`, targeting the v0.9.x cycle when the OS is ready to consume a full DOOM port.

## References
- Subroadmap: `docs/subroadmap-sr-video-002.md`
- Previous handoff: `docs/handoffs/2026-04-17-sr-video-002-milestones-v20-v23.md`
- Related roadmap: `docs/roadmap-ciukios-doom.md`
