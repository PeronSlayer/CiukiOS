# Desktop Display Properties validation — 2026-10-07

The final HDD gates use the full image SHA-256
`3c0d1491700375bdb7aaa1072cbbb9d23b06c280e3adbc818408b5626739b2fa`,
256 MiB guest RAM, and SHELL.COM SHA-256
`bd9434408b67b668cae49b57ce0c5a922c2c066f600225af09511f161abba056`.
The harness assembles a matching shell/listing, exercises real mouse and key
input, and observes guest RAM without changing it. All results below are Linux
QEMU evidence; they do not qualify the physical T23 or a Windows launcher.

## Final resolution and wallpaper gates

`build/full/t23-vbe-fix/properties-photo-profiles-verified/provenance.json`
passes the 640×480, 800×600, 1024×768 and 1280×800 standard-VGA profiles.
Each profile opens all three original photos through Display Properties and
verifies actual desktop pixels, with Ciuk1 Fill as the fresh-build default.
At 800/1024/1280, the sampled RGB source matches exactly. At 640×480 in the
15-bit mode, the maximum channel error is 7 and median error is 3, consistent
with RGB555 quantization. The source image remains unchanged.

`build/full/t23-vbe-fix/properties-1280-mode-verified/1280-virtio/result.json`
passes the real 1280×800 → 640×480 preview, rollback to 1280×800, second preview,
Keep, and cold restart at the saved 640×480 mode. WALL.CFG retains its applied
photo and positioning style. Wallpaper samples after Keep and restart have
the same maximum/median error of 7/3 in RGB555. The software pointer is parked
in the excluded top strip before taking those screenshots: the preceding run
had one sample at (325,251) on the white pointer, rather than on the photo.

## Styles, theme rollback and redraw damage

The independently completed checks in
`build/full/t23-vbe-fix/properties-1280-damage/1280-virtio/result.json` cover
Fill, Fit, Stretch, Center and Tile, all five saved-style cold restarts,
legacy CWP1 Tile, rejection of truncated/trailing-byte CWP2 files, and Cancel
of an unapplied photo/style choice. All live and restarted photo samples have
zero source error. That run's overall status remains failed because its late
hit-map observer raced a repaint; its completed checks are retained separately
from the later passing mode gate. The current Desktop, Display, WALLP, SHELL
and photo payloads are unchanged from that tested image; the subsequent
production edit affects Player's deferred close handling.

`build/full/t23-vbe-fix/properties-1280-complete/1280-virtio/result.json`
independently completes the Appearance preview/Cancel and window-drag checks
on the final HDD. Preview changes 241,412 window pixels; Cancel restores the
same window palette with zero changed pixels and leaves DESKTOP.CFG unchanged.
Six frames after title dragging each check 706,976 pixels outside the old/new
window, its shadow, the pointer path and the changing telemetry strip: none
changes. The measured release-to-completed-paint interval is 0.281 seconds
under a one-CPU host quota. This is one measured interaction, not a general
frame-rate guarantee. That run's later mode-selection observer failed while
the restored display probe was still updating the list; the final mode gate
waits for that probe and passes both selections.

The earlier drag check omitted the window's +5/+6 shadow extent from its
allowed rectangles and reported those shadow pixels as unexpected damage.
The corrected bounds match the actual app_damage rectangle; no tolerance or
pixel threshold was widened to make the redraw check pass.

## Supporting gates

The production wallpaper host fixture passes 229 cache, alignment, geometry,
band-clipping and canary cases; the actual NASM glyph clipping fixture passes
15/16/24/32-bit and planar paths. See
[`wallpaper-cache.md`](../design/wallpaper-cache.md).

`build/full/t23-vbe-fix/selection-properties/report.json` passes Desktop and
Files marquee/Ctrl selection, multiple copy/move/recycle operations and FAT
content comparisons. Its SHELL, Desktop and Files modules match the final HDD
byte for byte. Full icon labels use the shared wrapper and expanded placement,
hit and damage bounds; see
[`2026-10-07-desktop-selection.md`](2026-10-07-desktop-selection.md).

`build/full/t23-vbe-fix/doom-properties-final/report.json` records two actual
DOOM level starts after changing resolution, gameplay input changing 148,056
pixels per start, menu quit, desktop return and a second launch. Runtime media
and binary provenance are recorded in
[`2026-10-07-runtime-media.md`](2026-10-07-runtime-media.md).

`build/full/t23-vbe-fix/cd-properties-verified/report.json` passes the freshly
rebuilt full CD with 512 MiB and no hard disk. Its ISO SHA-256 is
`6ffecd4352bc52c2025301802bc4f5b6e94055e8bad9c3f958239593647c5f82`.
The shell and WALLP module match the source, WALL.CFG defaults to `0400`
(Ciuk1 Fill), and all 36 sampled photo pixels match the original RGB exactly
at 1024×768. Wallpaper becomes ready 20.23 seconds after QEMU start, including
startup About dismissal. The earlier `cd-properties-final` run remains
historical evidence from before the wallpaper poll/redraw correction.

## Workstation containment

Full builds and QEMU gates run sequentially in separate systemd user scopes
with MemoryMax=3G, MemorySwapMax=1G and CPUQuota=100%; builds use one job. Host
available memory is checked before each heavy job. No Semble ML indexing or
unrestricted parallel builds run during this final validation.
