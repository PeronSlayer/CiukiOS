# Automatic EXEC Memory Allocation

CIUKIDOS no longer chooses a normal child location from fixed `1800h`, `3800h`, or `7800h` program windows. Return-context depth and memory placement are separate decisions.

For a COM child, EXEC calculates PSP + file + a bounded stack reserve and finds the first live-arena gap which fits. It then follows standard DOS COM behavior by assigning the complete selected interval and deriving `SP` from that owned block. A COM program that launches another child must relocate its live stack and shrink its PSP block with `AH=4Ah` before nested `AH=4Bh`; the external shell and every project-owned COM launcher now follow that contract.

For an MZ child, EXEC preloads the standard header, derives the same compatibility copy extent used by the loader, adds `minalloc` and PSP/header safety space, then selects the first fitting interval. The scan respects the active parent's PSP end, allocated/resident MCB entries, and the exclusive arena limit. `maxalloc` is still bounded dynamically when the child PSP block is constructed.

There are no `PCDOOM`, `DOOMVAN`, `DOSNAV`, or `DN.COM` branches in the kernel allocator; the DOS compatibility smoke test enforces that policy.

Validated through 2026-09-01:

- doom-vanille/DOS4GW passes the 256 KiB low-DOS allocation lane and reaches `ST_Init`;
- the unmodified upstream DOSNavigator binary renders the dual-pane UI, accepts mouse and one-row arrow movement, completes Colors/XMS plus native `Alt+X` exit, restores the shell, and leaves ownership/mouse/video probes green in the same boot;
- CIUKPST passes nested COM/MZ termination, parent restoration, TSR survival, and unload;
- Costa still passes its three-MZ desktop/cursor/Calculator workflow;
- Windows 3.1 completes two 386 Enhanced Mode sessions with child-task `Alt+F4`, clean shell return, and XMS/EXEC state restoration.
- `NETSTART`, `DRVLOAD`, `PMIRQSB`, `DOOMSFX`, `DOOMVAN`, `DOOMSB`, and `NETCFG` relocate their transient stack, shrink their own COM allocation through `INT 21h/AH=4Ah`, and start children through the ordinary nested `AH=4Bh` path; no executable-name exception is present in the kernel.
- the three-layer audio gate proves `DRVLOAD -> SB16INIT` nested COM return, DOS/4GW protected-mode playback, and doom-vanille's external SB16 mixer without changing the kernel allocator.
