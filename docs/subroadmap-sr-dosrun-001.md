# SR-DOSRUN-001 - First Simple DOS Program Execution

## Objective
Reach a deterministic sub-milestone where CiukiOS can run a simple DOS user program end-to-end (load, execute, return status) with automated validation.

## Scope (short roadmap)
1. `DONE` COM runtime baseline exists.
2. `DONE` deterministic smoke path: launch known simple DOS program from shell + verify return code (`CIUKSMK.COM` -> `0x2A`).
3. `DONE` minimal EXE/MZ single-program smoke alongside COM smoke (`CIUKMZ.EXE` -> `0x2B`, reproducible from source via `tools/mkciukmz_exe`).
4. `DONE` errorlevel/return-code parity checks (`AH=4Ch` -> `AH=4Dh` one-shot behavior) with launch-path integrated selftest marker.
5. `DONE` extended command-level compatibility matrix for `run` UX (`success/not_found/bad_format/runtime/unsupported_int21/args_parse`) with deterministic serial markers + argv tail markers.
6. `DONE` INT21h coverage extended to date/time (`AH=2Ah`, `AH=2Ch`) and IOCTL get-device-info (`AH=44h`/`AL=00h`) with boot-time `[compat]` markers.

## Exit Criteria
1. Non-interactive gates pass on clean boot and validate:
- program launch markers (COM + MZ)
- success return markers (0x2A for COM, 0x2B for MZ)
- argv parse=PASS marker
- no panic/#UD
2. Dedicated gates available as `make test-dosrun-simple` and `make test-dosrun-mz`.
3. Shell remains usable after program return.
4. INT21 compatibility matrix gate (`make check-int21-matrix`) green with 2Ah/2Ch/44h documented.
