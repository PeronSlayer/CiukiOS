# HANDOFF - stage2 shell extended commands (cls/ver/echo/shutdown/reboot)

## Date
`2026-04-15`

## Context
User requested extending the stage2 mini shell with new DOS-style internal commands, in particular commands to power off and reboot the machine.

## Completed scope
1. Added `video_cls()` to `video.c`/`video.h` — clears the framebuffer and resets the cursor to (0,0).
2. Added port I/O helpers `outb_port` and `outw_port` as static inlines in `shell.c`.
3. Added `get_arg_ptr()` helper to extract the argument portion after the first token (used by `echo`).
4. Implemented five new internal commands in `shell.c`:
   - `cls`      — calls `video_cls()` to wipe the screen
   - `ver`      — prints OS version string
   - `echo`     — prints everything after the command to the screen followed by newline
   - `shutdown` — ACPI S5 soft-off via I/O port 0x604 value 0x2000 (QEMU PIIX4 PM1a_CNT, SLP_EN bit 13)
   - `reboot`   — keyboard controller reset pulse via `outb(0x64, 0xFE)`
5. Updated `shell_print_help()` to list all eight commands.
6. Updated serial marker in `stage2.c` to reflect full command set.
7. Updated `scripts/test_stage2_boot.sh` required pattern to match new marker string.

## Touched files
1. `stage2/include/video.h` — added `video_cls()` declaration
2. `stage2/src/video.c` — added `video_cls()` implementation
3. `stage2/src/shell.c` — `outb_port`, `outw_port`, `get_arg_ptr`, five new command handlers, updated help and dispatch
4. `stage2/src/stage2.c` — updated shell ready serial marker
5. `scripts/test_stage2_boot.sh` — updated required pattern for shell ready marker

## Technical decisions
1. Decision: ACPI shutdown via hardcoded port 0x604 (QEMU PIIX4 default).
   Reason: No ACPI table parser exists yet; this is the well-known QEMU PM1a_CNT address for i440FX+PIIX4 machines (default OVMF machine type).
   Impact: Works reliably under QEMU; on real hardware this port may differ — future ACPI driver must replace this.

2. Decision: Keyboard controller reset (port 0x64, value 0xFE) for reboot.
   Reason: Standard x86 reset mechanism, compatible with all PC-compatible BIOSes and hypervisors.
   Impact: Most reliable reset method available without ACPI or UEFI Runtime Services.

3. Decision: `echo` passes raw pointer into the original line buffer (after first token) rather than copying.
   Reason: `line` is a stack buffer inside `stage2_shell_run()` and is valid for the duration of `shell_execute_line()`.
   Impact: Zero-copy, no stack pressure; safe given call lifetime.

4. Decision: `cls` is a thin wrapper over `video_cls()` (not inlined into dispatch).
   Reason: keeps the dispatch table uniform and allows future `cls` logic (e.g. cursor blink reset) without touching dispatch.

5. Decision: `shutdown`/`reboot` have infinite `hlt` loops after the I/O write.
   Reason: defensive — the hardware reset should trigger before the loop is reached, but the loop prevents undefined behavior if the I/O has no effect.

## ABI/contract changes
1. New public function: `void video_cls(void)` in `video.h`/`video.c`.
2. Serial shell-ready marker changed from:
   `[ ok ] stage2 mini shell ready (help/ticks/mem)`
   to:
   `[ ok ] stage2 mini shell ready (help/cls/ver/echo/ticks/mem/shutdown/reboot)`
3. `test_stage2_boot.sh` required pattern updated accordingly.

## Tests executed
1. `make clean && make`
   Result: PASS — zero warnings.

2. `make test-stage2`
   Result: PASS — all 14 required patterns found, all 4 forbidden patterns absent.

## Current status
1. Stage2 shell now has eight internal commands: `help`, `cls`, `ver`, `echo`, `ticks`, `mem`, `shutdown`, `reboot`.
2. Shutdown and reboot are functional under QEMU/OVMF.
3. All boot tests pass; serial markers remain synchronized with test automation.

## Risks / technical debt
1. ACPI shutdown port (0x604) is QEMU-specific. Real hardware requires parsing FADT to find PM1a_CNT_BLK.
2. No UEFI Runtime Services (`ResetSystem`) path — once ExitBootServices is called, only bare-metal methods are available.
3. No command history or argument quoting; `echo` treats the entire remainder of the line as literal text.

## Next steps (recommended order)
1. Add `.COM` loader boundary — define how a flat binary gets loaded and jumped to from the shell.
2. Add `dir` / `type` commands backed by a simple FAT12 reader on the boot device.
3. Implement a proper ACPI table walker to replace the hardcoded shutdown port.

## Notes for Claude Code
- Keep `shutdown`/`reboot` I/O port values synchronized with the QEMU machine type in `run_ciukios.sh`.
- If the machine type changes to `q35`, the ACPI PM base changes — the shutdown port must be updated.
- Do not add interactive shell tests to the automated test suite; headless QEMU does not provide keyboard input.
