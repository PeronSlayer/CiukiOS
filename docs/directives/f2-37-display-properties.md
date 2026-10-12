# f2-37 — Control Panel Display: screen information, GPU information, resolution and colour depth

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-37` (branch `wt/f2-37` from `main`). Host-testable (libdeskui harness); the lead qualifies on QEMU and the T23.
- **Depends on f1-23** for the kernel side: code against the interface in `docs/directives/f1-23-display-modes.md` (`ciuki/display.h`, call 74). Until f1-23 lands, add the header with that exact layout in your worktree and a host stub returning a synthetic table; the lead merges f1-23 first.

## Owner request (2026-10-12)

"Nelle proprietà display voglio anche gestire risoluzione, bit ecc. come un normale OS; voglio le informazioni schermo, informazioni sulla GPU ecc."

## Scope (apps/control, Display page and a new "Adapter" tab)

1. **Settings subpage** (Display tab, under the preview): "Screen area" combo/list of modes from `ciuki_display_query` as `1024 x 768`, "Colours" list (16-bit High Colour, 24/32-bit True Colour) filtered by the available modes, Apply with a 15 s confirmation dialog ("Keep these settings?" countdown, revert on timeout like Windows), persisted through the desktop (`display_mode` in `desktop.cfg` via the DESK_SETTINGS protocol; the desktop performs `ciuki_display_set`).
2. **Adapter tab**: screen information (current mode, pitch, VBE version, video memory), GPU information (adapter name from a small table keyed by PCI vendor/device: S3 SuperSavage IX/C, ATI Rage Mobility M/P, VIA/Intel/NVIDIA/3dfx common 1998-2004 chips of the hardware baseline, fallback "Unknown adapter (vendor:device)"), VBE OEM/vendor/product strings, monitor "Plug and Play Monitor (no EDID yet)", a "Refresh" button.
3. System tab: add the display line to the summary.
4. Keep the Display page layout tidy (the preview and the five Position buttons stay); the mode controls go under the wallpaper controls or in a second column; render PNGs for the lead to `build/f2-37/*.png`.

## Tests

Control Panel host tests: mode list rendering from a synthetic table, filtering, Apply/confirm/revert flow (timer), DESK_SETTINGS message content, adapter table lookups, Adapter tab rendering; docs in `docs/desktop.md`.

## Constraints

No kernel change here; no QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f2-37-report.md`.
