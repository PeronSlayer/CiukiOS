# Handoff - Codex X3/X4 (INT 21h Priority-A + BIOS Test Harness)

## Branch
- `feature/codex-m2-m4-int21-bios-tests`

## Scope completed

### X3 - INT 21h Priority-A Expansion
Implemented/normalized in `stage2/src/shell.c`:

1. `AH=00h` -> terminate (`INT 20h` equivalent path)
2. `AH=01h` -> blocking input + echo
3. `AH=02h` -> char output in `DL`, mirrored in `AL`
4. `AH=08h` -> blocking input without echo
5. `AH=09h` -> `$`-terminated print, returns `AL=24h`
6. `AH=19h` -> current drive (`AL=0`, A:)
7. `AH=25h` -> set vector from `DS:DX`
8. `AH=30h` -> DOS version 6.22
9. `AH=35h` -> get vector into `ES:BX`
10. `AH=4Ch` -> terminate with return code

Deterministic stubs (until allocator work):
- `AH=48h` -> `CF=1`, `AX=0008h`
- `AH=49h` -> `CF=1`, `AX=0009h`
- `AH=4Ah` -> `CF=1`, `AX=0008h`

Unsupported `AH` behavior is deterministic:
- `CF=1`, `AX=0001h`

### X4 - BIOS Compatibility Test Harness
Added baseline compatibility markers in `stage2/src/stage2.c`:

- `[ compat ] INT10h baseline path ready (stage2 video text/gfx)`
- `[ compat ] INT16h baseline path ready (irq1 + key buffer)`
- `[ compat ] INT1Ah baseline path ready (pit tick source)`

Updated `scripts/test_stage2_boot.sh` to assert these markers.

## Docs updated
- `docs/int21-priority-a.md`
  - now reflects current implemented set + deterministic stubs + next extensions.

## Files changed
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `scripts/test_stage2_boot.sh`
- `docs/int21-priority-a.md`

## Validation
All pass:

1. `make test-stage2`
2. `make test-fallback`
3. `make test-fat-compat`

## Notes for next iteration
1. Replace memory stubs (`48h/49h/4Ah`) with real MCB allocator integration.
2. Add INT 21h file-handle subset (`3Ch..42h`) with DOS-like error mapping.
3. Add dedicated tiny COM test binaries for each implemented AH branch.
