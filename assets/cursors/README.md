# CiukiOS Cursor Schemes

This directory contains the pointer/cursor graphics for CiukiOS.

## Schemes Included in CURSORS.DAT

1. **Tango (Default)**:
   - Derived from the Tango Desktop Project icon and cursor theme guidelines.
   - License: **Public Domain** (see `assets/icons/README.md` and `assets/icons/upstream/COPYING`).
   - Standard 16x16 bitmaps with high contrast anti-aliased edge styling.

2. **Classic**:
   - Retro 16x16 pixel-perfect style inspired by classic PC desktop environments.
   - Public Domain / Clean-room pixel design.

3. **3D Contrast**:
   - Heavy high-visibility silhouette designed for accessibility and high contrast.
   - Public Domain.

## Binary Format

- Magic: `CIUKCUR1` (8 bytes)
- Theme count: `uint16_t` (2 bytes)
- Theme entry size: `uint16_t` (2 bytes, 214 bytes per theme)
- Per Theme:
  - Theme Name: 16 bytes ASCII null-terminated
  - 3 Cursors (Arrow, I-Beam, Forbidden):
    - 16 words outline/black mask (32 bytes)
    - 16 words fill/white mask (32 bytes)
    - Hotspot X, Hotspot Y (2 bytes)
