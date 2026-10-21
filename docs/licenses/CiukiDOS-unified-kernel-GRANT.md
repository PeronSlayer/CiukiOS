# Licence grant for CiukiDOS code ported into the unified DOS kernel

Date: 2026-10-21. Recorded by the project lead on the owner's instruction (dev diary 2026-10-21-04).

The owner of CiukiOS, holder of the copyright of the CiukiDOS code (`src/boot/floppy_stage1.asm` and the modules derived from it), grants that every part of that code ported into the unified DOS kernel (the fork of the FreeDOS 1.3 kernel maintained in this repository under `docs/design/unified-dos-contract.md`) is licensed under the GNU General Public License, version 2 or (at your option) any later version, the licence of the FreeDOS kernel.

Scope: only the code that is actually incorporated into the fork or into the CiukiOS-specific drivers and programs built from the fork's tree. The CiukiDOS code that stays outside the fork keeps its existing licence. Each ported file carries the GPL-2.0-or-later notice and names its origin.

Corresponding source: the fork, its patches, build scripts, pins and notices are published in the CiukiOS GitHub repository and, for every release containing the kernel, as a source archive whose SHA-256 is recorded in the release manifest.
