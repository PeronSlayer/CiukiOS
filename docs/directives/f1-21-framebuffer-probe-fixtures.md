# Directive f1-21: `framebuffer` probe fixtures exceed the kernel heap's 2 KiB class

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`framebuffer` row: mode,
  masks, digest, guard errors, invalid-rectangle rejection, present/fill
  paths), `device-firmware-ownership.md` (fbdev owns the LFB mapping),
  f1-14's rule that probe fixtures do not depend on heap classes.
- **Implementer:** Codex, `gpt-6-luna`, effort `medium` (mechanical change
  plus a host guard; escalate to `gpt-6.1-sol` only if the allocator choice
  turns out to need a kernel interface change).
- **Worktree:** `wt/f1-fb-probe`, from `main` at or after `69d4d3a`. Files:
  `src/kernel/drivers/fbdev_probe.c`, the host test that exercises it under
  `tests/host/`, `scripts/test/host_kernel_tests.sh` if a new test binary is
  needed, `tests/host/record_scope_test.py` or a sibling static guard only to
  add the fixture-size check described below.

## Observed on image `3d79e6a7…` (commit `5e6b2a4`, `f1-input` suite, `qemu-t23`, 2026-10-11)

```
probe=framebuffer event=DATA group=mode mode=0144 width=1024 height=768 bpp=32 pitch=4096 absent=0 owner=fbdev generation=18
probe=framebuffer event=DATA group=masks red=8:16 green=8:8 blue=8:0 reserved=8:24
probe=framebuffer event=DATA digest=811c9dc5 guard_errors=0 errors=0 mode_calls=0 error=-12 absent=0
probe=framebuffer event=END status=FAIL reason=framebuffer_error
```

`digest=811c9dc5` is the untouched FNV-1a basis and `mode_calls=0`: the probe
failed before drawing. `fixtures()` in `fbdev_probe.c` asks
`kmalloc(SP * SH)` = (25·4+7)·21 = 2,247 bytes; `src/kernel/core/kheap.c`
serves power-of-two classes up to 2 KiB, so the request returns NULL and
the probe reports `-ENOMEM` (12). The other fixture buffers (975 + 64 bytes
and smaller) fit. The host build passes because the host `malloc` has no
such bound. The failure gates `framebuffer-no-lfb`, `firmware_overrun`,
`disallowed_io` and every `qemu-desktop-1998/2002` case of `f1-input`.

## What to do

1. Give the probe fixtures storage that does not depend on heap classes:
   static buffers sized from the existing `FW/FH/SW/SH/SP/GUARD` constants
   (the probe runs once per boot, so static storage is acceptable; the
   `input-fault` fixture made the same move in f1-14), or page allocation
   through the existing `mm` interface if the contract prefers it. Keep the
   guard bytes, sentinel checks and every present/fill/invalid-rectangle
   subcase exactly as they are. No change to records or to `fbdev.c`.
2. Add a host check that fails when any single probe fixture allocation
   through `kmalloc` exceeds the heap's largest class (a static source
   guard over `src/kernel/probes/` and `src/kernel/drivers/*_probe.c`, or a
   host build of the heap with the real class table that the fixtures run
   against). The check must catch the 2,247-byte request of the current
   tree before the fix.
3. Host tests: the existing framebuffer host test still passes; the new
   guard passes after the fix.

## Acceptance by the lead

Host tests; kernel build; on QEMU the `framebuffer` case of `f1-input`
passes on `qemu-t23`, `qemu-e500`, `qemu-min128`, `qemu-desktop-1998` and
`qemu-desktop-2002`, and `framebuffer-no-lfb` runs. Reply with: files, the
storage chosen, the guard's output on the unfixed tree, test output.
