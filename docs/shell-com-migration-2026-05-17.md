# SHELL.COM Migration Slice (2026-05-17)

## Goal
Start moving shell UX out of Stage1 toward `\SYSTEM\SHELL.COM` without changing the default Stage1 shell, full/full-CD boot ownership, or the current DOOM and DOOMSFX lanes.

## Current Stage1 Shell Map
- Prompt and loop: `print_prompt` and `main_loop` in `src/boot/floppy_stage1.asm`
- Default shell directory setup: `init_shell_default_dirs`
- Input editor/history/tab completion: `read_command_line`, `shell_line_*`, `shell_history_*`, `shell_try_tab_complete_line`
- Builtin dispatch chain: `dispatch_command`
- Exec parsing and command-tail packing: `shell_arg_ptr`, `shell_next_arg`, `shell_exec_set_tail_from_si`
- External command resolution: `shell_try_resolve_exec_token`, `shell_try_exec_token`, `shell_try_exec_path`
- Current bare-name search order: CWD, `\APPS`, `\SYSTEM\DRIVERS`
- Current shell-heavy tests: `scripts/qemu_test_full_shell_stability.sh`, `scripts/qemu_test_full_cd_shell_drive.sh`, `scripts/qemu_test_full_dos_compat_smoke.sh`

## Boundary For `\SYSTEM\SHELL.COM`
- Normal DOS `.COM` process only; no Stage1 internal label calls.
- Stable APIs only: DOS `INT 21h`, BIOS `INT 10h` only where needed.
- No dependence on Stage1 private buffers like `cwd_buf`, `shell_exec_path_buf`, or shell editor state.
- Launch contract is just PSP/environment/default drive/CWD/console handles from the existing DOS exec path.
- Return contract is normal DOS process termination back to Stage1.

## Implemented In This Slice
1. Added `src/com/shell.asm`, a tiny DOS shell prototype.
2. Packaged it into full and full-CD images as `\SYSTEM\SHELL.COM`.
3. Kept the default Stage1 shell completely unchanged as the boot-time owner and fallback.
4. Added `scripts/qemu_test_full_shell_com.sh` and `make qemu-test-full-shell-com` for an opt-in launch lane.

## Prototype Command Surface
- `help`
- `ver`
- `echo`
- `cls`
- `exit`

## Next Recommended Slice
- Add `cd` and `dir` inside `SHELL.COM` via DOS `INT 21h`, or
- Add an opt-in external-shell boot switch while preserving Stage1 fallback as the default.
