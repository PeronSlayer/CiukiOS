# f2-57 — Desktop gate harness after the shell-first gate start-up (f2-56)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-57` from `main`. Host-testable.

## Evidence (main, 2026-10-14 04:20)

`python3 -m unittest discover -s tests/host -p test_desktop_gate.py` → `test_native_desktop_gate_loop` fails in the `late_input`/`no_input` variants added by f1-46: `tests/host/desktop/desktop_gate_test.c:270 ciuki_input_read: Assertion 'n==-EAGAIN' failed`; the model's `desktop_spawn_host_read_input(fd=4, …)` (`tests/host/proc/desktop_test.c:1098`, `grant_input_read(spawn_host_server, fd, …)`) returns **-9 (EBADF)**: at that moment fd 4 of the modelled server is not the input grant. Since f2-56 the gate desktop runs `shell_setup` before its render loop, so the order and numbering of the descriptors it opens before the first input read changed (the lead already removed the hard-coded fds 3/4 and the 6..63 range from the channel-pair checks in the same files; see the last commit). All other gate variants pass; the desktop suite (`tests/host/desktop`, 92 tests) and the full `tests/host` discovery pass except this test.

## Task

1. Find where the harness or the model assumes the input grant's fd number or its installation order relative to the shell's early opens (display grant, mixer, net/media queries, icon store) and make it follow the real descriptors (look them up by object kind like the channel-pair check now does).
2. Keep the semantics of the f1-46 variants (late keyboard attach after 5 s, no device at all → EAGAIN reads, keys delivered after attach) and of the f2-56 first-frame mark check.
3. Run `tests/host/test_desktop_gate.py`, `tests/host/test_desktop_performance.py` and the desktop suite.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-57-report.md`.
