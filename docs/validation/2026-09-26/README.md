# Desktop validation snapshot — 26 September 2026

These are unchanged JSON reports from the selected development disk image:
`08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`.
The 128 MiB image and large local captures are not distributed in this archive.
Paths inside reports identify the original local evidence locations.

- [Validation index](validation.json): eleven passing focused checks, report
  hashes and explicitly incomplete requirements.
- [Packaged binary manifest](manifest.json): the image's binary identities.
- [Source freeze](source-freeze.json): sources used for this image, before
  later documentation edits or virtualization work.

The index links each report by its relative path. Test scripts are published
under `scripts/`; [the implementation record](../../native-desktop-2026-09-26.md)
describes setup, limits and failure evidence. These reports cover QEMU with a
Pentium III model and 128 MiB RAM. They do not establish physical T23/E500
compatibility, original DOS graphics in windows, hardware acceleration or a
30 fps hardware guarantee. They are preserved evidence, not an assertion that
every subsequent checkout reproduces the same binary hashes.
