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

Critical cases use disposable E: media with 4096 known `F` bytes. The controller
selects the fixture before boot. A matched successful open return arms it;
metadata reads and BIOS resets cannot consume or rearm the failure. FreeDOS
2043's `LBA_Transfer` retries five times (`N_RETRY=5`), so one fault episode
fails five BIOS completions before INT24. The candidate fails one logical VFS
provider completion. One-shot clears for DOS Retry; persistent remains armed
through Fail/Abort. A fresh VM resets generation, cache and injection state.
Keyboard case D48036 receives X,Y only after `DOSKEY READY`. The COM programs
and EXEC helper remain unchanged. Actual guest INT24 counts still need lead
qualification on the final image.

Official release packages are provisioned and pinned. `scripts/dos_conformance.py` accepts
only separately provisioned `third_party/freedos/1.3/UPSTREAM.json` pins with
`release`, `distribution_url`, and `kernel`/`shell`/`share` records. Each record
contains `archive`, `url`, `package_sha256`, `member`, `sha256`, `version`,
`license`, `license_member`, and `license_sha256`. These must describe the actual
official release archives and binaries; no sample/fabricated pin is installed.
A separate `boot` pin describes an official release archive/member, its package
and complete member hashes, boot-sector offset, 512-byte sector hash, version
and license. The floppy `boot` sector is FAT12; the installed FAT16 template
comes from the pinned package's compressed SYS.COM (`boot_fat16`), reproduced
by a CPU-only host extraction test. See the [reference provenance](../../../third_party/freedos/1.3/README.md).
No boot code or kernel is compiled from master. The reference has one MBR
partition at LBA 63, within the unchanged VMM disk capacity.
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
diagnostics use the same sequence allocator. Before deleting each overlay,
the runner reads its bounded BOOT/BOOT1/BOOT2 logs through a read-only qcow2
view, without conversion, mounting or copying the base. It reconciles every
global sequence through the closing ledger with the serial log. Missing,
conflicting or duplicate records fail. A log read-back/reconciliation failure
still removes the owned overlay. This
supersedes the earlier paragraph about retaining failed QEMU overlays.

`reachability.py` decompresses the seven LZEXE members in memory and follows
explicit decoded edges. It retains unknown selectors and unresolved transfers.
It also maps DOOM's bound MZ/LE executable objects and Quake's DJGPP COFF text,
then decodes direct edges from the structurally established entries. Backward
targets are decoded at their actual boundaries. Indirect/far edges, relocations
and unproved alternate entries remain open; static results are not executions.

DOS API trace export uses four bounded `group=dos_api` fragments per row,
with a VM/generation, monotonic row, call sequence, registers, PSP/origin,
timing and byte count. `edge=0/1/2/3` means entry/return/process-termination/
VM-cancellation. Termination/cancellation is never a fabricated return result.
The protected UART carries trace rows; the disk budget holds disk-only rows.
`dos_trace_end` closes each VM's trace count and pending ledger, and
`record_ledger` closes the shared output sequence. Rows drain at each dispatch
and return, rather than waiting for the finite ring to overflow.

For title paths the lead supplies measured stage ranges and concrete coverage
in an evidence JSON (image/source/original-executable SHA-256, title, VM and
generation, coverage, paths and individually approved errors).
`trace.py --serial ... --disk ...
--run <id> --image <canonical-image> --evidence ... --out ...` requires loader,
start, menu, demo, play, save, load, config-write, config-reopen and exit ranges,
each containing actual reconciled calls. Missing fragments, unreturned calls,
cancelled accepted stages, unknown selectors, unsupported DOS functions and
unapproved errors fail. No complete title trace has been collected by Codex.

## Unified CiukiDOS binding (f3-56)

The DFV2 binding between the CiukiDOS fork of FreeDOS 2043, its control
driver `CIUKIDF.SYS` and the VMM has its own host qualification:

```sh
nice -n 19 bash tests/host/dos/run_unified_binding.sh
```

It builds the hash-pinned fork (patch 0001 + patch 0002 + the files in
`third_party/freedos/unified/ciuki/fork/`, see `fork.json`) twice and the
pristine patch-0001 kernel with the pinned Open Watcom 1.9 and NASM tools,
then runs `unified_binding_test.c` (the native adapter against synthetic,
guarded guest memory: negotiation, layout, frame/spoof and lifecycle
matrices with failure injection) and `unified_cpu_test.c`, which boots
small FAT16 disks through the pinned MBR/VBR on libx86emu with a ROM BIOS
and the production `dos_unified.c` at the INT FE gates. The guest programs
are in `h56/`: `h56.asm` (layout, handles, EXEC/TSR, PSP, internals, direct,
PENDING, spoof and failure-injection tests), `child.asm`, `h56fcb.asm` and
`h56nls.asm` (print-only FCB/NLS probes compared with the pristine kernel)
and `runall.asm` (runs the unchanged basic-fixture f3-48 probes, whose
results must be identical on the pristine kernel and the bound fork).
ASan/UBSan stay on with leak detection at its default. The script needs
libx86emu headers and the ignored FreeDOS 1.3 packages in
`third_party/freedos/1.3` (or in the primary checkout of a worktree), and
is part of `scripts/test/host_kernel_tests.sh`. `test_unified_fork.py`
checks the fork pins and shared constants without building.

