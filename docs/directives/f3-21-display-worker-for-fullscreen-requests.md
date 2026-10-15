# f3-21 — Queued DOS full-screen requests must be serviced without a prior display mode switch (QEMU refusal on image 35b9e5e4)

- **Model/effort:** Claude opus agent (Codex credit exhausted until 2026-10-20). **Worktree:** `../CiukiOS-wt-f3-21` from `main` (`70e124e7`). Host-testable; the lead runs the QEMU evidence. Time box 25 minutes.

## Request

`vm-fullscreen-qemu-t23` (suite `f3-vm`) failed on image `35b9e5e4`: after `ARM action=fullscreen_enter` and the runner's Alt+Enter the kernel logged `[dosvm] fullscreen_refused vm=1 generation=2509 error=-1` (-EPERM) and the probe's 10 s wait expired. -EPERM can only come from `display_fullscreen_begin` (`src/kernel/core/display.c`): no display owner, desktop stopping, or owner without a writable display descriptor. Find which condition fires and why, with file:line evidence; compare with the passing `vm-window-qemu-desktop-2002`; check whether the desktop's `--test=vm-fullscreen` mode touches the grant or the lease.

## Finding

The refusal is a side effect. `dosvm_fullscreen_input` only queues `fullscreen_request`; `dosvm_fullscreen_poll` runs from the `display-watchdog` task, which was created only by a display mode switch (`display_set`) or by `display_fullscreen_begin` itself. No mode switch had happened in that run, so nobody polled the request until the probe's cleanup, when the desktop was already stopping. The desktop still owned the grant with a writable display descriptor 17 lines before ARM (the forced `[cpu]` records after each `[desktop] stall` prove it).

## Task

Minimal fix with a host test that fails before and passes after: `display_worker_start()` creates the worker once (idempotent); `display_set` and `display_fullscreen_begin` use it; window VM creation (`dosvm_client.c`) starts it through a weak reference, failing the launch with -ENOMEM if the task cannot be created. Test in `tests/host/display_test.c`: the worker starts once before any mode switch, a second call does nothing, full-screen entry reuses it.

## Acceptance

`tests/host/test_display_modes.py` and the kernel host suite green; on the next image `vm-fullscreen-qemu-t23` shows `[display] fullscreen=1 mode=19` and `[dosvm] fullscreen=1` within about 200 ms of the Alt+Enter, then `case=demo active=1 ms>=10000`, `case=restore active=0`, END PASS. If still refused, the lead's refusal-reason log (`[display] fullscreen_refused reason=…`, commit `2bfec540`) names the condition.

## Constraints

No QEMU, no `systemd-run`, no image builds, no commits, no diary, `nice -n 19`, host tests one at a time. Report `build/lead-scratch/f3-21-report.md`.

## State

Delivered and merged 2026-10-15 (main `ac879443`); QEMU evidence pending on the rebuilt image.
