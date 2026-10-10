# Directive f1-05: framebuffer device and presenter

- **Step:** F1. **Contracts:** `f1-acceptance.md` (probe `framebuffer`:
  overlapping, clipped, empty and edge rectangles with 24/32-bit and
  padded-pitch fixtures; pixels equal a reference renderer; canaries and
  padding unchanged; overflowing requests rejected; live pattern visible;
  mode identity unchanged; mode-call count 0; no-LFB boot succeeds),
  `boot-memory.md` (VBE copies in `ciuki_boot_info`, LFB mapping and
  reserve), `device-firmware-ownership.md` (the LFB is owned by the kernel;
  no VBE calls after boot).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-framebuffer`. Files: `src/kernel/drivers/fbdev.c`,
  `src/kernel/drivers/fbdev_probe.c`, `src/kernel/include/ciuki/fbdev.h`,
  `tests/host/fbdev_test.c`, `scripts/test/host_kernel_tests.sh` (add the
  test), `scripts/build_kernel.py` only if a new source directory needs
  listing. The console (`console.c`) is not changed in this directive; the
  device coexists with it by taking the LFB mapping from the boot info the
  same way (read `console.c` and `vmm.c` for the existing mapping helper).

## What to build

1. **Device model** (`fbdev.h`): one device describing the boot mode —
   width, height, bpp (24 or 32), pitch (may exceed width×bytes), colour
   masks from the VBE mode info copy, physical base, mapped virtual base,
   size, and a `mode_id` that never changes after boot. Registry claim of
   the LFB MMIO range as `fbdev`, activated with a generation; when the boot
   info says text mode or no LFB (`CBI_F_TEXT_MODE`), the device reports
   `absent` and every present call returns `-ENODEV`; nothing else fails.
2. **Presenter API**: `fbdev_present(const struct fb_surface *src, const
   struct fb_rect *dst)` copies a caller surface (32-bit XRGB in memory,
   arbitrary pitch) into the LFB with clipping to the screen, converting to
   24-bit when needed; `fbdev_fill(rect, colour)`; `fbdev_rect_valid` rejects
   overflow (negative sizes, coordinates beyond `INT32_MAX − size`, source
   pitch smaller than width); writes never touch bytes outside the
   destination rectangle, in particular the padding between pitch and
   width×bytes. The presenter uses only integer ops (no x87/SSE: the FPU
   audit runs on the kernel), writes rows with `memcpy`/32-bit stores, and
   keeps interrupts enabled (no `irq_save` around copies).
3. **Reference renderer** for tests: a separate, obviously-correct pixel
   loop (`fb_reference_present`) in the test file, not in the kernel.
4. **Probe** (`fbdev_probe.c`, `int probe_framebuffer(void)` returning 0 on
   PASS): allocate a shadow buffer with canary bytes around the screen
   region and padding, run the contract's rectangle set (overlapping,
   clipped at all four edges, empty, 1×1 corners, full screen, padded-pitch
   source) through the presenter into the shadow and through the reference
   renderer, compare digests, check canaries, then present a live pattern
   (colour bars with a 1-pixel frame) to the real LFB so the screen shows it
   for the photographed runs, and report `mode=`, `pitch=`, masks, `digest=`,
   `guard_errors=0`, `mode_calls=0`, and `absent=1` on a no-LFB boot (which
   passes). Expose the probe in `fbdev.h`; registration is done by the
   `CIUKI_F1_PROBE` macro if present in your base, otherwise left to the
   lead's plumbing (directive f1-03).

## Host tests (mandatory)

`tests/host/fbdev_test.c` under ASan/UBSan: present/fill against the
reference renderer for 24 and 32 bpp, pitches equal to and larger than
width×bytes, every clipping case, rejected overflow requests (no write at
all), and canary integrity, using a heap "LFB".

## Acceptance by the lead

Diff review; host tests; kernel build with the FPU audit; on QEMU the
`framebuffer` probe passes on `qemu-t23`, `qemu-e500` and in a text-mode
boot (`absent=1`), and the F0 suites still pass. Reply with: files,
interfaces (signatures), test output, and any contract problem found.
