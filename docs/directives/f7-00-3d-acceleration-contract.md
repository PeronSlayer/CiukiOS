# f7-00 — 3D acceleration contract: the SuperSavage/Savage4 engine driven natively

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f7-00` from `main`. Research and contract only, no code under `src/`. Owner directive 2026-10-14: "the graphics drivers must be done properly; the GPU must be able to run 3D applications". F7 was planned after F5; the owner wants the 3D path started now in parallel.

## Task

1. Research the S3 Savage4 / SuperSavage (IX/C in the T23, 5333:8c2e) 3D engine from primary sources: the Savage4 register reference (RE046-B), the Mesa/DRI `savage` 3D driver (MIT licence; `src/mesa/drivers/dri/savage` of Mesa 6.x–7.x) and the Linux DRM `savage` module (GPL; BCI command buffers, DMA, aperture layout), the XFree86 `savage` 2D driver: BCI command stream format, state registers for the 3D pipeline (vertex formats, texture descriptors, z-buffer, blend/alpha, S3TC), the command buffer and DMA/AGP path, the page-flip and double-buffer rules, what SuperSavage differs in (two pipes, 2× texture units, the "IX/C" integrated memory). Cite every source with URLs.
2. Write `docs/design/f7-3d-acceleration-contract.md`: (a) the kernel driver side (engine ownership shared with the 2D accelerator of f1-27/f1-49, BCI command submission through a ring with bounded latency, DMA or MMIO path, framebuffer/z/texture memory management in the 16 MiB of the T23, fault containment and quarantine, the presentation lease so a full-screen 3D client owns the scanout); (b) the user-side API: a native immediate-mode 3D library (`libciuki3d`: contexts, vertex buffers, textures, depth test, blending, flat/Gouraud/textured triangles) consumed by the Graphics Demo first and by the F5 DirectX/OpenGL personalities later (Direct3D 6/7 immediate mode and OpenGL 1.1 mapping), with a software fallback so the same program runs on any VBE card; (c) acceptance: the Graphics Demo cube and a textured scene at 1024x768 ≥ 60 fps on the T23, QEMU without a 3D model only runs the fallback; (d) the directive plan f7-01…f7-06 with estimates.
3. Honest expectations: what the SuperSavage IX/C can and cannot do (fill rate, no T&L, texture sizes), and what the alternative for other machines of the baseline is (software rasteriser with SIMD-free fixed point; later other engines).

## Acceptance

Contract document complete with sources; no production code.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble. Report `build/codex/f7-00-report.md`.
