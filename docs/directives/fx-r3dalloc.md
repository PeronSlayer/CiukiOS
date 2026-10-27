# fx-r3dalloc: Savage 3D qualification allocates from pages, and no kernel kmalloc can exceed the heap classes (T23 trial 6c3b7800)

- **Written:** 2026-10-27 09:45 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main 0b9381696.
- **Evidence:** owner's T23, image 6c3b7800. Private log `legacy/local/physical/6c3b7800/disk/LOGS/BOOT.LOG` in the lead's checkout (read it, never copy it): every background qualification ends `[3d] probe phase=prepare index=0 state=end ... error=-12` and `[3d] probe result=blocked reason=scratch error=-12`, with 11.8 MiB of free VRAM and 437 MiB of free RAM. Hardware 3D therefore never ran on the T23; Quake III and the Graphics Demo used software OpenGL.

## Cause

`src/kernel/core/kheap.c` serves only power-of-two classes up to 2048 bytes including its header (largest request 2040 bytes) and returns 0 above that. `src/kernel/drivers/video/s3savage/savage3d_owner.c` allocates with `kmalloc` the 264,480-byte `struct render3d_service` (`scratch_create`, line ~405), every render3d host allocation of the probe (`host_alloc`, line ~396) and the 256/128-square texture staging (`kmalloc(base*base*4*2)`, line ~423). The host tests link `kmalloc` to `malloc`, so they never saw it. The Win32 render3d transport (`src/kernel/core/render3d_transport.c`, `ralloc`) already uses `desktop_buffer_alloc` and is not affected.

## Scope

1. **Probe allocations.** Allocate the probe service, its render3d host allocations and the texture staging from whole pages (the `desktop_buffer_alloc`/`desktop_buffer_free` pattern of `ralloc`, with its size prefix, or an equivalent page allocator that records the size), freed on every exit path including failures, revalidation and `fullsize_diagnostic`. Account the memory like the transport does if the probe has a budget.
2. **Compile-time guard.** In `src/kernel/include/ciuki/mm.h`, make a `kmalloc`/`kzalloc` call whose size is a compile-time constant above the largest heap class a build error (for example a macro over the real function with `__builtin_constant_p` and a function declared with `__attribute__((error(...)))`), with the limit derived from kheap.c's classes in one place. Build the whole kernel (`python3 scripts/build_kernel.py`) and fix or justify every hit. Then grep the kernel for `kmalloc`/`kzalloc` with non-constant sizes and list in the report each one that can exceed the limit, with its fix.
3. **Host fidelity.** Every host harness that links kernel code with a substitute `kmalloc` (find them under tests/host) must refuse requests above the same limit, from one shared header, so the host tests fail exactly where the kernel fails. Expect some host fixtures to break; fix the kernel code, not the limit.
4. **Tests.** A host case that runs the Savage background qualification `prepare` phase with the kernel heap limit in force and reaches the next phase; the existing render3d, savage family, GL and display suites stay green; `scripts/test/host_kernel_tests.sh` passes.

Report to build/codex/fx-r3dalloc-report.md. Leave changes uncommitted.
