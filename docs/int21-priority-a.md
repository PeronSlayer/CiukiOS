# INT 21h - Priority A (Compatibility Bootstrap)

## Goal
Provide a deterministic DOS-like syscall baseline for early `.COM`/`.EXE` compatibility work.

## Implemented in Stage2 (Current)
1. `AH=00h` - terminate program (`INT 20h` equivalent path).
2. `AH=01h` - blocking char input with echo (returns char in `AL`).
3. `AH=02h` - output character in `DL` (returns char in `AL`).
4. `AH=06h` - direct console I/O (output char, or non-blocking input when `DL=FFh`).
5. `AH=07h` - blocking char input without echo.
6. `AH=08h` - blocking char input without echo.
7. `AH=09h` - print `$`-terminated string.
8. `AH=0Ah` - buffered line input (`DS:DX` DOS line-buffer format).
9. `AH=0Bh` - keyboard status (`AL=00h` empty, `AL=FFh` ready).
10. `AH=0Ch` - keyboard flush + deterministic follow-up input dispatch subset (`01h`, `08h`, `0Ah`).
11. `AH=0Eh` - set default drive (`DL=0..25`, deterministic return `AL=01h`).
12. `AH=19h` - get current default drive.
13. `AH=1Ah` - set DTA pointer (`DS:DX`).
14. `AH=25h` - set interrupt vector (stores far pointer from `DS:DX`).
15. `AH=2Fh` - get DTA pointer (`ES:BX`).
16. `AH=30h` - get DOS version (`6.22`).
17. `AH=35h` - get interrupt vector (returns far pointer in `ES:BX`).
18. `AH=3Eh` - close handle (supports std handles `0/1/2`, validates others).
19. `AH=3Fh` - read handle (stdin baseline on handle `0`, deterministic errors for others).
20. `AH=40h` - write handle (stdout/stderr baseline on handles `1/2`, deterministic errors for others).
21. `AH=4Dh` - get last process return code (`AL`) + termination type (`AH`) with DOS-like one-shot consume-on-read behavior.
22. `AH=51h` - get current PSP segment (`BX`).
23. `AH=62h` - get current PSP segment (`BX`) (DOS 3+ style alias).
24. `AH=4Ch` - terminate process with return code.
25. `AH=48h` - allocate memory block (paragraph allocator baseline).
26. `AH=49h` - free memory block by segment (`ES`).
27. `AH=4Ah` - resize memory block by segment (`ES`) to `BX` paragraphs.
28. `AH=43h` - get/set file attributes (FAT-backed path).
29. `AH=56h` - rename file/directory entry (same-directory DOS-like subset).
30. `AH=4Eh` - find first matching file entry (wildcard search via DTA).
31. `AH=4Fh` - find next matching file entry (continues active DTA search).

## Partial Compatibility Notes
1. `AH=48h/49h/4Ah` currently use an internal paragraph heap allocator baseline (no full MCB chain yet).
2. Error conventions are DOS-like (`AX=0008h` for insufficient memory, `AX=0009h` for invalid block).
3. Full MCB-compatible metadata and ownership semantics remain roadmap M2 hardening work.

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
06h | IMPLEMENTED          | Direct console I/O (DL!=FFh output, DL=FFh non-blocking input)
07h | IMPLEMENTED          | Blocking char input without echo
08h | IMPLEMENTED          | Blocking char input without echo
09h | IMPLEMENTED          | Print $-terminated string
0Ah | IMPLEMENTED          | Buffered line input via DOS-compatible input buffer
0Bh | IMPLEMENTED          | Keyboard status (AL=00h/FFh)
0Ch | IMPLEMENTED          | Flush keyboard buffer + deterministic follow-up subset
0Eh | IMPLEMENTED          | Set current default drive (returns AL=01h)
19h | IMPLEMENTED          | Get current default drive (runtime state)
1Ah | IMPLEMENTED          | Set DTA pointer from DS:DX
25h | IMPLEMENTED          | Set interrupt vector (stores pointer DS:DX)
2Fh | IMPLEMENTED          | Get DTA pointer in ES:BX
30h | IMPLEMENTED          | Get DOS version (returns 6.22)
35h | IMPLEMENTED          | Get interrupt vector (returns pointer ES:BX)
3Ch | IMPLEMENTED          | FAT-backed create/truncate + handle return (fallback stub when FAT unavailable)
3Dh | IMPLEMENTED          | FAT-backed open + in-memory file handle (fallback stub when FAT unavailable)
3Eh | IMPLEMENTED          | Close supports std handles 0/1/2, validates others
3Fh | IMPLEMENTED          | Read supports stdin handle 0 baseline, deterministic errors otherwise
40h | IMPLEMENTED          | Write supports stdout/stderr handles 1/2 baseline
41h | IMPLEMENTED          | FAT-backed delete by DOS path (fallback stub when FAT unavailable)
42h | IMPLEMENTED          | Seek on opened file handles + std-handle deterministic baseline
43h | IMPLEMENTED          | Get/set FAT-backed file attributes by DOS path
56h | IMPLEMENTED          | Rename file/dir entry (same-directory subset, DS:DX old + ES:DI new)
4Ch | IMPLEMENTED          | Terminate with return code
4Dh | IMPLEMENTED          | Get last process return code + type (one-shot consume-on-read)
51h | IMPLEMENTED          | Get current PSP segment to BX
62h | IMPLEMENTED          | Get current PSP segment to BX (DOS 3+ alias)
48h | IMPLEMENTED          | Paragraph allocator baseline; returns AX=segment on success, AX=0008h/BX=max on failure
49h | IMPLEMENTED          | Free allocated paragraph block by ES segment; AX=0009h on invalid segment
4Ah | IMPLEMENTED          | Resize allocated block in place when possible; AX=0008h/BX=max on failure
4Eh | IMPLEMENTED          | Find first wildcard match in FAT directory, writes DOS-like DTA result
4Fh | IMPLEMENTED          | Find next wildcard match using active find state + DTA
*   | UNSUPPORTED          | Invalid function (returns CF=1, AX=0001h)
```

## Next Priority-A Extensions
1. Upgrade `48h/49h/4Ah` allocator from baseline heap model to stronger MCB-compatible semantics.
2. Expand FAT-backed handle table limits and add multi-chunk file buffering beyond current in-memory cap.
3. Improve DOS-like line editor behavior for `0Ah` (editing keys/history/overflow signaling).
4. Additional DOS error code mapping consistency checks.

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
