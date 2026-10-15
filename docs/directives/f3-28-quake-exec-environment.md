# f3-28 — Quake still refused by DOS EXEC after f3-25: "RUNREC FAIL R0A", rc=10 (image 072d27f0)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-28` from `main` (`f3ecd8c5`).

## Evidence

`quake-qemu-t23` (run `926f41bd`): the guest screen records show `\RUNREC.COM \QUAKE QUAKE.EXE +playdemo demo1` answered by `RUNREC FAIL R0A T00`; probe records `case=exit status=1 rc=10`, `case=memory … dpmi_active=0`, `case=video … text=1 changes=0`. Before f3-25 the same launch failed with `E000B` (DOS error 11 from the relocation-table validator); the executable now passes that check and EXEC fails later with code 10.

## Task

Decode R0A/T00 in `src/probes/dos/run_record.asm` and the corresponding EXEC stage in the DOS kernel (`src/boot/floppy_stage1.asm`): environment block with the program's full path after the count word (the DJGPP v2 stub of Quake 1.06 uses it), allocation per `e_minalloc`/`e_maxalloc` (largest block when `FFFF`), PSP fields; host test running EXEC for this executable (fails before, passes after); minimal fix keeping every existing EXEC test; kernel host suite and kernel build.

## Acceptance

`quake-qemu-t23`, `dos4gw-qemu-t23` and the `f3-vm` cases pass on the next image. Report `build/codex/f3-28-report.md`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`.
