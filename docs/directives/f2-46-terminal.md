# f2-46 — Terminal: a native command prompt with system commands and Lua scripting

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-46` from `main`. Host-testable (desktop host harness like `ciuknote`, `tasks`, `control`). Owner decision 2026-10-13: approved; CiukiDOS stays the DOS personality inside the DOS Box, the Terminal is a native program.

## Contract (write it as `docs/design/terminal-contract.md` before the code, citing what you verified in the native API)

- **App** `/Programs/terminal`, menu entry "Terminal" under Programs (icon Tango `utilities-terminal`), window with a monospace text grid on the deskui toolkit (80x25 default, resizable with f2-34, scrollback 1000 lines, selection + copy, Ctrl+C interrupts a running script).
- **Interpreter** linked into the app: DOS-style syntax of the era with Unix aliases. Built-ins: `DIR`/`ls`, `CD`/`cd`, `COPY`/`cp`, `MOVE`/`mv`, `REN`, `DEL`/`rm`, `MD`/`mkdir`, `RD`/`rmdir`, `TYPE`/`cat`, `CLS`/`clear`, `ECHO`, `VER`, `HELP`, `EXIT`, `START <program> [file]` (launches graphical programs through the desktop launcher, as Files does), drive letters and the Computer namespace of f2-36 (`C:`, `D:`), wildcards, relative paths, `..`.
- **System commands** backed by the same kernel nodes the desktop uses: `NET` (driver, MAC, carrier, state; `/dev/net`), `AUDIO [volume n|mute|unmute]` (`/dev/mixer`), `TASKS` (the process list of the Task Manager, `TASKS KILL <pid>`), `DISPLAY` (current mode, available modes; no change in this directive), `SHUTDOWN`/`REBOOT` (through the desktop's power path with confirmation), `MEM`, `DATE`/`TIME`. `REG` is reserved for f2-47.
- **Lua**: `LUA script.lua [args]` and interactive `LUA` through the Lua port already in `apps/lua` (link the library, do not spawn a second window); the terminal's commands exposed to scripts as `term.run("DIR")` returning the output text, plus `term.print`. `.lua` files typed as a command run as scripts.
- **External console programs**: if the native API provides pipes and redirected standard streams (check `pipe()` in the SDK: real or stub), `RUN <program> [args]` runs a native program with stdin/stdout in the grid; otherwise document the gap and leave `RUN` returning "not available in this release".
- Persistent history (`/Documents/.terminal_history`, 200 entries), tab completion of paths.

## Acceptance

Host tests in `tests/host/desktop/` driving the app through the harness: every built-in on a fixture tree, system commands against the fake nodes, a Lua script, resize and scrollback; `make CAP= apps` builds; payload inventory and menu tests updated; the developer docs (`docs/developer-api.md` regeneration, a `docs/terminal.md` user guide with the command list).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-46-report.md`.
