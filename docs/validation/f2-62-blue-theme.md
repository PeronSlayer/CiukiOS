# F2-62 — Blue default and theme polish

## References and implementation decision (2026-10-14)

Research before implementation:

- [Microsoft GetSysColor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsyscolor)
  defines active/inactive caption gradient colours as left/right endpoints,
  pairs selection with selection text, and uses the 3D face for dialog backgrounds.
- [Microsoft DrawEdge](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawedge)
  describes raised/sunken edges through their inner and outer borders.
- [W3C contrast guidance](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html)
  supplies the relative-luminance calculation and the 4.5:1 small-text threshold.
  Use it as a conservative host palette check, including RGB565 quantization,
  rather than as a claim of whole-desktop accessibility conformance.

Repository comparison: Blue currently uses top/bottom caption endpoints,
glassy caption buttons, pale inactive caption text, and the shared Platinum
control bevels. Defaults in `cfg_defaults`, the initial theme pointer and
the missing-settings poll fallback currently select Platinum. The shell and
Control Panel load existing named choices rather than requiring a migration;
there is no separate registry theme default in the current implementation
(the desktop's master registry instance owns audio preferences). The theme
source of truth remains `desktop.cfg` and `cfg_defaults`.

Decision: keep all existing names/order, geometry, fonts, portrait and Tango
assets. Make Ciuki Blue the missing-settings default; preserve every valid
persisted choice. Give Blue horizontal active/inactive caption gradients,
dark inactive text, shared raised caption buttons, a coherent cool 3D face,
dark blue selection and focus, and the existing warm Start accent. Retain
the existing cached black icon-label halo and white labels on wallpaper.
Other explicitly selected themes must keep their compositor goldens.

Host renders are review material, not T23 or QEMU evidence. The owner's T23
request establishes the desired default and aesthetic direction; the f2-61
directive records a theme/wallpaper freeze on image `ce2e6ce9`. Those records
do not prove these new colours, runtime performance or 16-bpp scanout on T23.
Hardware validation and the separate freeze fix remain with the lead.

## Host validation

The RGB565 check uses the channel truncation of
`src/kernel/drivers/fbdev.c:pack_565` (5 red, 6 green, 5 blue bits). Samples
cover normal and warning captions, selection/menu gradients, ordinary/hot/
pressed buttons, dialog faces, disabled text, Start, task buttons and tray
text. Minimum text contrast is 4.639:1 at 32 bpp and 4.617:1 after RGB565
quantization. This is palette evidence; the paired RGB565 PNGs are simulated
review renders, not physical framebuffer captures.

Inactive warning captions use a light warm tint so that their new dark text
remains readable. Ordinary inactive caption buttons use the same raised
control painter as active caption buttons. Caption endpoints stay anchored
to the complete frame during clipping, including rounded corners.

The normal-boot screen observer now samples Blue's Start gradients as well
as the closed/open base colours. Its 8-channel-value tolerance and 40% Start
coverage threshold remain unchanged. Production closed/open frames and a
quantized closed frame are checked by the normal-boot host tests. The saved
desktop-colour sentinel stays unchanged to preserve existing configurations.

Compositor benchmark (`nice -n 19 python3
tests/host/desktop/test_compositor_performance.py --benchmark --frames 2`):
Blue was 0.979 cycles/pixel before and 0.992 after, with approximately
183/186 microseconds per warmed frame on this host. Two frames per sample
are a small, directional timing sample, not a T23 performance claim. Blue's
new raw-frame SHA-256 is
`4a16782346cdcd1590e9f1a928083d0413f6e98f29e7587851514f29756a6638`.
Platinum and Classic 98 compositor hashes remain unchanged. The desktop
reference frames and five Graphics Demo goldens are pinned for lead review.

Final serialized checks (`nice -n 19`): desktop discovery with
`-s tests/host/desktop -t . -v` ran 97 tests in 269.572 s, OK with five
native-build-dependent checks skipped. Normal-boot observer discovery with
`-s tests/host -p test_normal_boot.py -v` ran 14 tests in 11.977 s, OK.
The desktop suite includes incremental/full redraw and accelerated/fallback
parity, row-cache/reference equality, clipping, interaction/resize replay,
Control Panel default/persisted selection and all pinned frames. The observer
suite includes production closed/open Start frames and RGB565 conversion.
No QEMU, native image build, commit or diary entry was performed.
Review PNG paths and the remaining integration requirement are listed in
`build/codex/f2-62-report.md`.

## Integration requirement outside the directive's file scope

`apps/desktop/desktop.c:401` still calls `desk_theme_use("Platinum")` when
its asynchronous settings poll finds no theme. That would undo the fresh
Blue default after the first missing-file poll. The directive explicitly
says “Not `desktop.c`/wallpaper logic (f2-61)”; this file has therefore been
left untouched pending a scope answer. The lead must replace only that
fallback argument with `DESK_THEME_DEFAULT` when integrating with f2-61.
The host settings/default tests do not prove that excluded async path.
