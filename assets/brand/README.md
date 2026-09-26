# CiukiOS — Ciuki

CiukiOS is dedicated to **Ciuki**, the dog in the original boot splash.
The owner explicitly selected the detailed first logo proposal, stored as
`ciuki-logo.png`. This exact portrait is the source for the OS identity.
The independently drawn pixel interpretation and the later simplified proposal
were rejected. Do not substitute, redraw or regenerate the approved portrait.

## Approved source

- `ciuki-logo.png`: original 1254 × 1254 RGBA PNG, copied without modification.
- SHA-256: `17210ac9cfe07c54d6b0069a1502a036d52cb6340715ad47bdd54fa4dd386620`.
- Original photographic splash: `misc/CiukiOS_SplashScreen.png`, unchanged.
- Initial generation used the built-in image_gen tool; [generation.md](generation.md)
  records the prompts and distinguishes the unselected alternative.

The dedication and the exact source requirement are also recorded in the root
`AGENTS.md`. UI copy stays English and the tagline remains `A modern Retro OS`.

## Native icons derived from the approved image

`scripts/build_ciuki_logo.py` converts the complete approved canvas to 16, 24
and 64 pixels at build time. It preserves pose and proportions with LANCZOS
resampling, a shared 64-colour palette and a one-bit transparency mask. There
is no hand-drawn replacement. `native/*.png` previews the actual compiled
indices; the full-resolution master retains all its original detail and alpha.

Desktop and Setup reserve colour indices 16–79 for the logo. Their existing
first 16 colours are unchanged. The existing VBE conversion supports indexed,
15-, 16-, 24- and 32-bit video. Planar VGA can display only 16 colours, so it
maps the same source-derived pixels to the nearest existing colour. That
hardware limitation does not cause a change of pose or silhouette.

`src/com/ciuki_logo.inc` reads embedded indices and groups horizontal runs.
It skips transparent pixels and rows outside the current composition band.
There is no runtime PNG decoding, resampling, heap allocation or file access.
About shows the portrait at 64 native pixels so its details remain readable.

Rebuild and verify the assets from the repository root:

```sh
python3 scripts/build_ciuki_logo.py
python3 scripts/build_ciuki_logo.py --check
```

The normal full build verifies source and asset hashes before assembling the
UI programs. `native/manifest.json` records provenance and conversion settings.

## Verification

Evidence: `build/full/ciuki-logo-approved-2026-09-25/`.

- 90 compiled-instruction sprite cases: sizes, packed/planar colours, partial
  and excluded bands, negative Setup origin, exact pixels and register/flag
  preservation.
- 10 palette instruction cases across five pixel depths, default 16-entry and
  extended 80-entry tables, bounds, DAC transfer and unchanged base colours.
- The existing 14 renderer regression groups passed.
- QEMU Pentium III / 128 MiB: actual desktop and About at 800×600 and 2560×1440,
  graphical Setup at 800×600, cancellation, COMDEMO and desktop return.
  Screenshot logo rectangles are compared against the exact PNG-derived runtime
  sprites, including transparent areas; tolerance is at most three channel
  values for the VGA DAC conversion, not a tolerance for changed geometry.

These checks cover this logo correction. Earlier UI fluidity and physical
hardware qualification limits remain open. The Setup preview binary has
installation writes disabled and is not a release installer. No physical disc
or previous release ISO was changed by this branding update.
