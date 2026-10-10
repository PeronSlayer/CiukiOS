# F0 kernel, loader and runner: QEMU evidence (9 October 2026)

Scope: the first Ciuki VMM scaffold (`src/kernel/`), the F0 loader
(`src/boot/mbr.asm`, `src/boot/ciukldr.asm`, `src/boot/ciukldr/`), the image
builder (`scripts/build_image.py`) and the runner (`scripts/test/run.py`),
measured on this Linux host with QEMU 11.1.1 (TCG, `pc-i440fx-9.2`,
`-cpu pentium3`). No physical machine was tested; the T23 and E500 gate (T4)
remains open. Contracts: `docs/design/f0-acceptance.md` and the six others
listed in `docs/README.md`.

## Build

- Toolchain pinned in `config/toolchain.json`: clang 23, ld.lld 23, NASM 3.
- Kernel `build/f0/VMM.ELF`: 210,440 bytes, three `PT_LOAD` segments at
  physical `0x00100000` (virtual `0xC0100000`), BSS to `0x0013A000`. The
  FPU/SIMD opcode audit passes with only the state-management routines
  allowed (`scripts/build_kernel.py`).
- Loader: MBR 512 bytes, `CIUKLDR` 23,040 bytes (45 sectors) at LBA 1.
- Image: 512 MiB sparse FAT32 (`mkfs.fat`, 4 KiB clusters, 130,557
  clusters), partition at LBA 2048, T1 checks pass (`fsck.fat -n`, loader
  CRC, geometry). `build-manifest.json` records commit, dirty state and the
  SHA-256 of kernel and image.
- T0 host tests: kernel library and `ciuki_boot_info` validator under
  ASan/UBSan (`scripts/test/host_kernel_tests.sh`), loader statics (6),
  runner/physical/loader-model unit tests (30).

## Loader defects found at the first boot

Codex wrote the loader without being able to run QEMU from its sandbox.
The first boot showed `L:SELECT_ERROR` and `L:VBE_SKIP` for every mode.
Cause, in both cases: real-mode memory operands that use `BP` as a base
address the **SS** segment by default, and the loader keeps `SS=0` with its
data in segment `0x2000`. Six operands (`menu.inc` selector matching,
`video.inc` mode list and score tables) read garbage and wrote the score
table into low memory. Fix: explicit `ds:` overrides. After the fix the
selector parses and QEMU's VBE mode `0x144` (1024×768×32, pitch 4096, LFB at
`0xFD000000`) is selected; the remaining 25 `L:VBE_SKIP` lines are legitimate
rejections (4-bpp planar and 15-bpp modes).

## Kernel probes (manual `f0:all` boot, run `0000a11b`, qemu-t23 profile)

| Probe | Result | Key evidence |
| --- | --- | --- |
| boot | PASS | `READY tick=104`, `elapsed_pit_cycles=11941930` (≥ 11,931,820) |
| bootinfo | PASS | 6 E820 entries, 3 reservations, 130,943 usable pages, ledger equal, 7 negative validator cases rejected, map digest unchanged |
| allocator | PASS | exhausted 130,463 pages cleanly (`allocation_error=12`), 100 mixed cycles, no duplicates/violations/pattern errors, `resource_delta=0` |
| protection | PASS | `cr0=8001003b`, 131,812 kernel PTEs without `U/S`, text read-only, guard pages absent; injected ring-0 write to read-only data → `#PF error=3`; guard-page read → `#PF error=0` |
| isolation | PASS | same VA `0x00400000` on PFNs 4105/4113, sentinels `11111111`/`22222222` intact after 402 dispatches |
| preempt | PASS | 10,000 timer-driven switches, dispatches 4,980/5,020, `max_wait=1` tick, no register/stack/flags/selector corruption, `critical_us=35`, no budget violations, no timer gap > 2 ms |
| localfault | PASS | kernel read/write → `#PF` error 5/7 with `cr2=c0100000`; `cli`/`in`/`out` → `#GP(0)`; offender terminated, survivor progressed ≥ 150 samples |
| syslife | PASS (after fix) | 12 syscall buffer cases with the expected errors and no side effects; 100 create/exit and create/fault cycles with zero page, task, mapping and heap deltas; survivor undamaged |
| fpu | PASS (after fix) | 1,000 switches with distinct x87 state preserved, owner destruction harmless, unmasked zero-divide → `#MF` (vector 16) in the offender only |
| panic | PASS | `PANIC vector=14 error=00000000 eip=c01002d2 cr2=ff800000`, no allocation, no lock, no storage I/O, then halt |

