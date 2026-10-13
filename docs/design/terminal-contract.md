# Native Terminal (f2-46)

Written before implementation, 2026-10-13. `/Programs/terminal` is a native
desktop client, listed as Terminal in Programs with Tango `utilities-terminal`.
It uses libdeskui's monospace face, canvas, clipboard, dialogs and negotiated
v2.1 resizing. Its initial grid is 80 columns by 25 rows; smaller screens may
resize it. It retains 1000 output lines and 200 commands, with history at
`/Documents/.terminal_history`. Selection copies text with Ctrl+C while idle;
Ctrl+C cancels a running Lua chunk. Tab completes paths, Up/Down recall history,
and Page Up/Down browse output. Resizing reflows retained lines without losing
commands or Lua state.

## Verified sources and native interfaces

- [POSIX directory enumeration](https://pubs.opengroup.org/onlinepubs/9799919799/functions/opendir.html)
  and [rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html):
  checked against `sdk/libciuki/posix.c`, `apps/files/vfs.c`, `model.c` and
  `transfer.c`. Reuse Files' replaceable VFS and transfer jobs, including short
  I/O, exclusive creation, temporary files, error cleanup and cross-volume moves.
  COPY does not overwrite silently. MOVE/REN refuse an existing destination.
  DEL removes files; RD removes empty directories. No implicit recursive delete.
- Files' f2-36 namespace maps `C:` to `/` and `D:`..`Z:` to `/mnt/d`..`/mnt/z`.
  Computer is virtual and lists mounted drives. Relative paths and `..` are
  normalized; `..` at a drive root stays there. Drive-relative `C:folder` is
  rejected, matching Files. Quoted paths retain spaces and DOS backslashes.
  Commands and wildcard comparisons are ASCII case insensitive. `*` and `?`
  expand the final path component; DOS `*.*` also matches extensionless names.
- [POSIX pipes](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pipe.html):
  `sdk/libciuki/excluded.c` implements `pipe()` as `-1/ENOSYS`. Spawn fd mapping
  exists, but no real pipe connects console streams. RUN therefore prints
  `not available in this release`; it never launches a second console window.
- `ciuki_proc_list` (call 70) and `apps/tasks/model.c` supply TASKS and MEM.
  TASKS KILL requests the desktop's close path for its children; the desktop
  uses its existing delegated termination callback for other eligible processes.
  PID 1 and the desktop remain protected. Terminal receives no authority fd.
- `/dev/net` with NET_GET_STATE is read-only and reports driver, MAC, carrier
  and driver state, without implying Internet connectivity. `/dev/mixer` with
  MIXER_GET_MASTER reads volume. DESK_MIXER_SET delegates writes to the desktop,
  as Volume Control does; extend its launcher authorization to Terminal.
- `ciuki_display_query` reports current and available modes; Terminal never
  changes the mode. `time()`/`localtime()` back DATE and TIME (read-only).
- START sends DESK_LAUNCH with a program and optional absolute file argument,
  using the same launcher as Files. Only graphical programs are accepted.
  Launcher-granted Terminal identity authorizes these requests, not its title.
- `ciuki_power` is restricted to the supervisor-spawned desktop. Add a bounded
  v2 DESK_TERMINAL_POWER request (serial, REBOOT=1/OFF=2), authorized only for
  desktop-launched Terminal, after its Yes/No dialog. The desktop runs the
  existing `desk_power_begin` close/flush/power sequence and returns TASK_RESULT.
- [Lua 5.4 C API](https://www.lua.org/manual/5.4/manual.html): checked the existing
  pinned 5.4.8 port in `apps/lua/build_lua.py` (generic configuration, static
  `liblua.a`, int64/double). Link that archive into Terminal. Use protected calls,
  `arg`, a persistent interactive state and an instruction-count hook that
  services the desktop and raises an interruption error on Ctrl+C. Lua print
  and term.print write to the grid; term.run executes the same command engine
  and returns captured text. Nested Lua invocation from term.run is refused.
  Blocking C/library calls cannot be preempted until control returns to Lua.

## Commands and limits

DIR/ls, CD/cd, COPY/cp, MOVE/mv, REN, DEL/rm, MD/mkdir, RD/rmdir,
TYPE/cat, CLS/clear, ECHO, VER, HELP, EXIT, START, NET, AUDIO
`[volume 0..100|mute|unmute]`, TASKS `[KILL pid]`, DISPLAY, SHUTDOWN,
REBOOT, MEM, DATE, TIME, LUA `[script.lua args...]`, and `.lua` command paths.
REG is reserved for f2-47. Interactive LUA accepts chunks and continuation
lines; `.exit` returns to the command prompt. Syntax errors are visible.
No pipelines, redirection or batch language is promised by this release.

Input is bounded to 1039 bytes and 32 arguments; output lines split at 1024
bytes. Lua command captures are bounded to 1 MiB and report overflow. The
command loop and Lua hook remain responsive to desktop messages. Errors are
printed with their operation and errno; an unsuccessful operation never
prints success. History writes use a temporary file, sync and rename.

## Host acceptance

Drive libdeskui input and the production command engine on a fixture VFS and
fake system providers. Cover every command and alias, drive navigation,
wildcards, quoted paths, short I/O and failures, Lua scripts/arguments/capture,
interactive state and Ctrl+C, history persistence, completion, selection,
resizing and the scrollback bound. Verify desktop authorization and protocol
validation, menu/icon and image inventory registration. Run suites sequentially
with `nice -n 19`; build `make CAP= apps`. QEMU and hardware qualification,
integration, commits and the diary remain with the lead.
