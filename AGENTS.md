## Project identity — Ciuki

CiukiOS is dedicated to **Ciuki**, the dog pictured in
`misc/CiukiOS_SplashScreen.png`. This is an explicit project requirement from
the owner, recorded on 2026-09-25; preserve it across sessions and redesigns.

The approved OS logo is the detailed Ciuki portrait in `assets/brand/ciuki-logo.png`
(the first generated proposal, explicitly selected by the owner). Derive runtime
icons directly from that exact asset. The owner rejected the hand-drawn pixel
reinterpretation and the later simplified alternative: do not redraw, regenerate,
or substitute them. Preserve the approved portrait's shape, pose and colours;
only deterministic size/format conversion for the renderer is appropriate.
Do not substitute a generic dog, an abstract letter C, or an unrelated mascot.
Keep the original Ciuki photograph in the boot splash. Desktop and Setup must
share this identity.
UI copy remains English; the existing tagline is `A modern Retro OS`.

The official system icon family uses **Tango Icon Theme 0.8.90**, whose upstream
icons are released into the Public Domain. Keep its source archive, authors and
license notice in `assets/icons/`; see `assets/icons/README.md`. Computer, About
and other CiukiOS identity icons must incorporate deterministic conversions of
the exact approved Ciuki portrait above. The portrait is a separate project
asset, not part of Tango's Public Domain release. Do not replace this family
with extracted Microsoft/Apple artwork or change the approved portrait.

## Active build profile

The main CiukiOS project now uses the full HDD image and, when relevant, the
full CD image. Do not build, run, test, or maintain the standalone floppy
profile as part of ongoing main-branch work. Existing floppy files may remain
for history; this decision does not exclude work on the shared CiukiDOS kernel
source (`src/boot/floppy_stage1.asm`) used by the full image. The owner is
considering a separate CiukiDOS-only branch, but has not asked to create it.

The canonical full build also refreshes `build/releases/CiukiOS-0.8.0-Windows-portable.zip`.
Keep this Windows QEMU bundle current whenever changes affect the full image.
It uses a pinned portable QEMU and a sanitized copy of the FAT16 image; local
commercial game payloads must never enter the release. Verify the archive
contents and integrity, but only the Linux full build/run profile requires a
runtime QEMU test. Do not claim that the Windows launcher was tested on Windows.
On this clone, a local `pre-push` hook publishes a dated, numbered GitHub
prerelease for each clean `origin/main` push using `scripts/push_release.py`.
Do not bypass that hook for normal main pushes. Other clones must run
`bash scripts/install_release_push_hook.sh`. No GitHub Actions are used.

## Code Search Policy

Use Semble before reading large files.

For code discovery, prefer:

```bash
semble search "describe what you need" .
semble search "symbol_or_label_name" .
semble find-related path/to/file line .

If semble is not available on PATH, use:

uvx --from "semble[mcp]" semble search "describe what you need" .
uvx --from "semble[mcp]" semble find-related path/to/file line .

Rules:

Use semble search before opening large files such as src/boot/floppy_stage1.asm.
Open only the returned file ranges or nearby small ranges.
Use rg only for exact literal confirmation.
Do not read entire large files unless strictly required.
Do not run broad repository scans.
For CiukiOS, always search first for labels/functions before opening floppy_stage1.asm.

Recommended searches:

semble search "int21 rmdir parent sector read write ES corruption" .
semble search "int21 mkdir directory cluster zero fill" .
semble search "SHELL.COM command parser builtins" .
semble search "read_sector_lba32 write_sector_lba32 FAT16" .
semble search "qemu shell com validation markers" .
semble search "external command execution INT21 AH 4B" .

## 4. Rendi disponibile il comando `semble`

Per i subagent è meglio avere anche il comando shell disponibile.

Opzione A, con `uv tool`:

```bash
uv tool install "semble[mcp]"

```
