# 2026-04-18 SR-VIDEO-002 WADVIEW interactive hook

## Context and goal
Advance the DOOM-prep graphics path in one session by turning `WADVIEW.COM` into a real WAD-backed interactive viewer, adding a multi-patch scene renderer, starting `TEXTURE1`/`PNAMES`/flat parsing, and hooking the preview path into the current DOS extender/runtime launch flow without bumping the public version.

## Files touched
1. `com/wadview/wadview.c`
2. `Makefile`
3. `run_ciukios.sh`
4. `documentation.md`

## Decisions made
1. Reworked `WADVIEW.COM` into a four-mode interactive viewer: scene, patch, texture, and flat.
2. Kept WAD access streaming-based and indexed only the needed directory subsets to stay compatible with the COM memory/offset model.
3. Used `PLAYPAL` for runtime palette setup and `PNAMES` + `TEXTURE1` to compose texture previews into a 64x64 indexed canvas.
4. Built the scene mode from real lumps: title/status patch selection plus navigable sprite rendering.
5. Attempted a dedicated preview `.EXE`, but removed it because the current tiny MZ wrapper format cannot carry a real WAD parser payload. The launch-path hook was instead implemented by chaining `CIUK4GW.EXE -> WADVIEW.COM -> DOOM.EXE` inside generated `DOOM.BAT`.

## Validation performed
1. `make build/WADVIEW.COM build/CIUK4GW.EXE`
2. `make all`
3. Editor diagnostics on modified files returned no errors.

## Risks and next step
1. `WADVIEW.COM` currently caps indexed patch/sprite/flat/texture lists to fixed small arrays and composes textures into a 64x64 preview canvas; larger WADs still need broader coverage and smarter caching.
2. The current extender/runtime hook is sequencing, not a true WAD-aware `.EXE` renderer, because of the present MZ wrapper size constraint.
3. Next step: either relax the MZ payload format or add a staged runtime handoff so a protected-mode preview binary can reuse the real WAD parsing/rendering path directly before `DOOM.EXE`.
