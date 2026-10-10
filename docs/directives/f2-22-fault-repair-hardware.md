# Directive f2-22: `signals-fault` `fault-repair` on the ThinkPad T23 delivers 5 of 7 faults

- **Step:** F2. **Contracts:** `posix-subset.md` (signal delivery for
  processor faults: `#DE`, `#UD`, `#PF`, `#GP`, `#AC`, `#MF` → `SIGFPE`,
  `SIGILL`, `SIGSEGV`, `SIGBUS`), `f2-acceptance.md` (`signals-fault`
  row), directive f2-14 (TCG `#AC` fallback by CPUID signature; hardware
  requires vector 17), Intel SDM Vol. 3A (CR0.AM/EFLAGS.AC for `#AC`;
  CR0.NE and the FERR#/IRQ13 legacy path for x87 errors when NE=0;
  CR0.MP/TS and `#NM`).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-faults-hw`, from `main` at or after `abcd677`.
  Files: `src/kernel/arch/*` and `src/kernel/core/cpu.c` (control
  register setup), `src/kernel/proc/signal.c` (fault mapping),
  `src/kernel/probes/f2_probes_signals.c`, `tests/host/proc/signal_payload.asm`,
  `signal_test.c`, `tests/host/test_firmware_records.py` or the sibling
  predicate tests, `docs/validation/2026-10-09-f0/README.md` if a
  hardware/TCG difference is documented.

## Observed on the ThinkPad T23 (image `90477716…`, sweep `66666666`, 2026-10-11)

```
probe=signals-fault … case=fault-repair part=alignment tcg_fallback=0 hardware_required_vector=17
probe=signals-fault … case=fault-repair part=status expected=0 observed=0 raw_vector=17 corruption=0
probe=signals-fault … case=fault-repair part=handlers entries=5 returns=4 max_depth=1 order=0
probe=signals-fault event=END status=FAIL reason=signal_contract
```

The payload exits 0 with no corruption and the real `#AC` (vector 17)
fires on hardware, but only 5 handler entries and 4 returns are counted
instead of 7/7, so two of the seven faults in the sequence (`#PF` read,
`#PF` write, `#UD`, `#DE`, `#GP`, `#AC`, `#MF`) were not delivered as
signals on the Tualatin, or one handler did not return. On QEMU (TCG) the
case passes with the fallback. The capture `build/t23/sweep-66666666.log`
in the worktree holds the full `fault-repair` record set with the
`observed`/`expected` index records that identify which indices are
missing.

## What to do

1. From the records, identify the missing indices. Check the control
   register setup on real hardware against the SDM: `CR0.NE` (with NE=0 a
   real x87 error is reported through FERR#/IRQ13, never as `#MF` vector
   16), `CR0.AM` and `EFLAGS.AC` (for `#AC` the payload sets AC; AM must
   be set by the kernel), `CR0.MP`/`EM`/`TS` (an unexpected `#NM`), and
   the `#DE` path. Fix the kernel so every fault in the sequence is
   delivered as the contract's signal on hardware; keep the TCG fallback.
2. If a fault depends on a feature the baseline cannot guarantee,
   declare it in the contract with the CPUID feature bit checked at run
   time and a `not_run` record for that index, never a silent skip.
3. Host tests: the control-register setup (asserted values at kernel
   init), the record parsing for a partial delivery, the payload under
   the host harness unchanged.

## Acceptance by the lead

Host tests; kernel build; QEMU `signals-fault` on three profiles
unchanged; on the T23 sweep `fault-repair` reports `entries=7 returns=7`
and the case passes.