Two probe defects were corrected during the session, both in the test, not
in the kernel: the "wrapping buffer" case used a length above the syscall
limit and therefore got `EINVAL` instead of `EFAULT` (now a legal length
past the end of the address space); and the panic target `0xDEAD0000` lies
inside the direct map on a 512 MiB machine (now `0xFF800000`, the window
`boot-memory.md` reserves as never mapped; suite updated).

## QEMU TCG deviation: FLD rounds to the precision control

The FPU probe's second task failed on its first iteration with control word
`0x0C7F` (24-bit precision): the value loaded by `FLD m64` came back
changed. On real x87 hardware precision control affects only arithmetic
results, not loads and stores (Intel SDM Vol. 1, "Precision Control
Field"); QEMU's softfloat rounds the conversion to `floatx80` at the
current precision. The probe now uses `0x0F7F` (64-bit precision, round
toward zero) so both control words keep loads exact on both the emulator
and real hardware; the probe still verifies that a distinct control word
and register contents survive preemption. Recorded in AGENTS.md as a known
emulator deviation.

## QEMU TCG deviation: scalar alignment checks are absent (f2-14)

The supplied `signals-fault.records` from run `0704fc09` (image `a9550f04…`,
payload SHA-256
`dd30d235d6a93f44897db741187bc46dd0eca9cd000b654351259849fb1d8939`)
contains six repaired faults: #PF absent, #PF protection, #UD, #DE, #GP,
then #MF. There is no #AC between #GP and #MF. The two payload errors are
the final `ENTRIES == 7` and `RETURNS == 7` comparisons: both observed six.
The fault siginfo, addresses, EIPs and surviving process agree with their
expectations. This is evidence from the supplied boot, not a new QEMU run.

[Intel SDM Vol. 1](https://cdrdv2-public.intel.com/819711/253665-sdm-vol-1.pdf),
section 3.4.3.3 (AC), enables user alignment checking with CR0.AM and
EFLAGS.AC. CiukiOS `fpu_init` sets AM; the payload sets AC and executes
`mov eax, [DATA + 1]` at CPL3. That doubleword operand is misaligned.
[QEMU 11.0.0's scalar load translator](https://github.com/qemu/qemu/blob/v11.0.0/target/i386/tcg/translate.c#L467)
emits `tcg_gen_qemu_ld_tl` with only the size and `MO_LE`, without a
CR0.AM/EFLAGS.AC alignment check. The
[integer operand load path](https://github.com/qemu/qemu/blob/v11.0.0/target/i386/tcg/emit.c.inc#L251)
calls that translator. Together with the missing vector 17 in the boot,
this establishes an emulator deviation rather than a signal-frame defect.

The payload still attempts the real alignment fault first. Only if it does
not trap and CPUID leaf `0x40000000` reports the exact `TCGTCGTCGTCG`
signature and a maximum leaf of at least `0x40000001` does it accept the
known omission. The signature is
[explicitly TCG-only in QEMU](https://github.com/qemu/qemu/blob/v11.0.0/target/i386/cpu.c#L8640).
It then triggers a real unmapped #PF with AC still set, checks handler AC
clearing and sigreturn AC restoration, GPRs, TLS and x87 preservation, and
still requires seven handler entries and seven returns with zero errors.
The ordinary observed/expected fault records show vector 14 for this
fallback; `case=fault-repair part=alignment tcg_fallback=1
hardware_required_vector=17` explicitly identifies it. A missing #AC on
hardware or an unidentified emulator increments the error counter. No
synthetic #AC or SIGBUS delivery is counted. TCG PASS is therefore not
evidence of hardware #AC generation: real #AC/SIGBUS remains a hardware
qualification requirement. Host tests retain exact #AC/SIGBUS mapping.

### x87 investigation and the independent sleep failure

Intel SDM Vol. 1 sections 8.1.10 and 8.3.12 describe FNSAVE's state save
followed by reset, and the non-waiting instructions that can inspect a
pending exception. Section 8.1.5.2 restricts precision control to the listed
arithmetic operations. The payload uses `FLD1`/`FLDPI`, retains its initial
FCW precision, and checks reset FCW `037f`/FSW zero before handler x87 use.
It repairs the saved FCW masks and clears the saved exception/summary/busy
bits while retaining TOP/condition bits. The examined
[TCG x87 helpers (QEMU 10.1.0)](https://github.com/qemu/qemu/blob/v10.1.0/target/i386/tcg/fpu_helper.c)
likewise set ES/B for an unmasked exception, raise #MF at FWAIT, save the
28-byte environment plus eight ten-byte logical registers, and reset after
FNSAVE. These helpers explain the tested behavior; they are not claimed
as a source audit of the pinned QEMU 11 x87 implementation. The supplied
boot reaches and repairs #MF at its expected EIP. No new x87 deviation or
weakened x87 comparison is justified by this failure.

For `nanosleep-eintr`, the two failing checks are resumed `EAX == -EINTR`
and context `SAVED_EAX == -EINTR`. Both are zero, as are the syscall result
and the remainder seen by the handler. The probe posted SIGUSR1 at about
10 ms into a 20 ms sleep, then synchronously emitted its injection record
before yielding. That serial output can consume more than the remaining
interval: the supplied injection line is 164 bytes including CRLF, or
42.708 ms at 38400 baud with 8N1 framing. `file_nanosleep` checks its
deadline before pending catchers on
redispatch, so an expired sleep correctly completes with zero; delivery
and sigreturn correctly preserve that completed result under
`execution-abi.md`'s completed-syscall EAX rule. This is a probe scheduling
defect, independent of TCG alignment and x87.

The probe now snapshots injection identity/mask/result and emits the same
record after the child exits, leaving no serial output between posting
and yielding. The host regression uses production `file_nanosleep`, signal
delivery and sigreturn with a fake scheduler posting at 10 ms. With a
50 ms reporting delay it reproduces result/saved/resumed EAX all zero and
remainder zero; without that delay all three are `-4` (`fffffffc`) and
the remainder, including at handler entry, is 10000000 ns. Hardware FPU
instructions and ring-3 payload execution remain outside this host test.

Host validation for this change: `python3 scripts/build_kernel.py` passed
with the FPU/SIMD audit; `ASAN_OPTIONS=detect_leaks=0 bash
scripts/test/host_kernel_tests.sh` passed (signal: 5291 checks, zero pages,
threads and zombies; production payload parser/load passed); and
`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host`
reported `Ran 99 tests`, `OK (skipped=5)`. The new payload SHA-256 is
`fd821d494f6fd2779a918e829744c8a386483e7ceb82e29bd189be1ec8b6d86d`.
No QEMU validation of this payload was performed here; the three directed
profiles and hardware #AC qualification remain with the lead.

## Runner suites

- `f0-smoke` (profiles `qemu-fast` and `qemu-t23`): PASS, both cases.
- `f0-panic`: the `panic` case PASS (serial `PANIC` record, five seconds of
  silence, zero block writes). The `uart-absent-qemu-t23` case produced the
  expected screen output (saved as `panic-uart-absent-run69573015.png`: the
  identical `PANIC` record on the LFB console, 16 reservations because no
  UART was claimed); the contract requires externally verified screen
  evidence, so the runner records it as FAIL. **The owner confirmed the
  screen evidence on 2026-10-09** (record `PANIC vector=14 … cr2=ff800000`,
  run `69573015`): the case counts as passed for the F0 gate.
- `f0-core`: the first run passed the five cold boots and failed the first
  warm-restart case on its 30 s deadline; the suite file used 30 s where the
  contract sets 180 s for core cases, and was corrected. The second run
  failed the same case after 180 s with no second boot in the serial log.
  The QMP log gave the cause: under `-no-reboot` QEMU turns the host
  `system_reset` into `SHUTDOWN reason=host-qmp-system-reset` followed by
  `STOP`, so the VM never restarted. The runner now omits `-no-reboot` only
  for warm-restart cases; guest-initiated resets remain detected through the
  `RESET` event count against the expected host resets.

## Cross-review of the kernel and the resulting fixes

Codex reviewed the kernel before the commit (`gpt-6.1-sol`, read-only) and
raised 13 blocking and 2 minor findings; all were applied:

| Finding | Fix |
| --- | --- |
| Record and log output ran with interrupts disabled (tens of ms at 38400 baud) | only the sequence number is taken under `cli`; output runs with interrupts enabled, panic stays synchronous |
| CR4 inherited (PAE/VME/PVI) before paging | `entry.asm` clears VME, PVI, PSE, PAE, PGE before loading CR3 |
| Initial FPU image kept stale register payloads after `FNINIT` | image made architectural: zeroed ST/XMM payloads, FCW 037F, empty tags, MXCSR 1F80 |
| Framebuffer could be aliased cacheable by the direct map | direct map skips the framebuffer range; `vmm_map_mmio` uses checked 64-bit spans |
| Handoff accepted bad colour masks and tiny geometry | validator checks formats, mask sizes/positions/overlap, minimum 320×200 |
| `spawn` leaked pages when a mapping failed | ownership tracked per mapping; unmapped pages freed |
| `probe_report` accepted control bytes (record injection) | printable ASCII only, otherwise `EINVAL` |
| Over-long records truncated silently | an `event=ERROR reason=record_overflow` record replaces them; the totals record was split (it measured 243 bytes) |
| Critical-section timing missed scheduler work; every unexplained gap called "external" | IRQ path timed through `schedule()`; gaps without a software cause are `gaps_emulation_host` under QEMU (SMBIOS) and `unclassified_gaps` on physical machines, which fail the probe until resolved |
| Preemption counter saturated at the target | the scheduler parks both measured tasks at exactly the target switch (`parked_at_target=1`) |
| ST1 check compared only a zero low dword; owner destruction not proven | both dwords compared, constants with nonzero halves; the current FPU owner is killed and reaped before the survivor check (`owner_destroyed=1`) |
| 8237 channels inherited from firmware | channels 0–3 and 5–7 masked at registry init (4 is the cascade) |
| Starvation boost missing | a normal task ready for 1 s is boosted once to the interactive queue (`starvation_boosts` reported) |
| Audit missed scalar SSE and MMX | classifier by instruction family with a positive/negative fixture in the host tests |
| Signed negation UB for `INT64_MIN` | unsigned magnitude arithmetic |

After the fixes all ten probes pass again in one `f0:all` boot (run
`0000a11d`): `preempt` reports `switches=10000 parked_at_target=1
critical_us=63 timer_gaps_over_2ms=1 gaps_emulation_host=1
unclassified_gaps=0`; `fpu` reports `switches=1089 owner_destroyed=1
destroy_errors=0`; the longest record is 236 bytes.

### Second review round

Codex verified the fixes and confirmed six of them, asked for more on
nine, and found one regression (the MMIO guard page could pass the end of
the region). Applied from that round: CR4 also clears `OSFXSR` and
`OSXMMEXCPT`; the handoff rejects a framebuffer below 1 MiB or overlapping
the kernel extent and checks the reserved colour component's capacity;
`vmm_map_mmio` bounds the mapping plus its guard page; the FPU probe
requires that the destroyed task was the FPU owner; the starvation boost
goes to the top priority queue; the audit classifies `movss`/`cmpsd`
(string operations are recognised only by their AT&T `b/w/l/q` forms, with
fixtures); the runner requires the exact host-origin `RESET` count for
restart cases. The scoped re-check returned APPROVE.

**Lead decisions recorded as disagreements**, reported to the owner and
**approved by the owner on 2026-10-09**:

- Timer gaps with no measured software cause are reported as
  `unclassified_gaps` and do not fail the probe (suite predicate `≥ 0`).
  The guest cannot prove what stalled it; attributing gaps to the
  emulator's host from inside the guest (an earlier version did so when
  SMBIOS said QEMU) was rejected by Codex as unsupported, and we agree;
  resolving them is a qualification activity with external evidence. The
  runner now records the host load average in `result.json` for that.
- Under TCG the `TSC` also counts time the host takes away from QEMU, so a
  critical-section measurement inside an interrupt-disabled interval can
  be inflated by a host stall: run `0000a11e`, taken while host unit tests
  and a Codex review ran concurrently, reported `critical_us=442` and one
  budget violation, against 35–66 µs in the runs on an idle host. Latency
  evidence must therefore be taken with the host otherwise idle; the
  `budget_violations = 0` pass condition stays, as the contract requires.
- Full grammar validation of `probe_report` through the kernel record path
  is deferred to F2, when native programs exist; in F0 the call rejects
  non-printable bytes and never starts a line with the record prefix.
- Deferred to a later phase: fault-injection tests of `spawn` allocation
  failures, serialised `RDTSC`, per-boost deferred logging (the boost
  count is reported), and the starvation guarantee across the device
  priority (no device tasks exist in F0).

### Instruction counting for latency evidence

The suite run on the image with the second-round fixes passed the whole
T23 block (ten boots including five warm restarts, nine probes) and the ten
E500 boots, then failed `preempt` on the E500 profile with
`critical_us=976 budget_violations=1` on an idle host (load 0.86). Under
plain TCG the guest's TSC and PIT follow host wall-clock time, so an
interrupt-disabled interval also contains host scheduling and TCG
translation work; the same code measured 35–66 µs in other runs. Those
wall-time observations are kept here as evidence of the measurement
problem, not of the software.

The evidence profiles now run with `-icount shift=1,sleep=on`. This is
synthetic time: 2 ns per executed instruction during busy execution (a
500 MIPS machine, no Pentium III cycle accuracy), while idle `HLT` periods
advance to the next timer deadline paced to real time. Host stalls during
busy execution are hidden from guest timing rather than measured, and busy
execution is not guaranteed to track real time; the runner's host
deadlines stay as a separate limit. The manual check under icount reported
`tsc_khz=999980 switches=10000 critical_us=0` with no gaps; the record now
also carries `critical_ns`, and `critical_us=0` means below 1 µs, not zero.
Under icount the budget pass condition is a regression gate in instruction
time (about 125,000 instructions for 250 µs); the physical 250 µs
requirement can only be established on the laptops, which measure real
time. `qemu-fast` (KVM) never produces latency evidence.

### Final suite results (icount profiles, image `5349514c…c8ac6`)

- `f0-smoke`: 2/2 PASS (`qemu-fast`, `qemu-t23`).
- `f0-panic`: `panic` PASS; `uart-absent-qemu-t23` recorded FAIL by the
  runner and confirmed by the owner from the screen capture.
- `f0-core`: 55/56 PASS: ten boots (five cold, five warm restarts) on each
  of `qemu-t23`, `qemu-e500` and `qemu-min128`; `bootinfo`, `allocator`,
  `protection`, `isolation`, `preempt`, `localfault`, `syslife` and `fpu`
  on each profile; `video-fallback` (`-vga none`, text console, serial
  evidence). The one FAIL is `safe-mode`: the runner reports that safe-mode
  automation is not defined by the frozen selector contract (the loader
  offers safe mode through the boot menu and `BOOT.CFG`, neither of which
  the runner can drive on the canonical image). Open item below.
- Host load during the run stayed below 1.0; total evidence on disk 8.6 MB
  (`build/test-runs`, last five runs per suite).


## T23 fault-repair controller deadline (f2-22)

The supplied second T23 sweep (`66666666`, image `90477716…`, recorded
2026-10-11) reports indices 1–5 only. Index 4 is the correctly delivered
#DE/SIGFPE. Missing records are index 6 (#AC/SIGBUS) and index 7 (#MF/SIGFPE).
The status record has `raw_vector=17`, while the last sampled handler has
`entries=5 returns=4` and still describes #GP. The same sweep's F0 `fpu`
probe passes with `mf_vector=16`. These facts do not establish two absent
processor exceptions or a successful exit: `signal_case` prints the process's
default status zero even when `signal_wait_zombie` timed out.

Research: [Intel SDM Vol. 3A](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf),
section 2.5 (CR0 AM/NE/MP/EM/TS), section 9.2.1 (x87 initialization), and
Chapter 6's Interrupt 0 (#DE), Interrupt 7 (#NM), Interrupt 16 (#MF), and
Interrupt 17 (#AC) entries. NE selects native vector 16 instead of legacy
FERR# external reporting; AM and user EFLAGS.AC together enable alignment
checks at CPL3. MP makes FWAIT honor TS; EM must be clear for native x87.
Repository comparison: `fpu_init` already sets MP/NE/AM, clears EM/TS before
initialization, then sets TS for lazy #NM ownership. `fpu_handle_nm` clears TS
before saving/restoring. The #DE/#MF/#AC signal mappings and handler flag
clearing already match the contract. No control-register change is warranted.
Host assertions now cover all 32 combinations of inherited MP/NE/AM/EM/TS,
for both FXSR and FNSAVE, including the values before first hardware use,
the final lazy TS, and preservation of unrelated CR0/CR4 bits.

The controller emitted four records before acknowledging each waiting
handler, within one two-second PIT deadline. `rec_emit` calls synchronous
UART output and the console; the LFB console redraws its full visible
history on scrolling. Slow physical output therefore consumes execution
time while the handler waits. A host replay of the original production
controller with a simulated 100 ms sink delay per record reproduces exactly
`observed=0 raw_vector=17 corruption=0 entries=5 returns=4`, missing indices
6 and 7, and FAIL. This reproduces the failure mechanism; the supplied
capture does not measure the T23's individual output durations.

Decision: save up to seven individual handler snapshots and acknowledge
them immediately, retaining the existing deadline. Emit those snapshots
after completion and the zombie wait. Require all seven snapshots in order,
seven entries/returns, completion and successful exit. An additional
`part=completion` record exposes completion, zombie state and snapshot count,
so a default status zero cannot be mistaken for an exit. The payload and
the exact TCG CPUID fallback remain unchanged; no fault is skipped and no
new CPU feature is required.

`test_signal_fault_records.py` compiles the production controller and tests
slow output with both real #AC and TCG's #PF fallback handshakes, partial
delivery, duplicate indices, buffer overflow attempts, and early completion
claiming 7/7 despite missing records. Existing suite predicates reject the
partial delivery independently of the terminal FAIL. These are host boundary
tests, not native payload execution or new hardware/QEMU qualification.
Worktree validation: `python3 scripts/build_kernel.py` passes, including
the FPU/SIMD audit and 4 KiB compiler frame limit; `VMM.ELF` is 1,553,720
bytes. `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`
passes (signal fixture: 5,709 checks; unchanged NASM ELF: 20,484 bytes,
production parser/load only). `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest
discover -s tests/host` passes: 204 tests, 8 skips, no failures/errors.
Logs are in `build/host/f2-22-kernel-build.log`,
`build/host/f2-22-host-kernel-tests.log`, and
`build/host/f2-22-host-unittest.log`. Scratch compilation files from the
original-controller reproduction were deleted after recording its result.
The lead must rerun QEMU `signals-fault` and the T23 sweep on the integrated
image before accepting the hardware result.

## Open items for F0 closure

- Safe-mode automation: amend the selector grammar (for example a
  fw_cfg-only `safe=1` key) in `f0-acceptance.md` and `boot-memory.md`, then
  implement it in the loader so the `safe-mode` case can run on the
  canonical image.

- Physical T23 and E500 runs (T4): the ten-boot matrix and the probes with
  screen evidence; the canonical image has never been written to a disk.
- Deterministic image builds (FAT timestamps and volume id change the image
  SHA-256 between identical builds).
- The loader's A20 keyboard-controller path, CHS fallback, BIOS menu and
  text-mode fallback were not exercised beyond `-vga none`.
