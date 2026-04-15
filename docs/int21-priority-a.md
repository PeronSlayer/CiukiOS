# INT 21h - Priority A (Compatibility Bootstrap)

## Goal
Implement first the functions that unlock basic shell and program execution.

## Minimum Priority-A Set
1. `AH=09h` - print `$`-terminated string.
2. `AH=02h` - output character in `DL`.
3. `AH=01h` - input character with echo.
4. `AH=08h` - input character without echo.
5. `AH=4Ch` - terminate process with return code.
6. `AH=25h` - set interrupt vector.
7. `AH=35h` - get interrupt vector.
8. `AH=48h` - allocate memory block.
9. `AH=49h` - free memory block.
10. `AH=4Ah` - resize memory block.

## Minimal File I/O Subset (A2)
1. `AH=3Ch` - create file.
2. `AH=3Dh` - open file.
3. `AH=3Eh` - close file.
4. `AH=3Fh` - read file/device.
5. `AH=40h` - write file/device.
6. `AH=41h` - delete file.
7. `AH=42h` - lseek.

## Test Criteria per Function
1. Nominal case.
2. Error case.
3. Correct flags/return code (`CF`, `AX`).

## Recommended Implementation Order
1. Console (`01h/02h/08h/09h`).
2. Process exit (`4Ch`).
3. Memory (`48h/49h/4Ah`).
4. Vectors (`25h/35h`).
5. File I/O (`3Ch-42h`).
