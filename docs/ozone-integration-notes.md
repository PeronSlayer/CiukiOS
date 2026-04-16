# oZone GUI Integration Notes

## Package Identity
- **Name:** oZone GUI (ozonegui)
- **Category:** GUI desktop environment for DOS/FreeDOS
- **Version:** FreeDOS 1.3 repository release (2022-02-17)
- **Platform:** x86 real-mode DOS (16-bit), uses VESA/VBE for graphics

## Source URLs
- **Repository listing:** https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/gui/
- **Package archive:** https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/gui/ozonegui.zip
- **FreeDOS project page:** https://www.freedos.org/ (oZone listed under GUI packages)

## Package Contents (Expected)
The `ozonegui.zip` archive is expected to contain:
- `OZONE.EXE` — main executable (DOS MZ format)
- `OZONE.INI` or similar configuration file
- Resource files (icons, fonts, theme data)
- Optional: documentation, README, license text

Note: Exact contents must be verified after download and extraction.
Checksums are recorded in `third_party/freedos/manifest.csv` after import.

## Trust Assumptions
1. The ibiblio.org mirror is the canonical FreeDOS distribution point.
2. The `ozonegui.zip` package is part of the official FreeDOS 1.3 repository.
3. oZone is distributed under GPL-2.0-or-later (to be confirmed from archive contents).
4. No binary is trusted without SHA-256 verification after import.

## Integration Policy
- oZone is treated as an **optional runtime payload** — never a hard dependency.
- CiukiOS core boot path must not require oZone presence.
- When present, oZone files are placed at `A:\FREEDOS\OZONE\` on the disk image.
- Launch requires CiukiOS INT 21h + INT 10h compatibility baseline to be functional.
- oZone integration does not replace the CiukiOS native GUI roadmap.

## Runtime Path
- Import target: `third_party/freedos/runtime/OZONE/`
- Disk image path: `A:\FREEDOS\OZONE\`
- Shell command: `ozone` (dispatches to `RUN OZONE.EXE` from FREEDOS\OZONE path)

## Licensing
- Expected: GPL-2.0-or-later (standard for FreeDOS GUI packages)
- License text must be confirmed from archive and stored in `docs/legal/freedos-licenses/`
- See `docs/freedos-integration-policy.md` for redistribution policy

## Status
- Provenance references: documented (this file)
- Manifest entries: added to `third_party/freedos/manifest.csv`
- Binary import: pending (requires user-supplied archive)
- Runtime verification: pending
