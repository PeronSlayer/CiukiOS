# Handoff — SR-VIDEO-002 Milestones M-V2.0..M-V2.3

**Date:** 2026-04-17
**Scope:** Flicker-free video baseline, 2D rasterizer, BMP decoder, stable 2D graphics services ABI.
**Version bump:** `CiukiOS Alpha v0.7.1` → `CiukiOS Alpha v0.8.0`
**Branch:** `feature/copilot-sr-edit-001`

## Context and goal
Subroadmap `docs/subroadmap-sr-video-002.md` was authored in the previous session to address persistent flicker, slow UI, and the lack of a 2D drawing API / image pipeline needed for the DOOM target. This session executed the first four milestones to deliver the flicker-free baseline plus all drawing primitives that COM programs (and eventually DOOM) require, without yet touching INT 10h/VBE mode switching (deferred to M-V2.4).

## Files touched

### New files
- `stage2/include/gfx2d.h`
- `stage2/src/gfx2d.c`
- `stage2/include/image.h`
- `stage2/src/image.c`
- `com/gfxsmoke/gfxsmoke.c`
- `com/gfxsmoke/linker.ld`
- `docs/handoffs/2026-04-17-sr-video-002-milestones-v20-v23.md` (this file)

### Modified files
- `boot/proto/services.h` — added `ciuki_fb_info_t`, `ciuki_gfx_services_t`, and `const ciuki_gfx_services_t *gfx` field on `ciuki_services_t`.
- `stage2/src/shell.c` —
  - finished migrating remaining `video_present*` sites from M-V2.0 (shell_int21_read_char_blocking, AH=0Ah buffered input, stage2_shell_run interactive loop, desktop EXITING path) to `video_present_dirty_immediate` / `video_begin_frame` + `video_end_frame`.
  - added `shell_gfx` (test-pattern + info), `shell_image` (show), wired into command dispatch, added includes for `gfx2d.h` / `image.h`.
  - added `g_gfx_services` populated table and assigned to `svc.gfx` when launching COMs.
- `Makefile` — added `COM_GFXSMOKE_*` vars and build rules; added `$(COM_GFXSMOKE_BIN)` to `all`.
- `run_ciukios.sh` — added copy block for `GFXSMK.COM` into the FAT image.
- `stage2/include/version.h` — bumped version strings to `Alpha v0.8.0`.
- `README.md` — updated Current Version and Changelog.
- `CHANGELOG.md` — added v0.8.0 section.

## Decisions made
1. **Frame scope over global lock.** Rather than a single boolean compositor flag, kept the depth counter `g_frame_scope_depth` introduced earlier so nested `video_begin_frame` calls (e.g. splash → HUD helper) remain safe. `video_end_frame` uses `video_present_dirty_immediate` to bypass pacing and guarantee atomic commit on scene boundaries.
2. **Input echo stays immediate.** Per-char echo in AH=0Ah buffered input and stage2 shell input loops were kept on `video_present_dirty_immediate` (not frame-scoped), since each keystroke is an independent logical frame and wrapping them would require buffering.
3. **Single dirty rect per primitive.** All `gfx2d_*` calls delegate to existing `video_fill_rect` / `video_put_pixel` / `video_blit_row`, which already track dirty rects — so one call → one bounding box extension. Scene composition stays efficient.
4. **Clip rect as additional constraint, not replacement.** `gfx2d_set_clip` intersects with framebuffer bounds in `gfx2d_effective_clip`, so overly large clip rects are harmless. All primitives clip via this helper.
5. **Alpha blit = masked blit for now.** True `OVER` requires a framebuffer read path (not yet exposed). For M-V2.1, `gfx2d_blit_alpha` treats alpha=0 as transparent and everything else as opaque write. True compositing deferred to M-V2.5.
6. **BMP decoder owns a static scratch.** `image_bmp_decode` writes into a single 8 MiB static buffer (`IMAGE_MAX_W * IMAGE_MAX_H`). Callers must consume the pixels before decoding another image. Avoids allocator coupling; upper bound matches backbuffer.
7. **Graphics ABI slot at end of `ciuki_services_t`.** Adding `gfx` as the last field before `reserved[]` (none today) keeps the layout extension-friendly without breaking existing COM binaries that ignore it (they see `NULL` if stage2 is older, but here stage2 always populates it).
8. **gfxsmoke uses same 0x600100 VMA + .bss-into-.data linker tweak as ciukedit.** Needed so `llvm-objcopy -O binary` produces a predictable flat image with no BSS gap at the COM entry.

## Validation performed
1. `make all` — clean build including new objects and `GFXSMK.COM` (full log captured during session).
2. Individual object builds verified:
   - `build/obj/stage2/gfx2d.o` — no warnings after final cleanup.
   - `build/obj/stage2/image.o` — no warnings.
   - `build/obj/stage2/shell.o` — no warnings.
3. Artifact existence confirmed: `build/stage2.elf`, `build/CIUKEDIT.COM`, `build/GFXSMK.COM` (928 bytes, sane COM size).
4. **Pending user validation (in QEMU):**
   - `gfx test-pattern` draws gradient quadrants + diagonals + centered filled circle + big triangle; serial contains `[gfx] test pattern v1 OK`.
   - `image show EFI/CiukiOS/<name>.BMP` renders BMP at frame center.
   - `run GFXSMK.COM` loads the COM, executes, emits `[gfxsmoke] OK` on serial, cleanly returns to prompt.
   - Splash/desktop/shell flicker-free end-to-end.

## Risks and mitigations
1. **Static scratch size.** `g_image_scratch` ~8 MiB in BSS. Inflates stage2 image, but no runtime cost and fits current memory map.
2. **No real alpha.** `gfx2d_blit_alpha` is a placeholder. Any caller expecting real OVER will see hard edges. Documented in source + this handoff; scheduled for M-V2.5.
3. **COM ABI growth.** Existing COMs compiled against the old `ciuki_services_t` will still work because the layout only grew at the end; new COMs that rely on `svc->gfx != NULL` must still null-check.
4. **Frame-scope discipline.** If any new caller forgets `video_end_frame` after `video_begin_frame`, the scene stays uncommitted and pacing-gated presents resume. Mitigation: keep wrappers tight; grep for mismatched pairs before every release bump.

## Next step
Proceed to **M-V2.4 — INT 10h / VBE mode-switching compatibility surface** (shell command `gfx mode set <W>x<H>x<bpp>`, `ciuki_gfx_services_t.set_mode`, detection table) and **M-V2.5 — palette / alpha / composite pipeline** (fb readback for `OVER`, 256-color palette indexable path used by DOOM). Then version bump to v0.9.0 and the DOOM graphics integration milestone.

## References
- Subroadmap: `docs/subroadmap-sr-video-002.md`
- Previous handoff: `docs/handoffs/` (earlier SR-VIDEO-002 authoring session)
- Related roadmap: `docs/roadmap-ciukios-doom.md`
