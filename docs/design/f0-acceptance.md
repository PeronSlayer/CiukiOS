# F0 acceptance: evidence for the Ciuki VMM scaffold

Author: Codex. Reviewer and integrator: Claude (lead). Status: approved
after the Claude–Codex cross-review, 2026-10-09.

## Decision

F0 is accepted only when the eleven criteria agreed in
[`2026-10-09-06-decisione-fondamenta-32bit.md`](../../dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md)
have reproducible evidence for one canonical HDD image on QEMU/SeaBIOS,
ThinkPad T23 and Armada E500. Embedded probes exercise the production memory,
exception and scheduling paths before an application loader exists. This
contract specifies evidence and orchestration; it does not claim that the
new kernel or runner already exists. It extends
[`test-architecture.md`](test-architecture.md); its image, resource and
retention rules remain binding.

## Sources

- [Intel SDM, Volume 3A](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf),
  protection, interrupt/exception handling and task management: fault semantics
  and processor state inspected by the probes.
- [QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html),
  [disk images](https://www.qemu.org/docs/master/system/images.html),
  [qemu-img](https://www.qemu.org/docs/master/tools/qemu-img.html) and
  [QMP](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html):
  runtime selection, overlays, observation and termination.
- Upstream systemd documentation for
  [resource control](https://github.com/systemd/systemd/blob/main/man/systemd.resource-control.xml)
  and [systemd-run](https://github.com/systemd/systemd/blob/main/man/systemd-run.xml):
  enforced scope limits.
- [IBM T20/T21/T22/T23 Hardware Maintenance Manual, April 2002](https://download.lenovo.com/pccbbs/mobiles_pdf/62p9631.pdf),
  printed pages 63 and 141: disk servicing and rear serial connector.
- [Compaq E500/V300 Maintenance and Service Guide](https://h10032.www1.hp.com/ctg/Manual/c01129749.pdf),
  rear components, hard-drive servicing and Appendix A: serial connector
  and its RX, TX and ground pins.
- [TI PC16550D datasheet, sections 8.4–8.6](https://www.mouser.com/datasheet/2/405/pc16550d-443503.pdf):
  reference UART register semantics, not proof of either laptop's UART model.

Repository evidence: `Makefile:qemu-test-full` currently calls
`scripts/qemu_run_full.sh --test --no-build`; that script's `MODE == test`
branch expects legacy `[STAGE1] CIUKIDOS ABI2 valid` and `[DESKTOP] READY`
markers. These do not establish F0. `scripts/capture_physical_logs.py:main`
checks `SYSTEM/SHELL.COM`, copies bounded files from `PATHS`, hashes them and
writes `manifest.json`. Its legacy imports and volume check require migration.
`src/boot/floppy_stage1.asm:serial_init` hardcodes COM1 and divisor 3;
`serial_putc` bounds its transmit poll to `0x1000` iterations. Preserve that
boundedness principle, not an assumed hardware configuration. Semble was used
for discovery; it could not index this assembler file, so exact label searches
and small adjacent ranges were used. At drafting, `scripts/test/`,
`tests/suites/` and `src/kernel/` are prospective interfaces.

## Normative scope and probe selection

**[F0]** Every MUST below is mandatory for F0 unless explicitly labelled
otherwise. SHOULD states a recommendation; MAY states an option. F0 MUST use
the shared i386, non-PAE, uniprocessor, 4 KiB paging model, supervisor kernel
above `0xC0000000`, `CR0.WP=1`, legacy PIC and 1000 Hz PIT. The authority for
these choices is the binding decision and the other foundation contracts.

**[F0]** Small C/NASM probes MUST be linked into `src/kernel/` as immutable
code/data with a fixed registry. Ring 3 payload bytes MUST be copied into
ordinary private user mappings and launched through the real context/cleanup
paths; no ELF loader, filesystem application or alternate scheduler is needed.
Test state MUST remain supervisor-only. Narrow expected-fault fixups MUST match
the exact instruction, vector and address; unrelated kernel faults MUST panic.

**[F0]** The selector grammar MUST be
`f0:<probe-id|all|core> run=<8-hex-digit-id> [platform=e500] [safe=1]`, at
most 64 ASCII bytes, keys in that order and each at most once. `core` runs
every probe except `panic` and then pages the evidence on screen: it is the
selector for photographed hardware runs, where `all` would end in the halted
panic screen and lose the earlier records (added 2026-10-10). `platform=e500`
and `safe=1` are accepted only through validated QEMU fw_cfg; other sources
MUST reject them as a selection error. `safe=1` has exactly the effect of
`safe=1` in `BOOT.CFG` or of the menu's `S` key (flag `CBI_F_SAFE_MODE`,
640×480 preferred, optional devices off); on physical machines safe mode
is selected only through the menu or `BOOT.CFG`. The `boot` probe reports
`safe_mode=<0|1>` in its `group=boot` record (added 2026-10-10 to automate
the `safe-mode` case). Unknown,
oversized or malformed requests MUST emit a selection error and run no probe.
When no request is present, boot MUST enter ordinary scaffold operation without
destructive tests.
`all` MUST execute in the order below, with panic last.

**[F2, f2-12 grammar amendment]** The F0/F1 grammar above is unchanged.
Only `f2:crash-isolation` gains a final optional `server=desktop|standin`
key, after `platform=e500` and `safe=1` when present; duplicates, other values,
wrong order and other phases/probes MUST fail selection. Omission selects
desktop for an available LFB and payload, otherwise stand-in; safe/text mode
always selects stand-in. The 64-byte bound remains, so the 67-byte E500 safe
selector with all three suffixes MUST be rejected (omit `server` there).
See [the f2-12 amendment](f2-acceptance.md#selection-and-evidence) for the
screen-observation decision and primary QMP references. Boot-loader and kernel
validation MUST agree before explicit server selectors can qualify a run.

**[F0]** On QEMU the loader MUST verify signature `QEMU`, enumerate the file
directory and read exactly `opt/it.alcybercloud.ciukios/test` using the bounded
PIO interface. It MUST handle directory big-endian fields and item lengths;
DMA is unnecessary. Physical selection MUST use a loader menu, or a bounded
serial command after `SELECT_READY`, and pass the same request in boot data.
No item means no automatic test. Physical firmware MUST NOT be assumed to
provide fw_cfg. These choices follow the
[fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html).
`boot-memory.md` and `device-firmware-ownership.md` MUST define when this
optional port probe is safe and who owns the selector ports.

**[F1–F4]** New suites MUST retain this protocol and canonical-image model.
ELF loading, FAT32/LFN, DOS/DPMI, GUI crash isolation, audio and acceleration
MUST acquire their own phase gates; they are not prerequisites for F0.

## Acceptance tests

### Eleven criterion probes

**[F0]** Each row is one probe, although it MAY contain subcases or require
multiple boots. Every measured field MUST be emitted in `DATA` records, followed
by exactly one terminal result. Any unmet condition, unexpected fault, missing
record or deadline is failure; lack of a required target is `not_run`, never
pass. Thresholds below are acceptance requirements, not claims about hardware.

| Criterion / probe | Action and exact pass condition | Required evidence |
| --- | --- | --- |
| 1 / `boot` | Boot the same canonical HDD image ten times per target: five cold starts and five restarts. QEMU MUST include a 128 MiB SeaBIOS/pentium3 profile. Every attempt reaches `READY`, completes ten seconds of timer progress, and has no unexpected reset, panic or boot failure. Hardware installed RAM MUST be measured and at least 128 MiB. | Attempt number, cold/restart classification, boot/build identity, CPUID, measured RAM, boot drive, ready tick and final tick; ten individual results per target. |
| 2 / `bootinfo` | Validate actual `ciuki_boot_info` v1, kernel/loader extents, E820 bounds and explicit reservations using the production validator. Synthetic copies with bad version/length, overflowing ranges, malformed entries and illegal overlaps MUST be rejected without changing the live map. Usable/reserved/free totals MUST reconcile with the normalized map and allocation ledger defined in `boot-memory.md`. | Raw E820 entries, normalized ranges, reservation reasons, byte/page totals, validator error for each negative subcase, map digest. |
| 3 / `allocator` | Preallocate emergency reporting resources, then exhaust the ordinary free-page pool. The next allocation MUST return the declared failure without panic or partial allocation. Release everything; repeat 100 mixed allocation/free cycles. Every allocation MUST be outside all reserved ranges, unique while live, and preserve written patterns. Final free-page and resource counts MUST equal the baseline. | Baseline, exhausted and final counts, failed request/error, cycle count, duplicate/reservation violations, pattern errors. Never probe reserved MMIO by writing patterns. |
| 4 / `protection` | Verify PE/PG/WP set, PAE clear, GDT/IDT bounds, loaded TR/TSS and privilege-transition stack. Audit all kernel PDE/PTE paths as supervisor and kernel stack guards as non-present. Execute a narrowly recoverable ring 0 write to a dedicated read-only page and access to a dedicated stack guard: both MUST produce the expected #PF, with no changed protected data. Ring 3 transitions MUST use the correct kernel stack. | CR0/CR3/CR4, descriptor summaries, kernel mapping violations, guard address, vector/error/EIP/CR2 for both injections, stack bounds/canaries. |
| 5 / `isolation` | Map a writable page at user VA `0x00400000` in each of two address spaces, backed by distinct physical pages. Write different sentinels, perform at least 100 alternating context switches, then re-read in each task. Both values MUST persist; PDEs 768–1022 MUST have identical supervisor mappings, while PDE 1023 MUST reference each address space's own directory, supervisor-only, and is excluded from equality comparisons (`boot-memory.md`). Native page zero MUST be absent. | VA, two PFNs/CR3 values, initial/final sentinels, switch count, null-page and shared-kernel audit. |
| 6 / `preempt` | Run two ring 3 assembly loops which never yield or issue syscalls. Complete exactly 10,000 task-to-task switches caused by timer preemption, excluding idle dispatches. Each task MUST receive at least 4,000 dispatches; neither runnable task may wait more than twice the declared scheduler quantum. Timer-sampled loop progress MUST advance in both. GPR, permitted EFLAGS, selector, stack-canary and stack-bound checks MUST report zero corruption. | PIT IRQ/tick counts, scheduler quantum, per-task dispatch/progress counts, maximum wait, register/stack errors, switch causes. No per-switch serial output. |
| 7 / `localfault` | In fresh offender tasks, attempt kernel-page read/write, `CLI`, and denied `IN`/`OUT`. Kernel accesses MUST #PF; the privileged/I/O cases MUST #GP under the declared IOPL/TSS policy. Only the offender MUST terminate. A concurrent survivor MUST advance at least 100 timer samples after each termination with unchanged sentinels. | Subcase, task ID, fault vector/error/EIP/CR2 when applicable, offender exit reason, survivor progress and value. Use a harmless reserved test port, never an ATA/PIC output port. |
| 8 / `syslife` | Pass unmapped, kernel, wrapping and page-straddling invalid buffers to the real syscall buffer-check path, including unreadable sources and unwritable destinations. Each MUST return the ABI error without partial side effects. Perform 100 create/fault/exit/reap cycles covering both faulted and normal exits. Free pages, tasks, mappings and handles MUST return to baseline after every cycle. | Buffer case/error/side-effect count; cycle totals and before/after ledgers. Zero leaked resources or survivor damage. |
| 9 / `panic` | Deliberately fault ring 0 at a known unmapped VA. Panic MUST report vector 14, expected error/EIP/CR2, then halt without allocation, locks or disk operations. No heartbeat or resume is permitted during five seconds of external observation. Repeat with UART absent/disabled: boot and screen reporting MUST still work. | Pre-injection record, complete panic record, five-second observer result, storage-call counter delta zero; QEMU block-stat deltas zero after injection is armed, and panic-path call audit. |
| 10 / `fpu` | Apply precisely the F0 policy declared in `execution-abi.md`. If user FPU is enabled, two preempted users with distinct x87 control/data state MUST preserve it through 1,000 switches and one task's destruction; test XMM/MXCSR only when CPUID and the policy enable SSE. If F0 explicitly denies user FPU, x87 MUST fault locally and the survivor MUST continue. Kernel code MUST contain no prohibited FP instructions/helpers. | Policy ID, CPUID features, state comparisons or expected denial fault, survivor result, build-time instruction/helper audit. Unsupported features are reported, never silently executed. |
| 11 / `runner` | Host orchestration probe exercises lock contention, malformed/missing markers, timeout, child termination, insufficient memory, disk-budget refusal and retention against fixtures; then observes a real canonical QEMU run and physical selector/evidence imports. Pass requires enforced caps, one QEMU, correct image hash, clean teardown and correct overlay retention. | Fixture outcomes, scope properties, actual child count, image hashes before/after, artifact inventory, physical menu/serial request and matching response. This host probe is not a kernel's self-attestation. |

**[F0]** Exception expectations MUST be checked against Intel's protection and
exception definitions; particularly U/S and WP protection, #GP privilege/I/O
checks, and #PF error bits/CR2. Page-table inspection alone is insufficient;
the injected accesses above MUST execute. See
[Intel SDM Volume 3A](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf),
protection and interrupt/exception chapters. This does not require NX on the
shared non-PAE baseline.

### Evidence protocol

**[F0]** Available serial and screen sinks MUST emit identical ASCII
records, one emission per sink; the `boot` video-fallback subcase with
`-vga none` requires serial evidence only. Grammar:

```text
CIUKI_TEST v=1 run=12ab34cd seq=000001 probe=boot event=BEGIN
CIUKI_TEST v=1 run=12ab34cd seq=000002 probe=boot event=DATA tick=10000
CIUKI_TEST v=1 run=12ab34cd seq=000003 probe=boot event=END status=PASS
CIUKI_TEST v=1 run=12ab34cd seq=000004 probe=panic event=PANIC vector=14 error=00000000 eip=c0101234 cr2=dead0000
```

Records MUST fit 240 bytes, use unique keys, decimal counters and fixed-width
hex addresses, and carry monotonic sequence numbers. Long data MUST use
multiple records. Nonfatal probes MUST end with their own terminal records;
panic MUST have no kernel `PASS` after halting. For `all`, only the observer
MUST produce an aggregate result, checking every preceding probe plus the
expected panic record and halt. A screen MUST preserve the final record
and provide paging for earlier evidence; photographs/transcriptions MUST retain
run IDs and sequence numbers. Panic MUST remain visible without interactive
paging. Raw serial is authoritative; the legacy
`scripts/serial_log_normalize.py:_normalize_line` MUST NOT repair F0 markers.

**[F0]** UART output MUST use finite polls and a total record budget independent
of PIT interrupts; absence MUST disable the sink without delaying boot
indefinitely. Panic MUST use preallocated buffers and direct bounded output.
The UART's THRE/TEMT meanings come from the
[TI reference datasheet](https://www.mouser.com/datasheet/2/405/pc16550d-443503.pdf);
port address, clock and availability MUST be established separately.

**[F0]** `result.json` MUST contain schema version, run ID, suite/probe,
UTC start/end, outcome/reason, image SHA-256, image size, build manifest hash,
build git revision and dirty-state declaration, runner revision, selector,
expected/observed evidence, artifact hashes and timeout/cleanup results.
QEMU results MUST include executable version, full arguments, pinned machine
version, CPU, accelerator, RAM, firmware hash and devices. Physical results
MUST include operator-confirmed model/unit identity, BIOS version where known,
CPUID, installed RAM, observed PCI IDs, target disk identity, write/readback
hash and capture settings. Unknown identity fields MUST say `unknown`.
The image hash MUST be computed externally: embedding its own SHA-256 would
be circular. An embedded build ID plus verified write/readback associates
hardware records with the canonical image. Prior log writes MUST be distinguished
from immutable image bytes.

### Runner and execution order

**[F0]** `scripts/test/run.py <suite>` MUST read versioned JSON suite files
under `tests/suites/`. Each MUST declare `image=full`, pinned profile, selector,
required markers/data predicates, expected terminal event and timeout.
`f0-smoke.json`, `f0-core.json`, `f0-panic.json` and `f0-runner.json` MUST
separate boot, nonfatal probes, terminal fault and host orchestration.
`make qemu-test-full` MUST delegate to smoke without rebuilding. CD suites
become mandatory **[F1 or the first phase claiming CD support]**.

**[F0]** The runner MUST hold an advisory exclusive lock at
`build/test-runs/.qemu.lock` through teardown, shared with manual QEMU launchers
and the other agent's worktree. Worktrees MUST resolve one common lock inode;
separate per-worktree locks do not serialize QEMU. Existing unowned
`qemu-system` processes or a heavy build MUST cause refusal, not termination
of another session. Lock contention MUST fail promptly with an explicit reason.

**[F0]** Before launching, check host `MemAvailable` is at least 2 GiB,
record it, and verify a functioning user systemd/cgroup memory controller.
Every QEMU MUST run in a uniquely named scope:

```text
systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0 -- <qemu and arguments>
```

The runner MUST verify effective limits and refuse an unrestricted fallback.
OOM is a failed run. These properties enforce unit memory/swap limits as
documented by [upstream systemd](https://github.com/systemd/systemd/blob/main/man/systemd.resource-control.xml);
scope creation follows
[systemd-run](https://github.com/systemd/systemd/blob/main/man/systemd-run.xml).
QEMU and full/CD builds MUST remain sequential. Resource thresholds MUST NOT
be raised automatically.

**[F0]** Create a fresh overlay at
`build/test-runs/<suite>/<run-id>/run.qcow2` using
`qemu-img create -f qcow2 -b <absolute-canonical-image> -F raw <overlay>`.
Only the overlay MUST be attached writable. No full-image copy, `-snapshot`,
RAM dump or `/tmp` artifact is permitted. Sockets, temporary files and captures
MUST stay in the run directory. Explicit overlays avoid QEMU's temporary
snapshot files; see [QEMU images](https://www.qemu.org/docs/master/system/images.html)
and [qemu-img](https://www.qemu.org/docs/master/tools/qemu-img.html).

**[F0]** Default wall deadlines MUST be 30 seconds for smoke, 180 for core,
and 45 for panic including observation. Timing starts at child launch and
MUST use a host monotonic clock. Missing terminal evidence MUST fail even if
QEMU exits zero. On completion, timeout or interruption: request QMP `quit`,
wait at most two seconds, terminate the owned scope, then kill it if necessary
after another two seconds; reap children and verify none remain before unlocking.
An unexpected reset MUST fail. F0 need not implement ACPI shutdown for tests;
the [QMP quit command](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-quit)
provides host termination. QMP block counters support the panic observation
([query-blockstats](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-query-blockstats)).

**[F0]** Pass MUST delete overlay/screenshots and keep only `result.json` and
raw serial log. Failure MUST retain bounded artifacts. Keep the last five runs
per suite; archive essential textual evidence before pruning. Refuse before
creation when `build/test-runs` exceeds 2 GiB, and stop a run before growth
passes that budget. Count both logical and allocated sizes to catch sparse
files. Serial/QMP/stderr logs MUST each have a 4 MiB cap; no dumps by default.
Pruning MUST occur under the lock and MUST NOT remove active runs.

**[F0]** Attempt T0 host validator/parser/runner checks, T1 image/layout/license
and payload checks, T2 smoke, then T3 `bootinfo`, `allocator`, `protection`,
`isolation`, `preempt`, `localfault`, `syslife`, `fpu`, and finally `panic` in
a fresh boot. T4 MUST repeat those probes on both laptops and complete the
ten-boot matrix. A suite MUST stop on prerequisite failure and record later
probes as not run. The identical order applies to menu-selected hardware tests.

### Emulation profiles and hardware replay

Added 2026-10-09 at the owner's request (no serial adapter available yet):
no emulator reproduces the T23 or E500 (no S3 Savage, Mach64 Rage Mobility,
ESS Maestro-2E, IBM/Compaq firmware or 830MP/ICH3-M model in QEMU, Bochs,
86Box or PCem; research recorded in
`dev_diary/2026-10-09-08-gemelli-t23-e500.md`). F0 therefore uses
**twin profiles** that match what the new architecture actually exercises
on those laptops (generic VBE LFB, i8042, ATA, PIT/PIC), plus replay of the
real firmware data captured on them.

**[F0]** Profiles live in `tests/profiles/*.json` and are referenced by
suites. Every profile pins the QEMU machine version, uses `-cpu pentium3`
and PIIX IDE for the canonical image (through its overlay). Profiles default
to the standard VGA device (Bochs VBE LFB), i8042 PS/2 and COM1 at `0x3F8`
logged to a file. Suites MUST declare runtime device exceptions for the
video-fallback and UART-absent subcases and record the effective
configuration:

| Profile | Accelerator | RAM | Purpose |
| --- | --- | --- | --- |
| `qemu-t23` | TCG | 512 MiB | ThinkPad T23 twin: RAM as measured on the owner's unit (130,656 usable pages), AC97 `82801AA` (same programming model class as the T23's ICH3 AC97) |
| `qemu-e500` | TCG | 256 MiB | Armada E500 twin: RAM as measured (65,264 usable pages); selector key `platform=e500` forces the firmware-first input policy (below) |
| `qemu-min128` | TCG | 128 MiB | minimum supported RAM |
| `qemu-fast` | KVM | 256 MiB | quick developer smoke only; never evidence of CPU-feature correctness |

TCG is mandatory for evidence: under KVM the host CPU executes instructions
a Pentium III lacks (for example SSE2) even when CPUID hides them, so such
defects would pass silently.

**[F0]** Evidence profiles (`qemu-t23`, `qemu-e500`, `qemu-min128`) run TCG
with `-icount shift=1,sleep=on`. Without it the guest's TSC and PIT follow
host wall-clock time, so interrupt-disabled intervals measured by the
kernel include host scheduling stalls and TCG translation work (observed on
2026-10-09: 442 µs with concurrent host load, 976 µs on an idle host, against
35–66 µs typical, all for the same code). With instruction counting:

- during execution, guest time advances 2 ns per executed instruction
  (shift 1: a synthetic 500 MIPS machine, not a Pentium III cycle count,
  and QEMU disclaims cycle accuracy);
- during `HLT`, `sleep=on` lets virtual time advance to the next timer
  deadline paced to real time, so guest time does **not** advance only
  through instructions; busy execution is not guaranteed to track real
  time (`align=off` is the default), and the runner's host-monotonic
  deadlines remain a separate limit that can still expire under host load;
- host stalls during busy execution no longer appear in guest timing: they
  are hidden, not measured. Synthetic (icount) timing and observed host
  elapsed time are therefore distinct evidence and MUST be labelled as
  such in records and validation documents.

Under icount, `budget_violations = 0` and `critical_us ≤ 250` are a
regression gate in instruction time (about 125,000 instructions for a
non-halting section), not proof of the physical 250 µs requirement, which
only the laptops can establish. The probe MUST report `critical_ns` beside
`critical_us` (a `critical_us` of 0 is truncation below 1 µs, not a zero
duration) and MUST report a nonzero `tsc_khz` calibration. `qemu-fast`
(KVM) never uses icount and never produces latency evidence. Physical
machines measure real time; a stall there is reported as unclassified and
resolved during qualification.
([QEMU icount documentation](https://www.qemu.org/docs/master/devel/tcg-icount.html),
[QEMU invocation, `-icount`](https://www.qemu.org/docs/master/system/invocation.html))

**[F0]** Hardware replay fixtures live in `tests/fixtures/hardware/t23/` and
`tests/fixtures/hardware/e500/`: raw E820 maps (`MEMMAP.BIN` captures), VBE
controller and mode-information traces (`VBE.TRC`), PCI identities
(`ACTIVE.CFG`) and EDID absence, extracted from the physical log captures
archived in `legacy/local/` with their source capture name and SHA-256.
They contain no disk serials or personal data. T0 host tests feed them to:

- the kernel's `ciuki_boot_info` validator and E820-to-allocator
  initialization (C, compiled for the host);
- `scripts/test/loader_model.py`, a reference model of the loader rules in
  `boot-memory.md` (E820 normalization, VBE mode eligibility and preference,
  input-policy decision), whose output for QEMU's own data MUST match the
  loader's real `ciuki_boot_info` from a `qemu-t23` boot (T2/T3 check).

Expected results per fixture (for example: the T23 VBE trace includes the
malformed-scanline mode that must be skipped) are stored beside it.

**[F0]** A second emulator, Bochs, MAY be used for debugging (strict CPU
checks, single-step debugger, different BIOS and VGA BIOS). It is not part
of the F0 gate. 86Box is deferred: it needs separately sourced ROM sets and
does not model either laptop.

**[F0]** Twin profiles never replace T4: the F0 gate on physical machines
still requires the T23 and E500 runs. Until a serial adapter exists, those
runs use screen evidence with the paging rules above.

### Physical procedure and failure records

**[F0]** Writing MUST target an explicitly selected expendable test disk.
Before writing, record model, serial/WWN when available, capacity and Linux
disk sequence; identify by stable path and recheck after reconnection.
Reject mounted disks, host root disks, undersized media and unconfirmed targets.
An operator MUST authorize the concrete destructive target. Write the canonical
image once, flush, and read back/hash exactly its byte length; do not save raw
disk dumps. Disk removal/reinstallation MUST follow the
[IBM servicing instructions](https://download.lenovo.com/pccbbs/mobiles_pdf/62p9631.pdf)
or [Compaq guide](https://h10032.www1.hp.com/ctg/Manual/c01129749.pdf).
No physical overlay or fw_cfg exists. Tests MUST avoid modifying boot/kernel
extents; any optional log writes MUST remain in a declared bounded region.

**[F0]** Both manuals establish a built-in serial connector:
[T23 rear view, printed page 141](https://download.lenovo.com/pccbbs/mobiles_pdf/62p9631.pdf#page=147),
and [E500 rear components and Appendix A](https://h10032.www1.hp.com/ctg/Manual/c01129749.pdf).
The E500 pin table identifies RX=2, TX=3 and ground=5. They support using
RS-232 capture; they do not prove the actual units' port health or configuration.
The operator MUST check firmware enablement and perform loopback/capture
verification. Prefer a null-modem cable to a host RS-232 receiver; a host USB
RS-232 adapter is acceptable and needs no guest USB driver. A TTL UART cable
MUST NOT be connected directly. Use 38400 baud, 8N1, no flow control as the
initial capture setting, verify the actual guest divisor/clock, and record
changes. Start capture before power-on into `legacy/local/<run-id>/`.

**[F0]** With an unavailable serial port, use the boot menu and preserve screen
records/photos plus an explicit transport limitation. Guest USB/CardBus serial
support MUST NOT become an accidental F0 dependency. Hardware deadlines MUST
use external observation, with the same suite deadlines plus up to 120 seconds
for firmware startup; failure is recorded before operator reset/power-off.

**[F0]** The successor to `scripts/capture_physical_logs.py:main` MUST preserve
bounded copying, hashes, missing-record reporting and an acquisition manifest.
It MUST enforce a read-only mount, identify F0 through its build manifest
rather than `SYSTEM/SHELL.COM`, and read an allowlist such as
`SYSTEM/LOGS/F0.LOG` and the manifest, each at most 128 KiB. An F0 disk log
MAY exist only after qualified storage initialization, using a preallocated
bounded sink; no FAT32/native VFS implementation is required solely for logging.
The collector MUST explicitly record `disk_log=unavailable` otherwise. Serial
or complete screen evidence remains mandatory, particularly for panic, which
MUST never flush a disk log. **[F1–F4]** Once storage logging is implemented,
its flush/error behaviour MUST follow `vfs-storage-contract.md`.

**[F0]** Every gate failure MUST produce a new Italian diary entry following
`dev_diary/README.md`, indexed there by the lead. It MUST record image hash,
revision, machine/profile, selector, expected/observed result, bounded evidence
location, first failing sequence and unattempted criteria. Hypotheses MUST be
separated from measured facts. Corrections MUST rerun affected probes and
smoke on the new image; prior-image evidence MUST NOT be relabelled. Lead
cross-review and the seven completed contracts are prerequisites for F0 code.
F0 acceptance MUST NOT resume prerelease publication: D10 requires F1 and
a requalified Windows bundle.

## Open questions

- Actual T23/E500 serial base, IRQ, clock, firmware enablement and electrical
  health are unmeasured. Close with firmware settings, discovery records and
  successful loopback plus externally captured records from each owned unit.
- Actual BIOS versions, installed RAM, E820 reservations and boot-disk identity
  are not established here. Close with per-unit inventory and `bootinfo` logs;
  minimum RAM is a requirement, not an assertion about these machines.
- An optional safe F0 disk-log sink is not yet qualified. Close with a declared
  reserved extent/file, bounded writer, interrupted-write test and read-only
  collection evidence, or explicitly defer it and use serial/screen for F0.
- Shared lock placement across agent worktrees needs integration evidence.
  Close by demonstrating two launchers contend on the same inode and a second
  worktree cannot start QEMU while the first owns it.

## Interfaces required from other contracts

| Contract | Interface required and mandatory phase |
| --- | --- |
| `foundations-transition.md` | **F0:** atomic canonical boot/build replacement, build manifest identity, phase gates, review prerequisite and suspended publication; **F1:** release resumption evidence. |
| `boot-memory.md` | **F0:** v1 selection field, safe loader menu, copied firmware records, extent/reservation validation, normalized totals, 128 MiB ledger and framebuffer/text fallback. |
| `execution-abi.md` | **F0:** embedded task creation, syscall buffer errors, context/register rules, quantum, TSS/guard policy, cleanup counters and exact FPU policy. **F1–F4:** loader and process resource extensions. |
| `vfs-storage-contract.md` | **F0:** any optional bounded log sink's ownership and prohibition before initialization; **F1–F4:** durable log flush/error and collection format. |
| `dos-dpmi-contract.md` | **F0:** confirmation that probes need no DOS VM or DPMI host; **later DOS phase:** VM-specific address-space/lifecycle probes and reflected-fault evidence using this runner. |
| `device-firmware-ownership.md` | **F0:** UART discovery/bounded output, fw_cfg probing policy, PIC/PIT ownership, IRQ/EOI counters, safe test port, storage quiescence and panic output. **F1–F4:** driver/firmware qualification suites. |
