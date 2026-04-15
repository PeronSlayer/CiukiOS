# INT 21h - Priority A (Compatibility Bootstrap)

## Goal
Provide a deterministic DOS-like syscall baseline for early `.COM`/`.EXE` compatibility work.

## Implemented in Stage2 (Current)
1. `AH=00h` - terminate program (`INT 20h` equivalent path).
2. `AH=01h` - blocking char input with echo (returns char in `AL`).
3. `AH=02h` - output character in `DL` (returns char in `AL`).
4. `AH=08h` - blocking char input without echo.
5. `AH=09h` - print `$`-terminated string.
6. `AH=19h` - get current default drive (`AL=0`, drive `A:`).
7. `AH=25h` - set interrupt vector (stores far pointer from `DS:DX`).
8. `AH=30h` - get DOS version (`6.22`).
9. `AH=35h` - get interrupt vector (returns far pointer in `ES:BX`).
10. `AH=4Ch` - terminate process with return code.

## Deterministic Stubs (Not Fully Implemented Yet)
1. `AH=48h` - allocate memory block: returns `CF=1`, `AX=0008h`.
2. `AH=49h` - free memory block: returns `CF=1`, `AX=0009h`.
3. `AH=4Ah` - resize memory block: returns `CF=1`, `AX=0008h`.

These stubs are intentional until MCB-style allocator work is completed in roadmap M2.

## Unsupported Function Behavior
1. Any non-implemented `AH` returns deterministic error:
2. `CF=1`, `AX=0001h` (invalid function number).

## Next Priority-A Extensions
1. Formal memory allocator backend for `48h/49h/4Ah`.
2. Handle-based file APIs (`3Ch-42h`) behind INT 21h.
3. Additional DOS error code mapping consistency checks.

## Test Criteria per Function
1. Nominal case.
2. Error case.
3. Correct flags/return code (`CF`, `AX`).
4. Stable behavior across repeated calls (idempotent errors where expected).
