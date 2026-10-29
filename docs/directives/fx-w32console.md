# fx-w32console: Win32 console programs in an interactive console window (W32-6)

- **Written:** 2026-10-29 11:39 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-w32console`, branch `wt/fx-w32console` from `main`.
- **Inventory:** W32-6, "Console programs in a console window", absent: "No interactive console window". The kernel32 console layer is `sdk/win32/kernel32/console.c`.

## Required behaviour

Research first:
- the Windows console architecture of the 9x era (Windows 98 used `CONAGENT`/`WINOA386` for console apps; NT used `csrss`), so that the model is chosen deliberately;
- the Win32 console API contracts on Microsoft Learn (`AllocConsole`, `GetStdHandle`, `ReadConsoleInput`, `WriteConsoleOutput`, `SetConsoleMode`, `SetConsoleCtrlHandler`, `GetConsoleScreenBufferInfo`);
- Wine's `conhost` and `kernel32` console code (LGPL; reuse is allowed, keep the notices).

Record the sources and the chosen model in a design note.

1. **Launch.** A PE with subsystem `IMAGE_SUBSYSTEM_WINDOWS_CUI`, launched from the desktop (Explorer, Run, a shortcut), gets a console window owned by the desktop:
   - an 80x25 text grid by default, with a scroll-back buffer;
   - the title is the program path;
   - the window closes when the last attached process exits.

   A CUI program launched from an existing console (a parent CUI process or a DOS-style command line) shares that console. A GUI program can call `AllocConsole`/`FreeConsole`/`AttachConsole`.
2. **Output.** `WriteFile` and `WriteConsoleA/W` to the standard handles, `WriteConsoleOutput*`, cursor positioning, attributes/colours and `FillConsoleOutput*`. These are enough for real console tools: installers' console helpers, `xcopy`-style tools and Win32 compilers.
3. **Input.** Keyboard into `ReadFile`/`ReadConsoleA/W`, with line editing in cooked mode, and `ReadConsoleInput`/`PeekConsoleInput` in raw mode, according to `SetConsoleMode`. Ctrl+C and Ctrl+Break reach `SetConsoleCtrlHandler` handlers, then the default termination.
4. **Window.** It uses the existing desktop window and text rendering (the DOS window's text path where it fits, without breaking DOS VMs), with:
   - Mark/Copy/Paste as in the DOS window (CB-5);
   - a Properties entry for font and buffer size;
   - Alt+Enter, which stays windowed with a notice: Windows 98 console apps were windowed or full screen, and full screen is out of scope here.
5. **Security.** The console belongs to the launching user's session. The new privileged path follows the session-console inheritance rules (fx-acc-dosfs): no cross-session console access. Add census rows for any new privileged operation.

## Tests and evidence

- Host tests:
  - a CUI test program (built with the existing Win32 test toolchain) that writes text and colours, positions the cursor, reads a cooked line and a raw key event, and handles Ctrl+C;
  - console sharing between a parent and a child;
  - `AllocConsole` from a GUI program;
  - window close on the last exit.
- Update W32-6 in the inventory.
- Run the Win32 and desktop host suites sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-w32console-report.md`.
