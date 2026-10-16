# f3-34 — DPMI return tracker without single steps (contract review of f3-10, owner authorization 2026-10-16)

- **Phase:** F3. **Model:** gpt-6-astra / xhigh (owner authorization: architecture/contract review). **Worktree:** `wt/f3-34` from main `0dcf11a3`.
- **Authority:** `src/kernel/vm/dpmi_dispatch.c` (return tracker, proofs, decode cache), `src/kernel/vm/dpmi*.c`, `src/kernel/arch/trap.c` (vector 1 path for VM clients only), `docs/design/dos-dpmi-contract.md`, the f3-10 validation records under `docs/validation/`, host tests (`tests/host/dosvm_dpmi_test.inc`, `dosvm_guest`). Not yours: `dosvm_bios.c`, `dosvm_io_burst.inc`, `v86_emulate*.c`, PIT/PIC port paths (directive f3-33 runs in parallel on them).

## Evidence (T23, Pentium III-M 1.13 GHz, image e8dfd0cd, `legacy/local/physical/e8dfd0cd/serial.log` and `BOOT.LOG`, read-only)

Quake (DPMI client, chain-4 window), five-second intervals: VM task 84–87 % of wall time, guest code 21–38 %, monitor 2.3–3.3 s. `vm_perf_cost`: `db` 2.0–2.2 s per interval, 420–455 k exits (about 90 k/s, 4.9 us each) — the DPMI return tracker's owned single steps (f3-10: TF-driven steps across near/far returns with a 512-step ceiling, 32-byte stack proofs and descriptor snapshots, f3-31 already acquires the proof only at the tracing boundary). This is 40 % of the machine. The second cost class (ports, INT reflection) is handled by f3-33.

## Required

1. **Contract review first.** State precisely which guarantees the return tracker provides today (from `dos-dpmi-contract.md` and the f3-10 records): what a hostile or buggy DPMI client could do without it (stack switch to kernel-visible memory? return into a different privilege/selector? IRQ delivery on an inconsistent frame?), which of those are already enforced by the hardware ring-3 isolation of the VM, by the LDT/GDT limits and by the IOPL/TSS bitmap, and which remain. Write this as an amendment proposal to the contract before any code.
2. **Redesign without per-instruction single steps.** Candidates to evaluate against the guarantees, with the Intel SDM (debug registers, TF semantics, IRET/RETF frame checks) as the reference: a hardware breakpoint (DR0–DR3, execute) on the return target captured at the boundary instead of stepping to it; proof validation only on the events that can violate the guarantee (stack segment or SS:ESP change, IRQ delivery, nested context entry, mode switch) instead of every instruction; lazy acquisition of the forward proof. The chosen design must keep: client TF/debug-register ownership (DPMI debuggers), nested contexts, exact instruction boundaries for IRQ delivery, the existing fault classes, and every behaviour the f3-10 fixtures prove; where a fixture only proves an implementation detail of stepping, say so and replace it.
3. **Implement** the design in the worktree; host replays before/after: the real DOOM timedemo (`tests/host/dosvm_guest`, `GUEST_PERF=1`) and the Quake startup replay, reporting DB exits per second and monitor time per interval; the full DPMI/VM host suites; `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`, `python3 scripts/build_dos_image.py`. Target: DB exits below 2 k/s in gameplay with the guarantees kept; no new physical grants.
4. Report in `build/codex/f3-34-report.md` (guarantees table, design decision with SDM references, before/after numbers, verbatim results). The lead runs f3-vm (dpmi, vm-video), dos4gw, doom and quake on QEMU, then the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`, host memory checked before each suite.
