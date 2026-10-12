# f1-23 — Display information and runtime mode switch (kernel, loader, SDK)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-23` (branch `wt/f1-23` from `main`). Host-testable parts (bootinfo parsing, mode table validation, syscall marshalling, firmware-VM call model in the existing host harness); the lead qualifies the real VBE switch on QEMU (`-vga std`) and the T23/E500.
- **Pairs with f2-37** (Control Panel UI) which codes against the interface below; keep it exactly.

## Owner request (2026-10-12)

Display properties must manage resolution and colour depth like an ordinary OS and show screen and GPU information.

## Interface (SDK header `src/sdk/include/ciuki/display.h`, kernel call 74 `ciuki_display`)

```c
#define CIUKI_DISPLAY_VERSION 1
struct ciuki_display_mode { uint16_t id, width, height, bpp; uint16_t pitch; uint8_t linear, current; };
struct ciuki_display_info {
    uint32_t version, size;                 /* caller sets both */
    uint16_t width, height, bpp, pitch;     /* active framebuffer */
    uint32_t vram_bytes, vbe_version;       /* VBE 2.0+ TotalMemory * 64 KiB, BCD version */
    char oem[64], vendor[64], product[64];  /* VBE OEM/vendor/product strings, ASCII, NUL-terminated */
    uint16_t pci_vendor, pci_device, pci_class, pci_subclass;   /* display adapter from the registry, 0 if unknown */
    uint32_t mode_count;                    /* modes available in modes[] (at most 32) */
    struct ciuki_display_mode modes[32];
};
int ciuki_display_query(struct ciuki_display_info *);    /* any process; -EINVAL on version/size mismatch */
int ciuki_display_set(uint16_t mode_id);                 /* desktop (PID of the supervisor's desktop) only; 0 or -errno */
```

## Scope

1. **Loader → bootinfo.** The loader already scans VBE modes to choose the boot mode; export the scanned table (id, width, height, bpp, pitch, linear flag; 640x480..1600x1200, 16/24/32 bpp, linear framebuffer only) plus the VBE controller strings and total memory into bootinfo (new tagged block; bump the bootinfo version with backward compatibility; the loader models in `scripts/test/loader_model.py` and the host tests follow).
2. **Kernel.** `ciuki_display_query` from bootinfo and the PCI registry (class 03 adapter). `ciuki_display_set`: switch through the firmware VM (`src/kernel/vm/biosvm.c`, VBE 4F02 with the LFB bit, then 4F01 to confirm pitch/base) with device quiescence as the firmware-ownership contract requires (`docs/design/device-firmware-ownership.md`); on failure restore the previous mode and return -EIO; update the framebuffer device (new base, pitch, geometry) and publish a `display changed` event on the display descriptor the desktop owns (the existing display grant/notification path; if there is none, add a readable change counter the desktop polls each frame). Serial log line `[display] mode=<id> <w>x<h>x<bpp> pitch=<n> result=<e>`.
3. **Desktop.** On a display change the compositor reallocates its screen buffer, repaints, re-lays out the bars, icons and windows (clamping windows into the new work area; CONFIGURE for resizable clients), and rebuilds the wallpaper cache. Persist the chosen mode id in `desktop.cfg` (`display_mode=<id>`, applied at the next start only if the id is still listed).
4. **Safety.** Mode set only from the desktop; refuse ids not in the table; refuse while a DOS window holds the display grant; a 10 s watchdog after a switch with no desktop frame restores the previous mode (the loader's boot mode is the fallback at the next boot: never persist a mode that did not produce a frame).

## Tests (host)

Loader model and bootinfo parsing with synthetic VBE tables; syscall validation (version/size/ids/rights); firmware-VM call sequence in the biosvm host harness with success, failure and restore; compositor relayout with two geometries; `desktop.cfg` round trip. Document the contract in `docs/design/f1-acceptance.md` (amendment) and `docs/desktop.md`.

## Constraints

clang i686 `-Werror`, no QEMU, no image builds, no commits, no diary, `nice -n 19`. Report to `build/codex/f1-23-report.md` with the verbatim host summaries and what the lead must verify on QEMU/T23 (the real VBE switch, the SuperSavage on the T23).
