# F0 runner implementation and host validation

Date: 2026-10-09. Codex teammate worktree; integration and F0 gate remain with
Claude. No kernel, frozen handoff, Makefile, design contract or diary edits.

## Sources checked before implementation

- [QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html):
  file chardev, named fw_cfg item, paused start, QMP and device options.
- [QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html):
  greeting/version, capabilities, query-status, system_reset, query-blockstats,
  screendump and quit. Version is obtained from the capped, paused QEMU's QMP
  greeting; the runner never launches an unrestricted version-query process.
- [systemd resource control source](https://raw.githubusercontent.com/systemd/systemd/main/man/systemd.resource-control.xml):
  unit properties require a functioning kernel memory controller. Verify both
  MemoryMax/MemorySwapMax and memory.max/memory.swap.max before allowing exec.
- [ACPI E820](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/15_System_Address_Map_Interfaces/int-15h-e820h---query-system-address-map.html):
  20/24-byte descriptors and reserved address types.
- [VBE 3 specification](https://pdos.csail.mit.edu/6.828/2012/readings/hardware/vbe3.pdf):
  version-dependent pitch/masks, supported graphics/LFB flags and framebuffer
  capacity. Never fix a malformed firmware pitch in the reference model.

Repository comparison used Semble first, then small ranges in
`src/com/vbe_console.inc:vc_trace`, `src/boot/input_platform.inc`, and
`scripts/inspect_boot_hardware.py:parse_trace`. The archived
`scripts/read_memory_map.py` and `scripts/tests/test_inspect_boot_hardware.py`
confirmed CMAP/CVT1 offsets. Raw selected captures were extracted only under
this worktree's build directory and removed after JSON conversion. No full
archive or image was extracted.

## Commands and integration

No build is needed for these Python standard-library tools:

```sh
python3 -m unittest discover -s tests/host -v
python3 scripts/test/run.py f0-smoke
python3 scripts/test/run.py f0-core --profile qemu-min128
python3 scripts/test/run.py f0-panic --image build/full/ciukios-full.img
python3 scripts/test/run.py f0-runner --physical-capture /path/to/capture
```

Build the canonical image once using the lead's `make build-full` integration,
under the mandatory 3G/1G systemd scope. These tools never build or copy it.
`make qemu-test-full` should call the smoke command without rebuilding.

The runner resolves the shared lock through Git's absolute common directory,
then `<common-directory>/../build/test-runs/.qemu.lock`. Manual launchers must
use `scripts/test/resources.py:common_lock` / `ExclusiveLock` (or flock the same
file); do not unlink or replace that inode. Both agents' per-worktree results
remain under their own `build/test-runs`.

An unowned QEMU causes 30-second polling, at most 20 minutes, then refusal;
lock contention and an active full/CD build cause refusal. MemAvailable must
be at least 2 GiB. A gated Python wrapper starts inside a unique capped scope;
the parent verifies systemd and actual cgroup v2 files before opening the gate.
QEMU starts paused until its QMP version and block counters are recorded.
No cgroup, unrestricted fallback, build flags, RAM dumps or /tmp files are used.

COM1 uses QEMU's file backend to a local FIFO. The host collector writes the
raw serial log with a 4 MiB hard cap. QMP and stderr have separate 4 MiB caps.
The FIFO keeps an arbitrarily fast serial writer from growing a disk file
between monitor polls. The child also has a file-size limit derived from the
remaining 2 GiB budget, with 32 MiB reserved for logs/results/captures; overlay
write growth cannot consume that reserve. Logical and allocated usage are
checked before creation and during observation.

Cold boots use fresh scopes and overlays. A restart case first requires a
passing initial boot, then issues QMP system_reset and observes a fresh boot
in the same VM. Both record sets are retained in result.json. Core includes
five cold/five restart attempts for T23, E500 and minimum-128 profiles, followed
by the nonfatal probes in contract order. Each failing prerequisite stops the
suite and lists later cases as not_run in summary.json. The summary checkpoints
each individual result's essential textual evidence, including observations and
image identity, before the next run can prune its directory; this preserves the
ten-boot matrix with only five run directories. Panic uses fresh boots.

Panic compares the terminal error/EIP/CR2 with ARM, checks five seconds of
silence, and compares block write/flush counters. Read/write/flush counters are sampled on ARM receipt and at the end of
observation. An additional write/flush baseline is taken before CPU start,
which is stricter and avoids losing a write between guest emission and host
receipt. Read counters use the ARM-receipt baseline because firmware disk reads
necessarily occur during boot; observing reads between guest ARM emission and
host receipt would require a synchronized guest/host arm handshake. A production F0 probe that
legitimately writes before ARM would need a synchronized arm handshake before
this stricter check could be relaxed. Teardown requests QMP quit, waits two
seconds, terminates the owned scope, waits two seconds, kills remaining owned
children and verifies teardown before releasing the lock.

Pass retains result.json and serial.log only; failures retain bounded evidence.
--keep records the request but does not override the contract's mandatory
passing-overlay removal or last-five-run retention.

## Evidence field mapping

The contract fixes envelope grammar and acceptance semantics, but does not
freeze DATA field names or grouping. The JSON suites declare an explicit
mapping for the lead to implement/review. Each DATA predicate names its fields
and optionally `group` or `case`. Counters are decimal, addresses are fixed
width hex without 0x; syscall error results are signed decimal (-14 = EFAULT).
Numeric operators are eq/ne/ge/le/mask/clear; `$field` references a same-group
measurement. Summary predicates use combine=true, so bounded records can split
large summaries; repeated summary fields must agree. Raw entries and cycle
ledgers are evaluated individually. `relations` permit a checked multiplier (twice_quantum must equal
2 * quantum_ticks), and unique/count checks require 100 distinct syslife
cycle ledgers. A later good value cannot erase an earlier matching violation.

The parser rejects duplicate keys, bad ASCII/framing, oversized records,
version/run/probe mismatches, nonincreasing sequences, missing BEGIN/terminal,
and records after a terminal. Any output after PANIC fails the silence check.
The host never repairs raw F0 markers. A screen transcription import is evidence
only with operator confirmation and matching run/build/write/readback hashes.

Build manifests are discovered as build-manifest.json or manifest.json next to
the selected image. Manifest keys accepted are git_revision/git_dirty (or
revision/dirty). Missing information is recorded as unknown; it does not
establish build identity. The runner records its own Git revision/dirty flag,
externally computed image hash/size and post-run image hash separately.

Physical import reads only acquisition.json and f0.log, each <=128 KiB. Supply
operator_confirmed=true, selector, selector_source=menu or serial, build_id,
and all physical identity fields in scripts/test/physical.py:FIELDS. Unknown
inventory fields must explicitly say unknown. Write/readback hashes must match
the canonical image; records must carry the same build_id. Panic additionally
requires external_halt_seconds>=5 and resumed=false. Disk logging defaults to
unavailable and cannot be asserted without storage_qualified=true. Imports do
not mount disks or perform writes. Importing captures is not a ten-boot T4 gate.

## Hardware replay

The two replay.json files preserve raw E820 entries, decoded VBE controller and
mode fields, PCI identities, EDID validity and provenance hashes. T23 has one
E820 map (130,656 candidate pages) and two distinct video data sets. E500 has
one map (65,264 candidate pages) and three distinct video/controller data sets.
These are firmware candidate pages above 1 MiB, before kernel reservations.
Every raw-file hash and capture name is documented in its machine README.
Identical payloads are coalesced; trace-header differences are historical
metadata and never repair eligibility. T23 EDID is absent; E500 blocks are
invalid, rather than invented valid EDID or assumed absence.

The overlap model preserves a single nonusable type and canonicalizes conflicts
between nonusable types to reserved type 2. Unknown types are never allocatable.
The contract does not specify a numeric precedence between ACPI reclaim, NVS,
reserved and unknown types; treating conflicts as generic reserved is a
conservative model choice for lead review. Merge equal-type spans before inward
rounding, so unaligned usable overlaps do not discard complete pages.

## Validation scope and remaining integration

Host validation uses scripted executables and real child processes; it runs no
real QEMU. The sandbox denies Unix socket creation, so fake QMP uses local FIFOs
through the same bounded JSON client. Production retains the required Unix
socket in the run directory. The synthetic Git test resolves two real linked
worktree metadata trees to the same path/inode and proves lock contention.
It creates no branch or commit in this repository.

No canonical F0 image exists here. Actual systemd enforcement, production QMP
socket, loader/model equality, full-image boot and T4 are not_run. Missing
image or identity evidence is never relabelled as F0 acceptance.

Contract/integration gaps to report to the lead:

1. Safe mode is required, but the selector has no safe-mode option and the boot
   menu's key/UI protocol is not frozen. The declared safe-mode case refuses
   before launching, rather than changing BOOT.CFG or inventing a selector key.
   Agree on menu automation or an additional runtime configuration interface.
2. UART-absent panic captures a QMP screen image and fails pending externally
   verified screen records. A bitmap is not parsed text evidence. The kernel
   screen renderer/font and paging protocol are not available to this worktree;
   no OCR success is claimed. Operator-confirmed physical screen transcriptions
   can already be imported with the bounded evidence importer.
3. The DATA field mapping above needs kernel/lead alignment; frozen contracts
   do not assign those field names. Missing fields deliberately fail predicates.
4. The build manifest's final filename/keys and embedded build identity need
   integration with the lead's canonical builder. Unknown identity is recorded
   explicitly, and does not close the corresponding F0 evidence requirement.
5. The reference overlap tie rule above needs lead confirmation. No frozen
   boot_info.h or boot_info.inc defect was found in the read layouts.
6. bootinfo requires raw firmware E820 evidence, while the frozen handoff stores
   only the normalized map. The lead must coordinate loader-side raw evidence
   with kernel-side envelope/sequence ownership; the original map cannot be
   reconstructed after overlap resolution and rounding.

The lead owns the Italian diary entry and independent review before integration.

## Results actually obtained

- `python3 -m unittest discover -s tests/host -v`: **30 tests, PASS**
  (final run; runner/parser/resource/physical-import and loader replay checks).
- `python3 -m py_compile scripts/test/*.py tests/host/*.py`: **PASS**.
- `git diff --check`: **PASS**; all deliverables are new uncommitted files.
- `python3 scripts/test/run.py f0-smoke`: **expected refusal**, exit 2,
  `canonical image missing; QEMU/physical runner qualification not_run`.
- `pgrep -c qemu-system`: **0** after tests; no real QEMU was launched.

Scratch extraction scripts, raw captures, fake images/overlays/logs, temporary
worktree metadata and Python bytecode caches were removed after validation.
No branches, commits, pushes, full-image builds or Windows runtime tests occurred.
