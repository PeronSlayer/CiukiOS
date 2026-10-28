# fx-icons: complete the system icon set (Tango first, Haiku/Oxygen/Tabler/Pixelarticons for the gaps)

- **Written:** 2026-10-27 14:05 by the lead. Implementer: Codex gpt-6-luna, effort medium. Base: main f7406f0ef or later.
- **Owner request 2026-10-27:** use the free icon sets recommended to him (Haiku icons MIT, Oxygen LGPL v3, Tabler MIT, Pixelarticons MIT) to give every part of CiukiOS a proper icon.
- **Unchanged rules (AGENTS.md):** Tango Icon Theme 0.8.90 stays the official family; Computer, About and the other CiukiOS identity icons keep the exact approved Ciuki portrait (`assets/brand/ciuki-logo.png`, deterministic conversions only); no extracted Microsoft/Apple artwork. Keep every set's licence and authors beside its files, as `assets/icons/README.md` does for Tango.
- **Archives (downloaded by the lead, pinned):** `build/downloads/icons-2026-10-27/` with `SHA256SUMS`: `haiku-icons.tar.gz` (darealshinji/haiku-icons exports), `oxygen-icons-6.0.0.tar.xz` (KDE), `tabler-icons.tar.gz`, `pixelarticons.tar.gz`. Your sandbox has no network: use these files only.

## Scope

1. **Inventory.** List every icon CiukiOS shows (desktop, Start menu, Files and its views, Control Panel applets, system tray and notifications, dialogs, toolbars and menus, file types, drives and devices, Games and Programs entries) and the asset each uses today; mark placeholders, generic fallbacks and missing sizes.
2. **Fill the gaps in this order of preference:** Tango 0.8.90 (already in `assets/icons/`); Haiku icons (desktop, folders, drives, devices, applications); Oxygen (file manager, recycle bin, peripherals); Tabler, then Pixelarticons, only for toolbar, menu and status glyphs. Keep one consistent style per context (do not mix Haiku and Oxygen in the same view).
3. **Format.** PNG with alpha at 16x16, 32x32 and 48x48 (the sizes the desktop and Files draw), converted deterministically at build time from the upstream files (no SVG renderer in the OS); keep the upstream sources in their archives, not copied into the tree wholesale; only the icons used are staged, with a manifest recording set, upstream path and licence per icon.
4. **Integration.** Through the existing icon pipeline (find how `assets/icons/` and the Tango conversions reach the image: scripts/build_image.py and the desktop/Files icon tables). Licence notices staged into the image with the icons.
5. **Tests.** A host test that every inventory entry resolves to a staged icon at all three sizes, that every staged icon has a manifest row with an allowed licence, and that the identity icons still derive from the Ciuki portrait; the existing desktop, Files, Control Panel and image-builder tests stay green.

Report to build/codex/fx-icons-report.md with the inventory table. Leave changes uncommitted.
