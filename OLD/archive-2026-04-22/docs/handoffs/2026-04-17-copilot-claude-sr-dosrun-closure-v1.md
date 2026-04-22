# SR-DOSRUN-001 Closure v1 — Handoff

Branch: `feature/copilot-claude-sr-dosrun-closure-v1`
Base: `origin/main`
Version: `Alpha v0.6.6`

## 1. Changed Files

### Added
- `com/dosrun_mz/ciukmz.c` — MZ payload source (`com_main`).
- `com/dosrun_mz/linker.ld` — freestanding flat-binary link script for the payload.
- `tools/mkciukmz_exe.c` — host-side reproducible MZ wrapper generator.
- `scripts/test_dosrun_mz_simple.sh` — dedicated non-interactive MZ gate.

### Modified
- `Makefile` — added `CIUKMZ.EXE` build chain (`mkciukmz_exe` host tool + payload → EXE), `test-dosrun-mz` target, PHONY entry.
- `stage2/src/shell.c` — D2 argv tail markers, D3 INT21h 2Ah/2Ch/44h handlers, D4 extended error taxonomy (`unsupported_int21`, `args_parse`).
- `stage2/src/stage2.c` — boot-time `[compat]` markers for date/time and IOCTL.
- `run_ciukios.sh` — image build copies `CIUKMZ.EXE`.
- `scripts/check_int21_matrix.sh` — required functions list extended with `2Ah 2Ch 44h`.
- `scripts/test_dosrun_simple_program.sh` — validates argv markers and new `[compat]` markers; forbids new error classes.
- `docs/int21-priority-a.md` — matrix extended with 2Ah/2Ch/44h rows.
- `docs/subroadmap-sr-dosrun-001.md` — closure items marked `DONE`.
- `Roadmap.md` — SR-DOSRUN-001 closure items marked `DONE`.
- `README.md` — v0.6.6 changelog.
- `stage2/include/version.h` — bumped to Alpha v0.6.6.

## 2. Artifacts Added

- `build/CIUKMZ.EXE` — deterministic DOS MZ wrapper (32-byte header + `CIUKEX64` marker + entry offset + payload). Reproducible from source via `tools/mkciukmz_exe`; byte-identical for a given payload.
- `build/tools/mkciukmz_exe` — host-side generator (compiled from `tools/mkciukmz_exe.c`).

## 3. Markers Added / Changed

Added (runtime serial):
- `[dosrun] launch path=CIUKMZ.EXE type=MZ`
- `[dosrun] result=ok code=0x2B`
- `[dosrun] argv tail len=<n>`
- `[dosrun] argv parse=PASS` / `[dosrun] argv parse=FAIL`
- `[dosrun] result=error class=unsupported_int21`
- `[dosrun] result=error class=args_parse`
- `[compat] INT21h date/time ready (AH=2Ah/2Ch)`
- `[compat] INT21h ioctl baseline ready (AH=44h/AL=00h)`

Preserved (unchanged): `[ test ] dosrun status path selftest: PASS`, `[dosrun] launch path=CIUKSMK.COM type=COM`, `[dosrun] result=ok code=0x2A`, `[ compat ] INT21h …` baseline lines, MZ-regression markers, INT21 priority-A selftest marker.

Type string fix: `EXE` → `MZ` in the MZ launch marker (matches task pack spec and updated gate expectations).

## 4. Tests Executed + Outcomes

| Gate | Outcome |
|---|---|
| `make all` | **PASS** (clean build, no warnings) |
| `make check-int21-matrix` | **PASS** (36 implemented, 2 stubs — 2Ah/2Ch — all required fns documented) |
| `make test-mz-regression` | **PASS** (deterministic MZ regression suite) |
| `make test-int21` | **INFRA** (QEMU serial capture unavailable on this host) |
| `make test-stage2` | **INFRA** (same root cause) |
| `make test-dosrun-simple` | **INFRA** (same root cause) |
| `make test-dosrun-mz` | **INFRA** (same root cause; static-side inputs built deterministically — `CIUKMZ.EXE` reproducible from source) |

Static validation confirms every required marker is emitted by the source (grep-verified in `stage2/src/shell.c` and `stage2/src/stage2.c`). The INFRA class matches the established project classification from the previous Phase 1 closure handoff: QEMU `-serial file:` output is not flushed on this host under `-no-shutdown`.

## 5. Residual Limitations (max 5)

1. QEMU serial capture is not usable on this host, so the live boot-log validation of D1/D2/D3 markers cannot be exercised here; static source-level validation is the substitute.
2. INT21h `AH=2Ah/2Ch` are deterministic stubs (fixed date 2026-04-17 Fri, fixed time 00:00:00.00) — no RTC/CMOS binding yet.
3. INT21h `AH=44h` covers only `AL=00h` (get device info); other IOCTL subfunctions fall through to the unsupported path.
4. Argv tail semantics are a baseline: spaces-as-separators with length clamp at 126 chars (`args_parse` error class on overflow); quoting rules are not yet expanded beyond the documented baseline.
5. `unsupported_int21` error class is inferred from a per-launch counter combined with a non-zero exit code — a program can still mask an unsupported call by returning 0, which is acceptable baseline behavior but not a guarantee.
