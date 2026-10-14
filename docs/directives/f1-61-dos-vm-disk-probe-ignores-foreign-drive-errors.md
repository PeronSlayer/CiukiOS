# f1-61 — DOOM and Quake show "I/O error" on the T23 without a launch record: the VM disk probe over drive letters aborts on an unrelated drive's error

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f1-61` from `main`. Host-testable; the lead runs `vm-window` cases on QEMU and the T23.

## Evidence (T23, image ce2e6ce9, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/ce2e6ce9/disk-log/BOOT.LOG.clean`)

The owner started DOOM and Quake from Programs > Games: both windows showed "I/O error". The log has `[launch]` records for `files` (three) and `control` (two) only: no `[launch-stage] name=dosbox`, no DOS VM record. At that time the USB stick's volume had been published as E: with `error=-5` (see f1-60) and the CD was D:. `src/kernel/vm/dosvm_disk.c:40-60` probes every drive letter for an owner fixture `X:\CIUKIGAM.IMG` before falling back to the bundled `C:\GAMES\CIUKIGAM.IMG`; an I/O error from a letter that is not the bundled base is treated as fatal instead of "absent". On 445e91c7 (no stick) DOOM launched. On QEMU `vm-window-qemu-desktop-2002` passes.

## Task

1. The fixture probe treats any error other than a readable fixture as absent (ENOENT, EIO, EBUSY, quarantined volume): only the bundled base's errors fail the launch, with a record `[dosvm] disk base=<path> error=<n> reason=<probe|bundled>` naming the path.
2. The desktop's game launch records `[launch-stage]` before the VM is created and a `[launch] ... error=` record when the VM creation fails, so the next log explains an "I/O error" window.
3. Host tests: a drive table with a failing letter before the bundled base; the bundled base missing; the base readable.

## Acceptance

Kernel host suite (dosvm disk tests) and desktop launch tests green; the lead runs `vm-window-qemu-desktop-2002` and `vm-starve-qemu-t23` on QEMU, then DOOM/Quake on the T23 with the stick inserted. Files: `src/kernel/vm/dosvm_disk.c`, desktop launch records, tests. Do not touch `usb_storage.c` (f1-60).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first where named (`--search`), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f1-61-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
