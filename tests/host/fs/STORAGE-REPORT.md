# f1-09 handoff — 2026-10-10

The authorized implementation is ready for the lead's diff review. Both required
host suites and the kernel build pass. **Full directive/contract acceptance is
not claimed:** the ordinary-output hook is outside the allowed files and remains
unapplied, so the guest bootlog probe explicitly fails until it is connected.
Identity binding and probe ordering also require lead integration decisions.

No QEMU, `make build-full`, systemd scope, commit, push or direct Git command was
run. The requested kernel builder reads revision/dirty metadata internally;
`GIT_OPTIONAL_LOCKS=0` was set to prevent optional Git writes. All generated
artifacts and temporary files remain under this worktree's `build/`.

## Files

New production files:

- `src/kernel/fs/mount.c`
- `src/kernel/include/ciuki/storage.h`
- `src/kernel/core/bootlog.c`
- `src/kernel/include/ciuki/bootlog.h`
- `src/kernel/probes/fat_probes.c`

Changed production files:

- `src/kernel/core/init.c`: calls storage after ATA activation/discovery.
- `src/kernel/fs/fs_port.c`, `fs_port.h`: read-only RTC provider/decoder and
  cooperative service hook.
- `src/kernel/fs/cache.c`, `cache.h`: live view-state refresh, writeback due
  metadata and cooperative servicing.
- `src/kernel/fs/fat.c`, `fat.h`: explicit RO upgrade, scan yielding and LFN
  fallback observation counters. Each integration change is explained in
  [STORAGE-VALIDATION.md](STORAGE-VALIDATION.md).

Host tests and documentation:

- New `tests/host/rtc_test.c`, `tests/host/fs/test_storage.c`,
  `tests/host/fs/STORAGE-VALIDATION.md` and this report.
- Changed `tests/host/runtime_init_test.c`, `tests/host/ata_test.c`,
  `tests/host/fs/fake.c`, `fake.h`, `kernel_compile.py`.
- Changed `scripts/test/host_kernel_tests.sh`, `scripts/test/host_fs_tests.sh`.

`src/kernel/core/output.c`, the builder/dispatcher, driver implementations,
contracts and existing f1-01 report were not edited. The requested SHA header
actually lives at `src/kernel/include/ciuki/sha256.h`, not `src/kernel/lib/sha256.h`.

## Commands and results

All three commands exited **0**:

```sh
ASAN_OPTIONS=detect_leaks=0 scripts/test/host_kernel_tests.sh
ASAN_OPTIONS=detect_leaks=0 scripts/test/host_fs_tests.sh
python3 scripts/build_kernel.py
```

The build additionally used `GIT_OPTIONAL_LOCKS=0`,
`TMPDIR=$PWD/build/host/storage`, `PYTHONDONTWRITEBYTECODE=1`; both host scripts
already set their temporary directory under `build/`. Full stdout/stderr:

- `build/host/storage/kernel-tests.log`
- `build/host/storage/fs-tests.log`
- `build/host/storage/build-kernel.log`

Selected verbatim results:

