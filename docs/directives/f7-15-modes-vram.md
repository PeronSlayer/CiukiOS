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
