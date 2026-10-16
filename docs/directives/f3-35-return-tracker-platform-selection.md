# f3-35 — return tracker: TF stepping under QEMU TCG, sparse execute breakpoints on hardware, checkpoint evidence

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh (after the f3-34 review fixes are merged). **Worktree:** `wt/f3-35` from main (after f3-34 fixes).
- **Authority:** `src/kernel/vm/dpmi_dispatch.c`, `src/kernel/vm/dpmi_return.inc`, `src/kernel/vm/dpmi.c` (tracker selection only), `src/kernel/vm/dosvm_perf.inc` (counters), `docs/design/dos-dpmi-contract.md` (amend), host tests, `docs/validation/` f3-34 record.

## Evidence

- QEMU 11.1.1 TCG (icount) dies with SIGSEGV as soon as a DPMI client runs on an image with f3-34's sparse execute breakpoints: `coredumpctl` shows three `qemu-system-i386` core dumps (2026-10-16 10:14, 10:20, 10:26 CEST) matching `vm-fullscreen-qemu-t23`, `dos4gw-qemu-t23` and `dpmi-qemu-t23` on image 27b35378; `result.json` records `cleanup.wrapper_returncode=-11` 13.6 s after start; the runner then reports the masked reason "Connection reset by peer". The same cases pass on 64a531f9 (f3-10 TF tracker).
- The f3-34 review (`build/lead-scratch/review-f3-34.md`, item 5): TCG compares debug breakpoints against EIP rather than the linear address and lacks the SS-inhibition check, so segmented PM/V86 code never raises the expected #DB under QEMU; the QEMU gates cannot exercise the breakpoint path at all.

## Required

1. **Platform selection.** Under `CBI_F_SMBIOS_QEMU` the return tracker uses the TF-stepping acquisition (the f3-10 behaviour as kept by the f3-34 fallback paths); on hardware it uses the sparse execute breakpoints. One switch, decided once at VM creation, recorded in the VM's startup record (`tracker=tf|breakpoint`). No DR7 execute breakpoint is ever programmed under QEMU.
2. **Evidence on hardware.** Add to the five-second `vm_perf_*` family a `vm_perf_tracker` row: checkpoints armed, hit, expired, fallback steps, proof reacquisitions; the T23 log must prove the breakpoint path runs and hits (the review's item 5). The host runner's TF/breakpoint causes stay distinguishable.
3. **Runner.** `scripts/test/run.py`: when QEMU exits before the runner's own quit, report the QEMU exit status (signal or code) as the reason instead of the QMP socket error.
4. Host coverage: both selections through the production code on the DOOM sound/menu replay (TF: unchanged counts from f3-10; breakpoint: the f3-34 counts), `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`, `python3 scripts/build_dos_image.py`. Report in `build/codex/f3-35-report.md`. The lead runs dpmi, dos4gw, quake, vm-fullscreen on QEMU (must pass or show the known timing reds only) and the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
