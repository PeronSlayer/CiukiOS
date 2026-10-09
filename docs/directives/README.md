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

Planned next: ATA PIO and MBR with registry lifecycle; native i8042 input
queue; framebuffer device, presenter and boot log; serialized V86 BIOS
service (E500 firmware-first input).
