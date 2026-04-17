# SR-DOSRUN-001 - First Simple DOS Program Execution

## Objective
Reach a deterministic sub-milestone where CiukiOS can run a simple DOS user program end-to-end (load, execute, return status) with automated validation.

## Scope (short roadmap)
1. `DONE` COM runtime baseline exists.
2. `DONE` deterministic smoke path: launch known simple DOS program from shell + verify return code (`CIUKSMK.COM` -> `0x2A`).
3. `IN PROGRESS` minimal EXE/MZ single-program smoke alongside COM smoke.
4. `DONE` errorlevel/return-code parity checks (`AH=4Ch` -> `AH=4Dh` one-shot behavior) with launch-path integrated selftest marker.
5. `DONE` small command-level compatibility matrix for `run` UX (`success/not found/bad format/runtime`) with deterministic serial markers.

## Exit Criteria
1. Non-interactive gate passes on clean boot and validates:
- program launch marker
- success return marker
- no panic/#UD
2. Dedicated gate available as `make test-dosrun-simple`.
3. Shell remains usable after program return.
