# Automatic EXEC Memory Allocation

CIUKIDOS no longer chooses a normal child location from fixed `1800h`, `3800h`, or `7800h` program windows. Return-context depth and memory placement are separate decisions.

For a COM child, EXEC calculates PSP + file + a bounded stack reserve, finds the first live-arena gap which fits, and sets `SP` from the block actually assigned. Unused conventional memory remains available to AH=48h or another nested EXEC.

For an MZ child, EXEC preloads the standard header, derives the same compatibility copy extent used by the loader, adds `minalloc` and PSP/header safety space, then selects the first fitting interval. The scan respects the active parent's PSP end, allocated/resident MCB entries, and the exclusive arena limit. `maxalloc` is still bounded dynamically when the child PSP block is constructed.

There are no `PCDOOM`, `DOOMVAN`, `DOSNAV`, or `DN.COM` branches in the kernel allocator; the DOS compatibility smoke test enforces that policy.

Validated through 2026-09-01:

- doom-vanille/DOS4GW passes the 256 KiB low-DOS allocation lane and reaches `ST_Init`;
- DOSNavigator completes its COM→MZ startup, renders the dual-pane UI, accepts mouse and one-row arrow movement, completes Colors/XMS plus `EXIT`, and restores the shell without an allocation error;
- CIUKPST passes nested COM/MZ termination, parent restoration, TSR survival, and unload;
- Costa still passes its three-MZ desktop/cursor/Calculator workflow;
- Windows 3.1 completes two 386 Enhanced Mode sessions with child-task `Alt+F4`, clean shell return, and XMS/EXEC state restoration.
- `NETSTART` relocates its transient stack, shrinks its own COM allocation through `INT 21h/AH=4Ah`, and then starts `NE2000.COM` through the ordinary nested `AH=4Bh` path; no executable-name exception is present in the kernel.