```text
runtime init/safe: PASS (0 failures, 119 records; ordering, flags, gating, fallbacks, failures)
RTC: PASS (5683 checks; BCD/binary, 12/24h, leap/pivot, UIP/seconds, timeout/wrap/stall, fallback)
storage cache/ATA: PASS (register-boundary write/flush faults, delayed EIO, sticky unmount, quarantine/no further command)
PASS kernel compile: 9 translation units; exact scripts/build_kernel.py CFLAGS
PASS kernel objects: no compiler runtime helpers
PASS storage mounts: RO-first, gate/durability, refreshed quarantine/cache hits, dirty/divergent/BPB, writer hook
PASS boot log: qualification/zero calls, 64KiB truncation ring, bounded RAM, durable reopen/append
PASS delayed write/flush errors: sticky caller result, RO revocation, no clean unmount; logger no recursion
PASS production FAT probes: FAT12/16/32 listings/hashes, 4MiB lifecycle, aliases/dir growth, cold reopen, cache, bootlog
PASS mount-crash probe: dirty fixture refusal, ARM/write cut trace, cache-loss reboot, independent copies crosslinks=0
STORAGE RESULT checks=39944 records=188 max_record=176 failures=0 ASan/UBSan=enabled
PASS crash fsck classification: cuts=2106 unknown_diagnostics=0 dirty=1111 divergent_fats=232 lost_clusters=1399 orphan_lfn=198 stale_fsinfo_hint=135 unowned_fat12_fragment=10
ARTIFACTS files=29 logical_bytes=80636477 allocated_bytes=11853824 limit=200000000
PASS host filesystem suite: 14 groups, 3 fsck checks, 3 independent mtools comparisons
[build-kernel] FPU/SIMD audit passed
[build-kernel] build/f0/VMM.ELF (1061672 bytes), build 7f3d9063afa5-dirty
```

The unchanged filesystem matrix totals **14 groups, 86,673 checks, zero
failures**, in addition to the new 39,944-check storage integration suite.
Its 2,106 crash cuts found **zero cross-links and zero corrupt owned chains**.
Three clean FAT12/16/32 runs passed `fsck.fat -n` and independent mtools
names/sizes/content comparison; the integration image also passed fsck after
restoring its baseline. Existing kernel suites (ABI, synchronization/registry,
input, framebuffer, ATA, SHA-256, V86, process and signals) also passed.
ASan/UBSan were enabled; LSan is explicitly disabled by the requested command.

ELF SHA-256 (not a boot-image/QEMU qualification):

```text
7ba00c987209a940708b6d322193b223715eb6307d2f814a747bd3473606d469
```

## Problems requiring lead action

1. **Output capture hook:** f1-09 does not allow `core/output.c` edits. Existing
   activation records/klog lines cannot reach a new sink without that hook.
   A concrete, **unapplied** proposal is in
   `build/host/storage/output-hook.patch`: append ordinary formatted output
   to RAM, never panic/raw output. The guest bootlog probe checks this boundary
   and fails EOPNOTSUPP while disconnected. Host capture is simulated explicitly;
   its success is not evidence of production wiring. The lead must authorize/
   apply/review the hook and test ordinary output versus panic before T3.
2. **Boot identity:** the frozen CBI1 handoff lacks loader sector fingerprints.
   This implementation follows f1-09's disk-0/primary-1 mapping and reports
   `qualified=0 reason=loader_fingerprints_absent`. It cannot satisfy the stronger
   f1-acceptance identity gate within this scope; no identity qualification is
   claimed. The lead must resolve this before treating normal RW mounts as
   acceptance-qualified.
3. **Log path/cap conflict:** directive `SYSTEM/BOOT.LOG`, 64 KiB implemented;
   acceptance contract `SYSTEM/LOGS/BOOT.LOG`, 128 KiB requires reconciliation.
4. **F1 aggregate ordering/lifecycle:** the builder's `f1_order` needs
   `probes_fat_probes` after ATA and before safe. The probes' individual selectors
   work, but writable probes finish with durable shutdown; same-boot `f1:all`
   additionally needs remount/orchestration between them. The builder and
   dispatcher are outside the allowed files. Existing F0/older F1 suites still
   require the lead's guest regression run.
5. **Inherited limits:** f1-01's documented torn-sector, crash-state, rename,
   split FAT12 directory tail, active-only FAT32 and workspace/COW limits remain.
   No contract was silently relaxed or rewritten. RTC uses a documented
   1980–2079 pivot because CBI1 has no century-register description.

The runner protocol (names/list digests, cold-reboot manifests, crash marker,
sector/barrier trace and absent fixtures), primary-source research, integration
notes and remaining timing/hardware limits are in
[STORAGE-VALIDATION.md](STORAGE-VALIDATION.md). The lead still needs QEMU,
fsck/mtools overlay evidence and physical qualification; none was run here.
