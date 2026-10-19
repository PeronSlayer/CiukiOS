# DOS file conformance battery (f3-48)

This directory contains real-mode COM test sources, a deterministic builder,
a pinned-binary candidate audit, strict transcript comparison, a production
host-model adapter and a **written, unrun** QEMU differential runner. It does
not implement DOS in Python, change the kernel, build images or claim guest
conformance from assembly checks. The contract is
[DF1–DF6](../../../docs/design/dos-dpmi-contract.md#dos-file-api-in-the-vmm).

## Host checks available now

The production C file bridge, native FAT and VFS have a focused ASan/UBSan
harness (fake guest memory, time and media; no guest boot required):

```sh
nice -n 19 bash tests/host/dos/run_file_api.sh
```

It is also run by `scripts/test/host_kernel_tests.sh`. The printed H49 labels
describe the cases actually exercised; the f3-49 report distinguishes these
from the full acceptance matrix and personality/controller integration.

Run one command at a time:

```sh
nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v
nice -n 19 python3 -B tests/host/dos/conformance.py build
nice -n 19 python3 -B tests/host/dos/inventory.py
```

The first command assembles every probe twice and checks deterministic binary
hashes, coverage and evidence rejection. It does not boot a guest. NASM from
the SDK builds the COM files; NASM 3's normal flat-COM absolute-address warning
is disabled explicitly while other warnings remain errors. Generated sources,
COMs, RUN.BAT and the case/helper hash manifest go under
`build/codex/dos-conformance/`, not tracked binary payloads. The static audit
unpacks the existing public shareware in memory and prints hashes, signatures,
exact adjacent-byte sites and their evidence level. It creates no image.

`cases.py` is the authoritative executable scenario list. The emitted manifest
maps `D48001` etc. to a stable descriptive id, covered INT selectors, fixture,
purpose and binary hash. Append new cases rather than renumbering established
ids. `child.asm` is the EXEC/inheritance/critical-abort helper; build and pin it
alongside the main probes. No external C runtime or compiler is needed.

## Results and fixtures

Each COM prints exactly one result, less than 48 bytes:

```text
D48001 PASS 0000
D48001 FAIL FFFF
```

PASS means its semantic assertions succeeded; FFFF means an assertion failed,
other failure details are the failing DOS AX. Expected errors (missing paths,
invalid handles, exhaustion etc.) are asserted inside the program, so two
implementations returning unsupported-function cannot pass by matching each
other. All results are required exactly once. There are no skip outcomes.
The comparator produces the requested function/reference/candidate/verdict
Markdown table. Missing, malformed, duplicate, unknown and contradictory
results fail. A complete identical failing battery also fails.

Probes emit to existing VMM port E9 and bounded-polled COM1 (38400 baud, 8N1).
The VMM's absent UART discards COM1 writes; its existing guest record encodes
E9 output as `group=guest ... part=1 parts=1 data=<hex>`. Standalone FreeDOS
uses COM1. The existing `dosvm_guest` host harness decodes E9 to `guest:` lines;
the comparator recognizes all three transports. Do not collect E9 *and* UART
as duplicate evidence for a standalone guest. Console calls under test may
write unrelated text; only the probe's exact result line is authoritative.

Install identical probes/helper under C:\D48. Use a writable disposable
C:\D48TEST, a fresh subdirectory per COM, and a clean JFT with only handles
0–4 initially present. Child EXEC tests assert that their first two opens
are 5/6 so inherited/private expectations cannot accidentally test another
description. The parent shrinks its block before EXEC; every failure has a
deterministic result. Reset VM/overlay after a failed probe; don't continue
with leaked handles or read-only attributes from it.

| Fixture | Required setup |
| --- | --- |
| basic | FAT16 private C:, 512-byte sectors, CP437/country 1, disposable scratch, enough free space; no ANSI/network redirector |
| share | Same plus SHARE in FreeDOS 1.3; candidate bridge's own SHARE semantics and 2F/1000 advertisement |
| keys-XY | Runtime controller injects X then Y through the virtual keyboard only after `DOSKEY READY`; no leftover command/keyboard bytes |
| fault-once | E:\D48FAULT.DAT, at least 4096 bytes of `F`; fail exactly its first data read after successful open, then retry succeeds |
| fault-always | Same path; fail data reads persistently; Fail and child-Abort paths must execute the guest INT24 handler |

Critical faults must traverse actual media/transport errors. The f3-51
controller configures equivalent faults below FreeDOS file services and at
the candidate's VFS provider. No direct handler call or missing-file error
substitutes for a critical error. Case ids, not global order assumptions,
select one-shot versus permanent failure. The full COM battery is incomplete
unless these fixtures and the input controller run.

Covered scenarios include create/read/write/EOF/delete, signed and beyond-EOF
seeks, zero-write resize, shared-offset duplicate/forced duplicate, handle
exhaustion, directory attrs/cwd/rename/nonempty rmdir, exact packed dates,
FindFirst empty/absent path, copied/interleaved guarded DTA, temporary/exclusive
and extended open, deny modes, range locks and child conflicts, 59h details,
regular/device IOCTL, FCB sequential/random/block/names, NLS, PSP/vectors,
memory, loader environment and critical Retry/Fail/Abort.

H49/H50 acceptance tables additionally require full sharing matrices, SFT/SDA
byte layouts, overflow/boundary/failure injection, cancellation/late completion,
native/DOS races, FCB/65h variants and raw/file cache coherence in the production
host harness. These kernel-visible assertions cannot be replaced by a COM
program. The delivered smoke scenarios do not claim those future harness
tests already execute or that every supported NLS/IOCTL variant is covered.

## Production host model adapter

After the lead supplies an expendable DOS volume containing the exact COMs,
configured AUTOEXEC scratch cwd and the requisite runtime fixture hooks:

```sh
nice -n 19 python3 -B tests/host/dos/conformance.py host \
  --model build/host/dosvm_guest --volume <prepared-DOS-volume> \
  --out build/codex/dos-host-results
```

This invokes the **existing** production DOS VM/x86emu CLI once per case with
`rc:` so a failing COM still yields its own record, captures each log and checks
fixture immutability. Each invocation creates the host model's own disposable
overlay. It does not make/install/copy a volume. The current stock model only
has fake backing-file VFS I/O and no general file bridge or per-case keyboard/
media fault controller. f3-49/f3-50 must extend that model using real production
bridge/VFS state before this command can qualify all cases. It deliberately
fails on missing records/fixtures rather than substituting a Python oracle.
Use repeatable `--case <id>` to run an available subset; evidence is marked `subset` and cannot satisfy the complete comparator. Host replay is not FreeDOS reference or hardware evidence.

For extracted result transcripts:

```sh
nice -n 19 python3 -B tests/host/dos/conformance.py compare \
  <reference-results.log> <candidate-results.log> \
  --out build/codex/dos-comparison.md
```

## Lead-only QEMU runner (do not run in the Codex sandbox)

[f3-dos-conformance.json](../../suites/f3-dos-conformance.json) uses a dedicated
schema **48**, rejected by the generic schema-1 runner. Its complete runner is
`qemu_reference.py`. This prevents an empty generic case list from reporting
success before the runtime probe exists. Both runs use the same canonical full
HDD; selectors choose `freedos` (bridge off) or `ciukidos` (bridge on). FreeDOS
is inside the VMM with the same virtual hardware; no floppy profile is booted.

The lead implements the f3-51 runtime adapter/selector and canonical read-back
manifest, then runs inside its already-authorized `MemoryMax=1500M`,
`MemorySwapMax=0` user scope:

```sh
nice -n 19 python3 -B tests/host/dos/qemu_reference.py \
  --image build/f0/ciukios.img \
  --integration-manifest <f3-51-read-back-manifest.json> \
  --probes-manifest build/codex/dos-conformance/manifest.json
```

No scope or QEMU command has been executed as part of f3-48. The runner refuses
an image without adapter version 1, image/probe hashes and pinned FreeDOS 1.3
kernel hash. It checks actual cgroup memory/swap limits, available memory,
other QEMU/build activity, then takes the common lock and every runner slot.
It runs QEMU sequentially, each on a qcow2 overlay under `build/test-runs/`,
never modifies/copies the canonical image, captures bounded serial output and
enforces timeouts. Its owned processes are terminated/reaped in `finally`;
successful overlays are deleted and failed overlays/logs retained for lead
diagnosis. It records full image SHA/profile/manifest identities with results.

The controller's unique run/guest BEGIN and END markers surround all forwarded
case records, and END counts must match the current case manifest. Missing
adapter/controller/reference or lack of terminal evidence is an explicit
failure; no current production image is asserted to support this selector.

Remaining source/reference/game/performance evidence is enumerated in
[f3-48 validation](../../../docs/validation/f3-48-dos-api-inventory.md) and the
finished [f3-49](../../../docs/directives/f3-49-vmm-dos-file-api.md),
[f3-50](../../../docs/directives/f3-50-ciukidos-vmm-hook-fcb-nls.md),
[f3-51](../../../docs/directives/f3-51-dos-conformance-games-and-fallback.md)
directives.

## f3-51 integration status

The canonical DOS builder now installs the unchanged COM/helper manifest,
controller shell-return witness and scratch directories. The runtime selector
accepts exactly `f3:dos-conformance run=<8 hex> guest=freedos|ciukidos`; ordinary
sweeps do not select this battery. Each basic/SHARE/input case uses a fresh
VM/COW and must return to the shell and restore the VMM ledger.

Critical cases currently fail explicitly with `fault_backend_unavailable`.
The E: removable fixture/injection backend is not implemented: this is an open
M1 requirement, not a skip or guest PASS. FreeDOS's block driver contains BIOS
retry logic; an isolated INT13 error is not sufficient evidence that INT24 ran.
The lead must resolve/qualify that injection boundary against the release.

Official release packages are absent here. `scripts/dos_conformance.py` accepts
only separately provisioned `third_party/freedos/1.3/UPSTREAM.json` pins with
`release`, `distribution_url`, and `kernel`/`shell`/`share` records. Each record
contains `archive`, `url`, `package_sha256`, `member`, `sha256`, `version`,
`license`, `license_member`, and `license_sha256`. These must describe the actual
official release archives and binaries; no sample/fabricated pin is installed.
A separate `boot` pin describes an official release archive/member, its package
and complete member hashes, boot-sector offset, 512-byte sector hash, version
and license. No boot code or kernel is compiled from master.
Canonical builds without that input remain CiukiDOS-only and produce no
integration manifest. A present but corrupt reference fails the build.

With provisioned packages the same canonical build installs `FREEDOS.IMG` and
`FREEDOS.SHA`, then writes `build/f0/dos-conformance.json` by reading back the
outer image and both embedded FAT fixtures. Its probe manifest is
`build/f0/dos/conformance/manifest.json`. The runner independently repeats that
read-back before accepting the evidence manifest. This does not qualify boot,
SHARE execution, media faults, games or physical performance.

The lead-only runner requires an ordered complete transcript, matching run
and guest identities, increasing VMM serial sequence, zero reported dropped records,
and `D48 CLEAN ... handles=0 searches=0 locks=0 vms=0 dropped=0` before END.
It performs two independent guest pairs on the identical base SHA. It always
reaps its owned processes and removes its COWs, including failure paths;
bounded serial logs and comparison tables remain as text evidence. Disk-only
diagnostics use the same sequence allocator, so generic sequence gaps require
the lead's disk logs; they cannot be classified as UART loss from serial alone. This
supersedes the earlier paragraph about retaining failed QEMU overlays.

`reachability.py` decompresses the seven LZEXE members in memory and follows
explicit decoded edges. It retains unknown selectors and unresolved transfers.
DOOM's bound LE body, Quake's go32/runtime body and actual title paths still
require the lead's disassembly/trace inputs; static results are not executions.
