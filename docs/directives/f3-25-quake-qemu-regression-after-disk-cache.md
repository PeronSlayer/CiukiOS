# f3-25 — Quake does not start on QEMU after the disk read-cache change (image aae6a170)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-25` from `main` (`0712bc59`). Host-testable; the lead runs the QEMU evidence.

## Evidence

`quake-qemu-t23` (suite `f3-workloads`, run `3c5fee8f`, image `aae6a170`): `case=video width=640 height=400 text=1 changes=0`, `case=memory … dpmi_active=0 entries=0`, `case=audio … frames=0`, `case=exit status=0 rc=0 … error=-5`, END FAIL error=-5; `vm_idle_counts` all zero: the screen never changed from text mode and DPMI was never activated. `dos4gw-qemu-t23` passed in the same chain. The case passed before 2026-10-15; since then main merged f3-24 (two 64 KiB read-ahead windows on the backing image with the private overlay checked first, screen-text records behind `CBI_F_VERBOSE`, per-vector cost records), f3-23, f0-22 (`CBI_F_DIAG_VERBOSE` from the fw_cfg selector, loader changes), f3-21 and f2-66.

## Task

1. Find why the Quake session does not start: the read cache first (window crossing the end of the base image, `base_sectors` bound, stale window after an overlay write inside a cached range, cache not reset per VM generation, metadata window returning data from the wrong image), then the probe's launch path (RUNREC/AUTOEXEC, the in-guest PAK hash loop), then the boot flags.
2. Host test in the dosvm harness reproducing the failure with the real layout semantics (reads spanning the window boundary, overlay sectors inside a cached window, end-of-image window), failing before the fix.
3. Fix.
4. The f3-24 screen-text records must also appear on QEMU runs that set `CBI_F_DIAG_VERBOSE`, bounded as before.
5. Kernel host suite green.

## Acceptance

`quake-qemu-t23`, `dos4gw-qemu-t23` and `f0-smoke` pass on the next image. Report `build/codex/f3-25-report.md`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`; `rg` not semble.
