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
| [f1-03-selector-dispatch-and-rebase](f1-03-selector-dispatch-and-rebase.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (54 runner tests) |
| [f1-04-i8042-input](f1-04-i8042-input.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-10 |
| [f1-05-framebuffer-device](f1-05-framebuffer-device.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-10 |
| [f2-00-posix-contract](f2-00-posix-contract.md) | F2 | gpt-6-astra / xhigh | contract approved and merged 2026-10-10 |
| [f1-06-ata-pio](f1-06-ata-pio.md) | F1 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-10 |
| [f1-07-bios-vm-firmware-input](f1-07-bios-vm-firmware-input.md) | F1 | gpt-6-astra / xhigh | partial delivery 2026-10-11 (65,900 host checks); completed by f1-07b |
| [f1-08-runtime-integration](f1-08-runtime-integration.md) | F1 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (drivers start at boot; F1 QEMU suites pending f1-10) |
| [f1-09-storage-bringup](f1-09-storage-bringup.md) | F1 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 (39,944 storage checks) |
| [f2-01-abi-header](f2-01-abi-header.md) | F2 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (687 layout checks) |
| [f2-02-processes-and-memory](f2-02-processes-and-memory.md) | F2 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 (94,456 host checks; QEMU probes pending f2-09) |
| [f2-06-sdk-newlib](f2-06-sdk-newlib.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (clean build 17 s; guest not_run) |
| [f2-03-files-and-paths](f2-03-files-and-paths.md) | F2 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 with its integration patch (14,837 file checks) |
| [f2-09-runner-f2-suites](f2-09-runner-f2-suites.md) | F2 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (74 runner tests; 33 F2 cases) |
| [f2-07-lua-port-and-payloads](f2-07-lua-port-and-payloads.md) | F2 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (50 payloads; host-reference suite OK) |
| [f2-04-signals-and-faults](f2-04-signals-and-faults.md) | F2 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 (4,344 signal checks; QEMU pending f2-09) |
| [f2-05-desktop-objects-and-supervisor](f2-05-desktop-objects-and-supervisor.md) | F2 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 with its integration patch (35,965 checks) |
| [f1-07b-bios-vm-followup](f1-07b-bios-vm-followup.md) | F1 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 (71,160 V86 checks) |
| [f2-08-desktop-process](f2-08-desktop-process.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (portrait conversion checked visually by the lead; guest run pending) |
| [f1-10-suite-alignment-and-loader-safe](f1-10-suite-alignment-and-loader-safe.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (82 runner tests) |
| [f1-11-activation-records](f1-11-activation-records.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11; F0 56/56 on image acd8af37 |
| [f1-12-integration-followups](f1-12-integration-followups.md) | F1/F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (probe order per contract; 82 runner tests) |
| [f1-13-boot-time-records](f1-13-boot-time-records.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (runtime guard patch applied; supervisor framing allowed) |
| [f1-14-input-fault-guest-fix](f1-14-input-fault-guest-fix.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (root cause: 10 KiB kmalloc over the 2,040-byte limit) |
| [f1-15-firmware-input-qemu](f1-15-firmware-input-qemu.md) | F1 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 (root cause: SeaBIOS PM timer INL 0x608 refused by the port policy) |
| [f1-16-fixture-disks](f1-16-fixture-disks.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (43,674 storage checks) |
| [f1-17-fat-suite-alignment](f1-17-fat-suite-alignment.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (93 runner tests) |
| [f1-18-firmware-queue-starvation](f1-18-firmware-queue-starvation.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 |
| [f2-10-probe-completion](f2-10-probe-completion.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (uname; libc-smoke failures were EROFS before the write gate and ENOSYS) |
| [f1-19-firmware-input-delivery](f1-19-firmware-input-delivery.md) | F1 | gpt-6-astra / xhigh | delivered and merged 2026-10-11 (retained virtual-PIC IRQs); last Astra task before the Sol-first rule |
| [f1-20-generic-pc-baseline](f1-20-generic-pc-baseline.md) | F1 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (baseline doc, two desktop profiles, loader checks; 98 runner tests) |
| [f2-11-storage-shutdown-namespace](f2-11-storage-shutdown-namespace.md) | F2 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (open descriptions preflighted under the namespace lock; cwd pins no longer block the detach; namespace-linked storage host test) |
| [f2-12-crash-isolation-desktop](f2-12-crash-isolation-desktop.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (native desktop controller through call-3 reports and a control page, loader `server=` key, five desktop + six stand-in cases); QEMU evidence by the lead pending |
| [f1-21-framebuffer-probe-fixtures](f1-21-framebuffer-probe-fixtures.md) | F1 | gpt-6-luna / medium | delivered and merged 2026-10-11 (static fixture buffers; fixture-size guard caught the 2,247-byte request) |
| [f2-13-f2-suite-alignment](f2-13-f2-suite-alignment.md) | F2 | gpt-6.1-sol / high | delivered and merged 2026-10-11 (predicates rewritten from the real records; evidence records added to fd-table, spawn-wait, mmap, threads-wait, libc-smoke); QEMU evidence by the lead pending |
| [f2-14-signals-fault-regression](f2-14-signals-fault-regression.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 in two Sol deliveries (TCG omits #AC: payload falls back by CPUID signature, hardware still requires vector 17; spinning peers delayed the sleeper past its deadline, peers now yield); `f2:signals-fault` PASS on `qemu-t23` image `c581dfe8…` |
| [f2-15-app-gate-probe](f2-15-app-gate-probe.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (probe registered last in the F2 order through the build list, provenance sidecar, supplement path reconciled); QEMU evidence by the lead pending |

| [f1-22-mount-crash-fixtures](f1-22-mount-crash-fixtures.md) | F1 | gpt-6.1-sol / xhigh | in progress 2026-10-11 (runner attaches no fixture for the `mount-*` injection declarations; crash cut never implemented) |
| [f2-17-app-gate-double-fault](f2-17-app-gate-double-fault.md) | F2 | gpt-6.1-sol / xhigh | delivered and merged 2026-10-11 (frames 2,180/2,348/1,280 → 668/276/256 bytes; `-Wframe-larger-than` 4 KiB kernel-wide, 1 KiB for the probe and supervisor; guarded 8 KiB stack host regression); QEMU evidence pending |
| [f1-23-firmware-overrun-records](f1-23-firmware-overrun-records.md) | F1 | gpt-6.1-sol / high | in progress 2026-10-11 (`firmware_overrun`/`disallowed_io` declared not_run: `biosvm_selftest` has no record emitter; gates the desktop-profile cases) |
| [f2-18-app-gate-upstream-run](f2-18-app-gate-upstream-run.md) | F2 | gpt-6.1-sol / xhigh | in progress 2026-10-11 (upstream Lua run reaches `files.lua`, fails `os.getenv"PATH"`: gate env lacks PATH; audit of file I/O coverage) |
All F1 and F2 directives are written; launches follow their prerequisites
(f1-10 when QEMU is free; f2-03 after f1-09; f2-08 after f2-05).