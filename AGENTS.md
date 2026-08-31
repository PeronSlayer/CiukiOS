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
