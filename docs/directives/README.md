# Directives

A directive is the written task Claude (lead) gives Codex (implementer) for
one bounded piece of work, per the owner's rule of 2026-10-10: Claude
directs, reviews, integrates and runs the QEMU and hardware evidence; Codex
writes the code. Each directive names its contract, the files it may touch,
the interfaces it must respect, the host tests it must deliver, the model
and effort chosen for it, and how it is accepted. Codex works in its own
worktree and never commits; the lead reviews the diff, runs the QEMU
evidence and merges.

| Directive | Step | Model / effort | State |
| --- | --- | --- | --- |
| [f0-01-review-fixes](f0-01-review-fixes.md) | F0 | gpt-6.1-sol / high | delivered 2026-10-10; host tests 36/36; QEMU suites by the lead |
| [f1-00-kernel-services](f1-00-kernel-services.md) | F1 | gpt-6-astra / xhigh | delivered 2026-10-10; reviewed; waits for the F0 hardware close (task.c exit hook and `timing_calibrate` at init are lead glue at integration) |
| [f1-01-fat-vfs](f1-01-fat-vfs.md) | F1 | gpt-6-astra / xhigh | in progress |
| [f1-02-runner-and-selector](f1-02-runner-and-selector.md) | F1 | gpt-6.1-sol / high | delivered 2026-10-10; host tests 44/44; to rebase on f0-01 and move the F1 dispatch out of `parse_selector` before integration |
| [f1-03-selector-dispatch-and-rebase](f1-03-selector-dispatch-and-rebase.md) | F1 | gpt-6.1-sol / high | written; starts when f0-01 is on main |
| [f1-04-i8042-input](f1-04-i8042-input.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-10 |
| [f1-05-framebuffer-device](f1-05-framebuffer-device.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-10 |
| [f2-00-posix-contract](f2-00-posix-contract.md) | F2 | gpt-6-astra / xhigh | contract approved and merged 2026-10-10 |
| [f1-06-ata-pio](f1-06-ata-pio.md) | F1 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-10 |
| [f1-07-bios-vm-firmware-input](f1-07-bios-vm-firmware-input.md) | F1 | gpt-6-astra / xhigh | issued 2026-10-10 |
| [f1-08-runtime-integration](f1-08-runtime-integration.md) | F1 | gpt-6.1-sol / xhigh | written; starts after f1-03 and f1-07 |
| [f1-09-storage-bringup](f1-09-storage-bringup.md) | F1 | gpt-6-astra / xhigh | written; starts after f1-03, f1-06 and f1-08 |
| [f2-01-abi-header](f2-01-abi-header.md) | F2 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (687 layout checks) |
| [f2-02-processes-and-memory](f2-02-processes-and-memory.md) | F2 | gpt-6-astra / xhigh | issued 2026-10-11 |
| [f2-06-sdk-newlib](f2-06-sdk-newlib.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (clean build 17 s; guest not_run) |
| [f2-03-files-and-paths](f2-03-files-and-paths.md) | F2 | gpt-6-astra / xhigh | written; starts after f2-02 and f1-09 |
| [f2-09-runner-f2-suites](f2-09-runner-f2-suites.md) | F2 | gpt-6.1-sol / high | written; starts after f1-03 |
| [f2-07-lua-port-and-payloads](f2-07-lua-port-and-payloads.md) | F2 | gpt-6.1-sol / high | issued 2026-10-11 |

Planned next (F2, after f2-02): f2-04 signals and faults, f2-05 desktop
objects and the bootstrap supervisor, f2-08 ring-3 desktop.
