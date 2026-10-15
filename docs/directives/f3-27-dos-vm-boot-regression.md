# f3-27 — Every DOS VM faults at 0000:7C00 on image 52e3736a (QEMU gates of the second wave)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-27` from `main` (`97dc5f7e`).

## Evidence

`dos4gw-qemu-t23` (run `333797e2`): `case=loops completed=0 required=20`, `group=vm_stop vm=1 generation=2346 reason=2 vector=14 eip=00007c00 address=00007c00 error=4 code=eb3c904349554b46`, END FAIL error=-5; `vm-audio-qemu-t23` fails the same way with correct audio data. The same case passed on image `aae6a170` (main `0712bc59`).

## Finding

`dosvm_startup_report` (f3-24) runs after the VM's private directory is selected and before `v86_enter` creates the continuation; it calls `schedule()` when a reschedule is pending, and the CR3 hook still required a continuation to choose the VM directory, so the task resumed under the kernel directory and the first boot-sector instruction faulted. f3-25's report gate (`CBI_F_DIAG_VERBOSE`) exposed the window to the probe boots.

## Task

Reproduce with a host regression that forces the startup scheduling boundary and executes the assembled boot-sector entry through the active page tables; fix minimally; kernel host suite and kernel build.

## Acceptance

`dos4gw-qemu-t23`, `vm-audio-qemu-t23`, `quake-qemu-t23`, `f0-smoke` pass on the next image. Report `build/codex/f3-27-report.md`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`.

## State

Delivered and merged 2026-10-15.