Evidence limits: the native backend is a fixture; every file operation is
FreeDOS's local FAT (the fixture boots load `CIUKIDF.SYS /LOCAL`, the
explicit isolated-fixture policy). f3-57 supplies the live VFS backend
(next section). Editing the fork:
`python3 -B scripts/unified_fork.py --checkout build/<dir>`, edit, then
`--refresh build/<dir>` regenerates patch 0002 and `fork.json`. Wire layout
and decisions: `docs/validation/f3-56-unified-binding.md`.

## Unified CiukiDOS local IFS (f3-57)

```sh
nice -n 19 bash tests/host/dos/run_unified_file.sh
```

`unified_file.py` builds the fork twice (determinism) and the pristine
patch-0001 kernel, compiles `unified_cpu_test.c` with `-DH57_IFS` (the
production `dos_unified.c` + `dos_unified_ifs.c` over the production
VFS/FAT and `dos_file_volume.c`, mounting the boot disk's partition as C:
and a small FAT16 volume as read-only D: at the final bind), and boots:
`h57/h57.asm` (`H57.COM F`: drive classification 4409h/4408h/32h/1100h,
create/read/seek/zero-length truncate, dup/forced dup, copied-DTA search,
attributes, rename/delete, directories and current-directory refusal,
36h, locks, 6C00h actions, file times, checked FCB-close size metadata,
read-only D:, and INT 24h Retry/Fail/Abort and O_NOCRIT through provider
read faults armed on port EBh); the f3-56 suite `H56.COM N` over the live
backend; the activation barrier (refused mount: contained stop before the
shell; `/LOCAL`: local FAT; a view beyond LASTDRIVE: refused after the
mount and unwound); and the unchanged f3-48 basic battery on the pristine
kernel and on the fork with the live IFS, whose results must be identical.
Guest INT 13h after activation must be zero. On success it writes
`build/unified-dos/fork-qualified.json`; only then does the canonical image
builder install the fork kernel, `CIUKIDF.SYS` and the `DEVICE=` line for
`kernel=unified` (`scripts/unified_dos.py` `fork_guest_files`), which the
script also reads back. Part of `scripts/test/host_kernel_tests.sh`.
Decisions: `docs/validation/f3-57-unified-file-channel.md`.

## Unified CiukiDOS in the production VM path (f3-61)

`run_unified_file.sh` ends with the H61 production-VM boots (`production_vm`
in `unified_file.py`): the H50 hook-CPU build of `tests/host/dosvm_guest.c`
(production dosvm/BIOS/DPMI/`dos_file_vm.c`, real VFS, libx86emu) boots a
`kernel=unified` base made by the canonical `build_reference` (MBR, FAT16 at
LBA 63, `.SHA` sidecar) with `GUEST_KERNEL=unified`, i.e. `config.kernel=2`
and the v1 bridge disabled exactly as `/Programs/dosbox` creates it. It
requires the DFV2 binding and the C: IFS (no `CIUKIDF: no DFV2 host`), a
replay of the pre-f3-61 state (`GUEST_UNIFIED_UNBOUND=1`: the fork unbound)
and an unbound `freedos-1.3` base.

The same build replays a game from the canonical image's DOS bases (read
only; the harness writes its scratch overlay under `build/host/`):

```sh
# after tests/host/dos/hook_cpu.py --prepare and the run_hook_cpu.sh compile
# (add -O2 and drop the sanitizers for long replays)
GUEST_KERNEL=unified GUEST_INT21_TRACE=1 GUEST_PLAY_MS=40000 \
  build/host/dos/dosvm_hook_cpu build/f0/dos/UNIFIED.IMG \
  'shell:CD \DOOM' - 'play:DOOM.EXE -config CIUKI.CFG' DOOM
```

`GUEST_KERNEL=freedos-1.3|unified` selects the base and sidecar,
`GUEST_INT21_TRACE=1` prints every INT 21h the guest kernel serves as one
`I21` entry/return line (V86 INT 21h matched to the kernel's IRET; DOS/4GW's
0302h calls into the INT 21h vector and reflected PM INT 21h at the host's
real-mode return stub), `play:` runs `GUEST_PLAY_MS` simulated milliseconds
with one `H61 PLAY` progress line per second, and `GUEST_PLAY_QUIT=1` leaves
DOOM through its menu (Esc, Up, Enter, y) and requires the shell prompt with
no EXEC frame outstanding. Not QEMU or hardware evidence. Decisions:
`docs/validation/f3-61-unified-dos-game-watchdog.md`.

## f3-63 retirement

The canonical image and every desktop DOS launch use the qualified unified
fork. ABI kernel values 0/1 map to 2 at the window boundary. The pristine
FreeDOS comparison guest remains inside the host harness; it uses the explicit
host transport rather than a production kernel selector. The old two-kernel
`qemu_reference.py` entry point refuses execution. The classic H50 CPU boot and
its emitted-layout/idle tests are retired from the host gate; H49 file API tests
and H56/H57 unified binding/file tests remain. `hook_cpu.py --prepare` supplies
shared host preparation only and does not assemble the classic kernel.

`test_unified_only.py` tests the production payload plan with synthetic builder
outputs, without constructing an OS image. The DOS Box loading replay includes
kernel/settings CLI refusal, F12 forwarding, busy-close retention and the actual
shell-readiness handshake. Its 2505 ms first-frame model excludes CREATE and
cannot close the T23 cold-verification or post-frame CPU targets.
