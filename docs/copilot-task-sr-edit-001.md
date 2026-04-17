# Copilot Task Pack - SR-EDIT-001 (DOS `EDIT` Clone for CiukiOS)

Audience: external Copilot agent (Codex/Claude Code) working on a feature branch.
Baseline: CiukiOS Alpha v0.7.1. Do NOT touch `main` directly.

## Mandatory Branch Isolation
```bash
git fetch origin
git switch -c feature/copilot-sr-edit-001 origin/main
```

No commits on `main`. No force-push on shared branches. No regressions to existing gates.

## Mission
Recreate the historic MS-DOS `EDIT.COM` program as a native CiukiOS `.COM` binary (`CIUKEDIT.COM`) that allows the user to create, view, and save plain-text files (`.txt`) from the CiukiOS shell, using only the INT 21h surface already implemented in stage2.

This is a user-visible milestone: after this lands, `run CIUKEDIT.COM HELLO.TXT` inside CiukiOS must open a text editor, let the user type lines, and save the file to the FAT-backed filesystem so that a later `type HELLO.TXT` from the shell prints the same content.

## Context You Need (read before touching code)
1. `CLAUDE.md` (repo collaboration readme, versioning cadence rule).
2. `docs/int21-priority-a.md` (exact INT 21h surface you are allowed to rely on).
3. `stage2/src/shell.c` (how COM binaries are launched, how INT 21h is dispatched, how `run` resolves paths).
4. `boot/proto/services.h` (the services ABI and `ciuki_int21_regs_t` shape).
5. `com/hello/hello.c` + `com/hello/linker.ld` (reference COM program layout and PSP-relative string trick for `AH=09h`).
6. `com/dosrun_mz/` (reference for MZ layout if you go EXE instead of COM).
7. `Makefile` search for `COM_HELLO_` and `COM_M6_DPMI_MEM_SMOKE_` to see how to wire a new COM binary end-to-end.
8. `run_ciukios.sh` (how files land on the FAT image at build/run time).
9. `scripts/test_m6_dpmi_mem_smoke.sh` and `scripts/test_dosrun_simple_program.sh` (test harness patterns: 120s QEMU timeout + static-fallback grep mode).

## Hard Constraints
1. The binary MUST be built from source in this repo. No redistributing MS-DOS/Microsoft `EDIT.COM`.
2. Do NOT introduce new INT 21h functions in stage2. Only use what is already `IMPLEMENTED` in `docs/int21-priority-a.md`.
3. Do NOT regress: `make test-stage2`, `make test-mz-regression`, `make test-m6-pmode`, `make test-vga13-baseline`, `make test-m6-dpmi-ldt-smoke`, `make test-m6-dpmi-mem-smoke`, `make test-doom-boot-harness`, `make test-doom-target-packaging`.
4. Deterministic serial markers only. No random strings, no timestamps.
5. Stay under the MZ hard-size limit if you build an EXE (512 bytes payload). Prefer a `.COM` for this task: no size cap other than the paragraph allocator baseline.
6. Respect the versioning cadence rule in `CLAUDE.md`: this task counts as 1-2 roadmap items. Only bump the patch version when the 3-4 item threshold is reached across tasks.

## Scope (7 tasks)

### E1) Editor COM Source Tree
1. Create `com/ciukedit/linker.ld` mirroring `com/hello/linker.ld` (ENTRY `com_main`, `. = 0x100`).
2. Create `com/ciukedit/ciukedit.c` implementing the editor (details in E2/E3).
3. Keep the sources self-contained: use `services.h`, `bootinfo.h`, `handoff.h` only.

### E2) Editor UX (Line-Oriented, DOS-Faithful Minimal Subset)
Implement a line-oriented editor, not a full-screen one (full-screen needs cursor positioning beyond the current INT 21h surface). The UX must be unambiguous and discoverable.

1. Header banner printed with `AH=09h`:
   ```
   CiukiOS EDIT v1 - line editor
   File: <FILENAME>
   Commands: :w save   :q quit   :wq save+quit   :l list   :d N delete line N   :h help
   ```
2. Input loop using `AH=0Ah` buffered line input (`DS:DX` DOS line-buffer format). The editor maintains an in-memory buffer of lines (cap: 200 lines, 128 chars each, statically allocated inside the COM image).
3. Each entered line that does not start with `:` is appended to the buffer. Lines starting with `:` are commands.
4. Commands:
   - `:w` - write buffer to file (see E3). Emit marker `[edit] save path=... lines=N bytes=M`.
   - `:q` - quit without saving. Emit `[edit] quit dirty=0|1`.
   - `:wq` - save then quit.
   - `:l` - list all lines with 1-based line numbers via `AH=09h`.
   - `:d N` - delete line N (1-based). Reject invalid N with `[edit] error class=bad_index`.
   - `:h` - reprint help banner.
   - any other `:` command → `[edit] error class=bad_command` and reprint help.
