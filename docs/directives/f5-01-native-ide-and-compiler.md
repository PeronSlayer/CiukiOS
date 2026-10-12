# f5-01 — Native development on CiukiOS: Tiny C Compiler on target, CiukIDE and the developer guide

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` allowed. **Worktree:** `../CiukiOS-wt-f5-01` from `main`. Host-testable; the lead qualifies on QEMU/T23.

## Owner request (2026-10-12)

"Vorrei anche un IDE e compilatore per poter scrivere app native per CiukiOS (con tanto di documentazione e guida)."

## Scope

1. **Compiler on target.** Port Tiny C Compiler (tcc, LGPL-2.1; pin the release and SHA-256, vendor under `third_party/tcc` with its licence) to run as a CiukiOS native program `/Programs/tcc` producing i386 ELF executables that the CiukiOS loader runs: tcc built against the CiukiOS SDK (newlib) with its runtime (`libtcc1.a`), the SDK headers and libraries installed in the image under `/System/sdk` (headers `ciuki/*.h`, libc, libdeskui, crt0, linker script), so that `tcc -o hello hello.c -ldeskui` works on the machine. Document the supported subset (no dynamic linking; C99; which libc parts).
2. **CiukIDE** (`/Programs/ciukide`, Start > Programs > Developer > CiukIDE): a libdeskui program built on CiukNote's editor with C syntax highlighting, line numbers, a project (a folder with `project.cfg`: sources, output, libraries), Build (runs tcc as a child process, captures stdout/stderr through the existing process/pipe facilities, parses `file:line: error` into an error list that jumps to the line), Run (launches the built program through the desktop launch path), templates New Project → "Hello window" (libdeskui) and "Hello console". Keyboard: F7 build, F5 run.
3. **Developer guide** shipped in the image and in the repository: `docs/developer-guide.md` (installed as `/Documents/Developer/Guide.txt` or rendered in CiukiOS Help): the CiukiOS native model (processes, POSIX subset, desktop protocol via libdeskui), building on target with tcc and on the host with the SDK, a tutorial that writes and runs the Hello window app in CiukIDE, the API reference generated from the public headers (a script `scripts/gen_api_reference.py` producing Markdown from `ciuki/*.h` and `deskui.h` comments), and a sample projects folder `/Documents/Developer/Samples`.
4. Tests: tcc builds and runs the samples in the host harness (host-side tcc targeting the SDK ABI, run under the existing guest/ELF checks), IDE harness (project parsing, error list parsing, build/run state machine), guide generation reproducible.

## Constraints

Respect licences (LGPL notice shipped as `/System/sdk/TCC.TXT`). No QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f5-01-report.md`.
