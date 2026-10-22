# f6-05 — NDIS host step 2: foreign ring-0 fault containment, then the native ntoskrnl/HAL shim for the pinned profiles

- **Phase/model:** F6, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f6-05` from `main` (which includes f6-04).
- **Contracts:** `docs/design/ndis-miniport-host-contract.md` (ND5–ND8, ND12, ND13 f6-05 row) and `docs/design/ndis-loader-boundary.md` (f6-04). Read both completely.
- **Lead decisions on the f6-04 report (2026-10-22 23:45):**
  1. The execution gate opens only after part 1 below exists with host evidence.
  2. Ordinal imports stay refused unless a pinned profile needs one. In that case, pin the mapping from the exact binary's import table and cite it.
  3. The symbol set is the complete import set of the three pinned profiles, not the 126-symbol family union:
     - QEMU `e1000` with `e1000325.sys`;
     - QEMU `pcnet` with `atnicm51.sys`;
     - the T23 PRO/100 with `e100b325.sys`.

## Part 1: containment (before any shim)

1. **Faults.** A CPU exception whose EIP lies in a registered foreign image, or in a shim called from one (a foreign frame is active on the callback record of f6-04), does not reach the fatal path. The kernel then:
   - records `[ndis] fault driver=… vector=… eip=+offset`;
   - fences the device: Bus Master and I/O/memory decode off, INTx disabled, IRQ handler detached;
   - releases every lock and IRQL state the shim tracked for that driver;
   - marks the driver quarantined persistently for the boot;
   - resumes at the recovery point of the outermost host-to-driver thunk with an error. This is a saved context on the same kernel stack, the role exception tables play in Linux.

   Other drivers, the network stack and the desktop continue.
2. **Nonreturning callbacks.** Each callback kind has a budget (ISR, DPC, passive; values from ND5–ND6). The timer interrupt detects an expired foreign callback when the interrupted EIP is foreign and abandons it through the same recovery path. A callback interrupted inside a shim is abandoned when it returns to foreign code.
3. **Static admission scan.** Before entry, disassemble the executable sections (length decoder over the i686 baseline). Refuse an image that contains `cli`, `sti`, `hlt`, `lgdt`/`lidt`/`ltr`, control-register or MSR writes, or `int` other than `int3`, with a record of the reason and offset. Port I/O is allowed, because drivers access their own BAR, and is checked against the claimed BAR ranges in the shim's port helpers. A CLI spin is the one case recovery cannot cover, so it is refused at admission.
4. **Host tests.** Synthetic foreign images exercise:
   - page fault, divide error and invalid opcode inside the image and inside a shim called from it;
   - a nested callback;
   - a timeout inside an ISR, a DPC and a passive callback;
   - recovery with spin locks held;
   - quarantine persistence;
   - the scan refusing each listed instruction while accepting `in`/`out`.

   An i386 execution harness runs them where possible.

## Part 2: shim

Implement with real semantics the NT/HAL exports of the three pinned profiles: IRQL, DPCs, spin locks, waits and time, pool, MDLs, registry parameters, PCI configuration and DMA ownership through the F6 ledger, as ND7–ND8 require.

To get the exact import lists, run `scripts/pe_import_census.py` read-only on the owner's pack under `third_party/` (git-ignored). The pack never becomes a test input: tests use synthetic fixtures. Exports outside those three sets stay unimplemented and refuse the load.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (Microsoft DDK pages, Intel SDM for the instruction scan). Host tests one at a time with `nice -n 19`. Write `build/codex/f6-05-report.md`.

## Amendment 1 (lead, 2026-10-23 00:13): user-mode driver host replaces ring-0 containment

The first run showed that ring-0 containment cannot be completed on i686. The contract now has **ND14 and ND14.1**: each miniport runs in a ring-3 driver-host process. This directive changes accordingly, and the first run's work (execution context, PCI fence, recovery code, fixtures) is kept where it still applies.
- **Part 1, new.** Build the ND14/ND14.1 host infrastructure:
  - per-task I/O permission bitmaps installed on every switch;
  - typed, revocable uncached user mappings of owned BARs and ledger DMA memory;
  - CLI/STI/HLT trap emulation and the reviewed flag-save mechanism;
  - the teardown order of fence first, then kill;
  - interrupt delivery with exclusive or per-function-masked lines and generation-tagged pending delivery;
  - kernel-owned deadlines;
  - the loader's user-host policy;
  - the validated shared packet ring.

  Each piece gets host tests, plus an executable i386 fixture host process wherever the sandbox allows.
- **Part 2.** The NT/HAL shim for the three pinned profiles now runs inside the host process.
- **Unchanged.** The admission scan of the first run is no longer required: keep it as a diagnostic only.
