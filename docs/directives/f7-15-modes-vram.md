# f7-15: Savage 3D on the T23's real configuration (desktop 1024x768x32, 15.5 MB VRAM, games changing mode)

- **Written:** 2026-10-26 23:31 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: wt/f7-1213 753ab5f35 (f7-12 and f7-13 integrated, with review fixes).
- **Evidence:**
  - The f7-13 report (build/codex/f7-13-report.md, "T23 retest") expects that with the T23 census (SuperSavage 5333:8c2e, 15,532,032 bytes of VRAM, desktop mode 280 at 1024x768x32, 2D cache vram_rows=1280) qualification ends in a clean `mode_heap`/-ENOMEM refusal. Its words: "not a GPU test or positive admission".
  - Qualification is invalidated by any display-generation change. Quake III full screen switches mode (r_mode 3 is 640x480, or the lease asks for the desktop mode), so even a qualified boot would lose hardware admission exactly when the game starts.
  - The owner wants Quake III accelerated on the T23 from the Start menu.
  - Private trial logs, read only: /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/.

## Scope
1. **VRAM partition.**
   - The 2D offscreen cache is a cache. When a 3D context opens and needs a heap (render targets, depth and textures for the requested size), the 2D owner yields offscreen rows through a bounded, revocable reservation. It gets them back when the context closes or loses the lease.
   - Follow the display driver model contract and the claimed-heap rules (guards, generations, no overlap with scanout or the cursor).
   - Measure and report the numbers for the T23 census at 1024x768x32, at 16 bpp and at 640x480.
2. **Qualification independent of the scanout mode.**
   - Qualify render-target sizes and formats (640x480, 800x600, 1024x768 in the qualified formats) offscreen, in the background job, with results kept per (device, VRAM layout generation) and not per scanout mode.
   - A later mode change re-validates only what the change can affect: heap placement, guards, scanout pitch for presentation copies. Use a short bounded check instead of a reboot.
   - Keep the attempt marker, the budgets and all the containment and restore rules.
3. **Full-screen games.**
   - When a full-screen lease changes the mode (BIOS VBE on the panel, after fx-biosvm), the GL context keeps or regains hardware admission without a reboot when the new mode's targets are qualified.
   - Presentation uses the GPU tiled-to-linear copy into the new scanout.
   - Test the Quake III H1 replay with a 640x480 full-screen lease from the 1024x768 desktop, and with the desktop-mode lease.
4. **Follow-ups from the f7-1213 report.**
   - Masked depth/stencil clears on the engine using BITPLANEWTMASK, as the Linux savage driver does (cite savage_state.c), replacing the CPU read-modify-write where the engine can do it.
   - A total time bound for normal-job readback while it holds the presenter.
   - A retry instead of refusal on a passing -EBUSY in the paced windows.
5. **Tests.**
   - The fake engine and the T23 census as a fixture.
   - Admission before and after the mode changes, with the desktop presenting throughout.
   - The existing render3d, Savage, GL/GDI, display, latency and owner suites.
   - Run host tests one at a time with nice -n 19.

Write the T23 procedure (Start menu launch of Quake III, the timedemo, the expected [3d] records) in build/codex/f7-15-report.md. Leave changes uncommitted.

## Lead amendment 1 (2026-10-27 02:47): review follow-ups and decisions

Round 1 is committed. Lead decisions on your questions: the 200 ms normal readback, 50 ms cached check and 2 s 2D-ready bounds and the cache-epoch semantics are accepted for the T23 trial (the trial records must show them). Then apply the items below; several may already be done by round 1 — verify each with file:line and implement what is missing:
Lead amendment for f7-15 after its first round (from the Opus review of f7-1213, /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f7-1213-review.md):
1. P2 budget (lead decision: adopt the reviewer's recommendation): reserve the attempt-marker clear time inside the 5 s total; an overrun inside an optional format gate (RGB565, D24/S8) with a restored, idle engine refuses only that format and keeps the qualified minimum and full-size results; the marker is cleared normally in that case (the marker covers the whole S3 Savage driver and a retained marker would also cost 2D acceleration on the next boot).
2. P2 D24/S8 partial clears: the engine's masked clear (BITPLANEWTMASK, Linux savage_state.c) replaces the CPU read-modify-write (already in this directive, item 4); the boot gate must test the same path the runtime uses.
3. P3: guard render3d_close(NULL) when the RGB565 scratch allocation fails; retry a transient -EBUSY from fbdev_render3d_enter instead of refusing (item 4); do not check the per-phase deadline after the last phase (cleanup/logging must not become budget_phase); return the texture pool after a refused D24 gate; fix the design-doc wording about register restore in runtime windows.
4. Main-line issue: runtime readback reads every row of every attachment per job under the presenter with the 1 ms timer restarting every 128 pixels (~0.9 M uncached reads per job at 640x480) — apply the yield-plus-absolute-deadline pattern and read back only what the job needs (item 4 total time bound).
5. Lead decision on the per-frame readback (from f7-14's report: every hardware frame reads colour and depth back into RAM, 1.8 MB at 640x480 and 4.7 MB at 1024x768, which limits the T23 frame rate): render targets stay GPU-resident; read back only when the CPU really needs the pixels (a software-fallback draw for that context, glReadPixels, a CPU present path); a frame that starts with a full clear never reads back the previous contents. Shared opengl32 code; coordinate with wt/f7-14 (native GL, committed) whose provider moved from sdk/win32/gdi32/render3d.c into sdk/win32/opengl32/provider.inc.

Report as 'Review round 1'. Leave changes uncommitted.