5. Exit path: terminate via `svc->terminate(ctx, return_code)` (NOT raw `INT 21h AH=4Ch` inline) with code `0x00` on clean exit, `0x01` on save error, `0x02` on parse error.

### E3) File I/O Path (FAT-Backed)
1. Resolve the filename from the COM command tail (`ctx->command_tail` / `command_tail_len`). If empty, default to `UNTITLED.TXT` and emit `[edit] warn class=no_filename default=UNTITLED.TXT`.
2. On launch, attempt `AH=3Dh` open-for-read of the target file. If it exists, pre-populate the buffer by reading via `AH=3Fh` in chunks and splitting on `\n` (accept `\r\n` as well, but write back with plain `\n` for determinism). If it does not exist, start with an empty buffer. Emit exactly one of:
   - `[edit] open path=... lines=N bytes=M`
   - `[edit] open path=... new=1`
3. On `:w` / `:wq`, use `AH=3Ch` create/truncate, then `AH=40h` write, then `AH=3Eh` close. Report:
   - success: `[edit] save path=... lines=N bytes=M`
   - failure: `[edit] error class=write rc=0xXX` and exit with code `0x01`.
4. Do NOT leak file handles. Always close on both success and error paths.

### E4) Shell + Image Wiring
1. Append build rules to `Makefile` for `CIUKEDIT.COM` following the `COM_HELLO_` pattern (payload + ELF + final COM). Add `$(COM_CIUKEDIT_BIN)` to the `all:` target.
2. Add an mcopy block in `run_ciukios.sh` that copies `build/CIUKEDIT.COM` to the FAT image alongside `INIT.COM` / `CIUKSMK.COM` / `CIUKMEM.EXE`.
3. Ensure the shell `run` and direct-exec paths already resolve `CIUKEDIT.COM`. No shell changes should be required; if any are, they must be minimal and documented in the handoff.

### E5) Deterministic Test Gate
1. Add `scripts/test_ciukedit_smoke.sh`:
   - Pattern: 120s QEMU timeout, then static fallback identical in spirit to `scripts/test_m6_dpmi_mem_smoke.sh`.
   - Required runtime markers (runtime path, when reachable): `[dosrun] launch path=CIUKEDIT.COM type=COM`, `[edit] open`, `[edit] save`, `[dosrun] result=ok code=0x00`.
   - Static fallback greps: the new sources under `com/ciukedit/`, the Makefile wiring, the run_ciukios.sh mcopy block, and `scripts/test_ciukedit_smoke.sh` itself for the marker strings. The fallback must `PASS` without a functional runtime.
2. Add `test-ciukedit-smoke` to `Makefile` `.PHONY` and as a standalone target.
3. DO NOT insert `test-ciukedit-smoke` into any aggregate chain in this PR (leave readiness/harness chains untouched). Aggregation will happen in a follow-up once the runtime path is validated end-to-end manually.

### E6) Documentation
1. Create `docs/sr-edit-001.md` with: goal, ABI surface used, UX command reference, file format rules (`\n` line-end on write), known limits, failure taxonomy.
2. Add a one-line entry to `Roadmap.md` under the appropriate user-tooling section (create the section if missing), marked `DONE`.
3. Do NOT bump the version yet. Do NOT touch `CHANGELOG.md`, `README.md`, `documentation.md`, `stage2/include/version.h`, `CLAUDE.md` unless the main agent explicitly asks for a version bump.

### E7) Handoff
Create `docs/handoffs/YYYY-MM-DD-copilot-sr-edit-001.md` with:
1. Context and goal.
2. Files touched (new + modified).
3. Decisions (line vs full-screen editor, buffer caps, newline normalization, exit-code taxonomy).
4. Validation: exact commands run and their PASS/FAIL status.
5. Risks and next step.

## Validation (run before handoff)
```bash
make clean all
make -C boot/uefi-loader clean all
bash scripts/test_ciukedit_smoke.sh
make test-stage2
make test-mz-regression
make test-m6-pmode
make test-vga13-baseline
make test-m6-dpmi-ldt-smoke
make test-m6-dpmi-mem-smoke
make test-doom-boot-harness
make test-doom-target-packaging
```
All must PASS. Attach the tail of each run to the handoff.

## Non-Goals (out of scope on purpose)
1. Full-screen editing, colored UI, mouse support.
2. Multi-file editing, undo/redo history, search/replace.
3. Syntax highlighting.
4. Any new INT 21h function in stage2.
5. Version bump and changelog entries.

## Reviewer Checklist
1. No writes to `main`; PR targets a feature branch.
2. No MS-DOS or Microsoft proprietary bytes committed.
3. INT 21h surface used matches the implemented list in `docs/int21-priority-a.md`.
4. `CIUKEDIT.COM` builds from source and appears in the FAT image.
5. New test gate passes both runtime (when reachable) and static fallback.
6. All pre-existing gates still PASS.
7. Handoff file present and complete.
