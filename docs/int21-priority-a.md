# INT 21h - Priority A (Compatibility Bootstrap)

## Goal
Provide a deterministic DOS-like syscall baseline for early `.COM`/`.EXE` compatibility work.

## Implemented in Stage2 (Current)
1. `AH=00h` - terminate program (`INT 20h` equivalent path).
2. `AH=01h` - blocking char input with echo (returns char in `AL`).
3. `AH=02h` - output character in `DL` (returns char in `AL`).
4. `AH=08h` - blocking char input without echo.
5. `AH=0Bh` - keyboard status (`AL=00h` empty, `AL=FFh` ready).
6. `AH=0Ch` - keyboard flush + deterministic follow-up input dispatch subset.
7. `AH=09h` - print `$`-terminated string.
8. `AH=19h` - get current default drive (`AL=0`, drive `A:`).
9. `AH=25h` - set interrupt vector (stores far pointer from `DS:DX`).
10. `AH=30h` - get DOS version (`6.22`).
11. `AH=35h` - get interrupt vector (returns far pointer in `ES:BX`).
12. `AH=3Eh` - close handle (supports std handles `0/1/2`, validates others).
13. `AH=3Fh` - read handle (stdin baseline on handle `0`, deterministic errors for others).
14. `AH=40h` - write handle (stdout/stderr baseline on handles `1/2`, deterministic errors for others).
15. `AH=4Dh` - get last process return code (`AL`) + termination type (`AH`).
16. `AH=51h` - get current PSP segment (`BX`).
17. `AH=62h` - get current PSP segment (`BX`) (DOS 3+ style alias).
18. `AH=4Ch` - terminate process with return code.

## Deterministic Stubs (Not Fully Implemented Yet)
1. `AH=3Ch` - create/truncate file: returns `CF=1`, `AX=0005h`.
2. `AH=3Dh` - open file: returns `CF=1`, `AX=0002h`.
3. `AH=41h` - delete file: returns `CF=1`, `AX=0002h`.
4. `AH=42h` - seek: deterministic baseline (std handles return `DX:AX=0`, others `CF=1`, `AX=0006h`).
5. `AH=48h` - allocate memory block: returns `CF=1`, `AX=0008h`.
6. `AH=49h` - free memory block: returns `CF=1`, `AX=0009h`.
7. `AH=4Ah` - resize memory block: returns `CF=1`, `AX=0008h`.

These stubs are intentional until MCB-style allocator work is completed in roadmap M2.

## Unsupported Function Behavior
1. Any non-implemented `AH` returns deterministic error:
2. `CF=1`, `AX=0001h` (invalid function number).

## INT21h Compatibility Matrix (Automated Gate)
This matrix is the single source of truth for Priority-A INT21h function status.
Format: Function Code | Status | Implementation | Notes

```
FN  | Status               | Implementation Details
----|------|---------|-----------|--
00h | IMPLEMENTED          | Program termination via exit code register
01h | IMPLEMENTED          | Blocking char input with echo
02h | IMPLEMENTED          | Blocking char output
08h | IMPLEMENTED          | Blocking char input without echo
0Bh | IMPLEMENTED          | Keyboard status (AL=00h/FFh)
0Ch | IMPLEMENTED          | Flush keyboard buffer + deterministic follow-up subset
09h | IMPLEMENTED          | Print $-terminated string
19h | IMPLEMENTED          | Get current default drive (fixed to A:)
25h | IMPLEMENTED          | Set interrupt vector (stores pointer DS:DX)
30h | IMPLEMENTED          | Get DOS version (returns 6.22)
35h | IMPLEMENTED          | Get interrupt vector (returns pointer ES:BX)
3Ch | DETERMINISTIC_STUB   | Create/truncate returns access denied (AX=0005h)
3Dh | DETERMINISTIC_STUB   | Open returns file not found (AX=0002h)
3Eh | IMPLEMENTED          | Close supports std handles 0/1/2, validates others
3Fh | IMPLEMENTED          | Read supports stdin handle 0 baseline, deterministic errors otherwise
40h | IMPLEMENTED          | Write supports stdout/stderr handles 1/2 baseline
41h | DETERMINISTIC_STUB   | Delete returns file not found (AX=0002h)
42h | DETERMINISTIC_STUB   | Seek baseline (std handles deterministic zero, others invalid handle)
4Ch | IMPLEMENTED          | Terminate with return code
4Dh | IMPLEMENTED          | Get last process return code + type
51h | IMPLEMENTED          | Get current PSP segment to BX
62h | IMPLEMENTED          | Get current PSP segment to BX (DOS 3+ alias)
48h | DETERMINISTIC_STUB   | Memory alloc (returns CF=1, AX=0008h, blocked until M2)
49h | DETERMINISTIC_STUB   | Memory free (returns CF=1, AX=0009h, blocked until M2)
4Ah | DETERMINISTIC_STUB   | Memory resize (returns CF=1, AX=0008h, blocked until M2)
*   | UNSUPPORTED          | Invalid function (returns CF=1, AX=0001h)
```

## Next Priority-A Extensions
1. Formal memory allocator backend for `48h/49h/4Ah`.
2. Real FAT-backed handle table for `3Ch-42h` (replace current deterministic stubs/baseline).
3. Additional DOS error code mapping consistency checks.

## Test Criteria per Function
1. Nominal case.
2. Error case.
3. Correct flags/return code (`CF`, `AX`).
4. Stable behavior across repeated calls (idempotent errors where expected).

## Matrix Validation Gate (CI Integration)
1. **Automated check**: `make check-int21-matrix`
   - Validates that INT21h Compatibility Matrix section exists in this document.
   - Verifies all required functions have documented status.
   - Ensures status values are valid: IMPLEMENTED, DETERMINISTIC_STUB, or UNSUPPORTED.
   - Counts implementation coverage and reports advisory warnings if coverage is low.
2. **Integration point**:
   - Run in CI/pre-commit to ensure documentation matches implementation intent.
   - Script fails if matrix is incomplete or malformed; passes otherwise.
3. **Gate enforcement**:
   - If adding new INT21h functions, update matrix row in this document.
   - If changing function status, update matrix and re-run check.
