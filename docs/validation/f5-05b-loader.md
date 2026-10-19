# f5-05B loader/kernel host validation — 2026-10-19

The production ntdll backend uses CIUKI_SYS_W32 (96), including additive loader
and private wait-word records. The Linux i386 adapter remains independent.
Research and design decisions are recorded in the dated f5-05B amendment of
[the personality contract](../design/win32-personality-contract.md) and the
[execution ABI](../design/execution-abi.md).

## Evidence boundary

The kernel model links the actual syscall dispatcher, record validation,
VM/section/COW code, process lifecycle and shared loader/ntdll core under
ASan/UBSan. Native C accesses are mirrored into the fixture's fake physical RAM.
PE callbacks are observed, not executed. This is H0 evidence; it is not proof
of i386 instruction execution, real FS/FPU scheduling, QEMU or physical memory
use. The independent target-layout tests compile ELF/MSVC/MinGW records.

The corpus has 137 cases: **83 load successfully and 54 are refused as expected**.
All 137 also receive the expected separate structural-validation result.
The following table lists every distinct corpus load refusal, preserving part
A's names and Windows error values.

| Named refusal | Win32 error | Fixtures |
| --- | ---: | --- |
| `bound-iat-without-names` | 193 | `bound-without-names` |
| `bound-name-bounds` | 193 | `bad-bound-name` |
| `certificate-file-bounds` | 193 | `bad-certificate` |
| `delay-attributes` | 193 | `bad-delay-attributes` |
| `directory-count` | 193 | `bad-directory-count` |
| `directory-image-bounds` | 193 | `bad-directory-image-bounds` |
| `dos-header-truncated` | 193 | `truncated-dos` |
| `dos-magic` | 193 | `bad-dos-magic` |
| `entrypoint-bounds` | 193 | `bad-entrypoint` |
| `export-count` | 193 | `bad-export-count` |
| `export-ordinal-index` | 193 | `bad-export-ordinal` |
| `file-alignment` | 193 | `bad-file-alignment` |
| `headers-size` | 193 | `bad-header-size` |
| `image-address-wrap` | 193 | `bad-image-wrap` |
| `image-size` | 193 | `bad-image-limit`, `bad-image-zero` |
| `import-descriptors-unterminated` | 193 | `bad-import-terminator` |
| `import-iat-missing` | 193 | `bad-import-iat` |
| `import-ordinal-reserved-bits` | 193 | `bad-import-ordinal` |
| `import-thunk-alignment` | 193 | `bad-import-thunk` |
| `machine-not-i386` | 193 | `bad-machine` |
| `missing-dll` | 126 | `missing-dll`, `builtin-import` |
| `missing-export` | 127 | `missing-export` |
| `not-executable-image` | 193 | `bad-not-executable` |
| `optional-header-bounds` | 193 | `bad-optional-size`, `truncated-optional` |
| `optional-magic-not-pe32` | 193 | `bad-pe32-plus` |
| `pe-header-bounds` | 193 | `bad-pe-offset-bounds` |
| `pe-offset-alignment` | 193 | `bad-pe-offset-alignment` |
| `pe-signature` | 193 | `bad-pe-signature` |
| `relocation-base-collision` | 193 | `cycle-a`, `cycle-b` |
| `relocation-block-size` | 193 | `bad-reloc-size` |
| `relocation-target-bounds` | 193 | `bad-reloc-target` |
| `relocation-type` | 193 | `bad-reloc-type` |
| `resource-cycle` | 193 | `bad-resource-cycle` |
| `resource-directory-bounds` | 193 | `bad-resource-leaf` |
| `section-alignment` | 193 | `bad-section-alignment` |
| `section-count` | 193 | `bad-zero-sections`, `bad-too-many-sections` |
| `section-header-overlap` | 193 | `bad-header-overlap` |
| `section-image-bounds` | 193 | `bad-section-image-bounds` |
| `section-raw-alignment` | 193 | `bad-section-raw` |
| `section-raw-wrap` | 193 | `bad-section-raw-wrap` |
| `section-rva-alignment` | 193 | `bad-section-rva` |
| `section-virtual-overlap` | 193 | `bad-virtual-overlap` |
| `tls-callback-alignment` | 193 | `bad-tls-callback-alignment` |
| `tls-callback-bounds` | 193 | `bad-tls-callback-address` |
| `tls-index-bounds` | 193 | `bad-tls-index` |
| `tls-template-bounds` | 193 | `bad-tls-template` |
| `unsupported-subsystem` | 193 | `bad-subsystem-native`, `bad-subsystem-posix`, `bad-subsystem-ce` |

Additional existing production paths retain `builtin-manifest`,
`builtin-image-manifest`, `expected-executable`, `image-address-range`,
`image-protect-failed`, `dllmain-attach-failed` (1114), `forwarder-depth` (127),
`unload-during-initialization` (87), and bounded module/dependency/TLS/allocation
refusals. The kernel seed additionally requires `builtin-entry` and reports a
missing bootstrap as `missing-dll` (126). Kernel preparation failures use spawn
rollback; user loader refusals emit protected diagnostics and exit with the
corresponding NTSTATUS. Native spawn does not wait for user DllMain, matching the documented
[CreateProcess return boundary](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa#return-value).

## Runtime and placement checks

Tests exercise kernel compare/enqueue/wake for a contended critical section,
recursion, heap zero/grow/free, SEH dispatch/filter, RTL command-line/environment
construction, static TLS template/zero fill, independent DATAFILE views,
forced relocation/COW, production builtin import/helper resolution and
process teardown. Warning tests cap 70 stub attempts to 64 protected records
while preserving a subsequent fatal load record. The production kernel core
suite also tests plain unwind continuation and unwind from a noncontinuable
exception, and additionally retains nested
exception/context delivery, COW isolation/protection changes, allocation-failure
rollback and 100 process-lifetime cycles. The adapter retains its deeper loader,
SEH/unwind, resource, heap and search-order tests.

The image-payload unit test builds 21 original CiukiOS builtin DLLs, checks
manifest placement and refuses overlapping ranges and modified DLL hashes.
The JSON manifest's versions are read from real VERSIONINFO resources; the
binary companion must match it exactly. T1 reads both manifests and every DLL
back through the ordinary payload hash check. No image is built by these tests.
The self-authored corpus is included as test payloads; no commercial binary
or Wine implementation is added.

## Memory

Each fixture runs in a fresh production process model. Peak fake physical-page
use is **57,344–4,284,416 bytes** (56 KiB–4.086 MiB), including page tables,
section backing, COW credits and PMM-backed metadata. Small kernel-heap objects
allocated by the host malloc fixture are excluded from that byte count. The
mirror reads all readable committed pages, so this is a fixture allocation
measurement, not a measured guest working set. Per-fixture values are retained
in `build/host/win32/kernel-loader/corpus-results.json` and the implementation
report. Every model teardown returns physical pages and Win32 objects, handles,
views and COW credits to zero. The full core suite independently reconciles its
ledger and host-heap baselines.

The existing VFS executable snapshot ceiling and the 128 MiB hardware profile
are unchanged. Whole-system/game memory qualification remains with the lead.
The QEMU case `w32-fixtures-qemu-t23` is written and registered for five original
executable fixtures; it remains unrun. No QEMU, systemd scope, image build,
commit or diary edit was performed.

Final command results are recorded in `build/codex/f5-05b-report.md`.

## Completed host checks

The full host kernel script passes with ASan/UBSan: 62,535 production Win32
checks, the independent 1,371-value ABI oracle and five kernel-loader tests.
The broader Win32 discovery passes 66 tests with 10 explicit skips: six i386
execution gates denied by seccomp, two Wine socket comparisons denied by the
sandbox and two optional full Wine source/PE cache checks. The final focused
image-payload metadata/corruption test also passes. Kernel compilation and its
instruction audit pass. Final apps/builtin inventory and artifact hashes are
in the implementation report; none is image, QEMU or hardware evidence.

The separate production-store startup model peaks at 339,968 fake physical-page
bytes (83 pages, four reserved COW credits) while loading the actual ntdll and
kernel32 builtins, resolving the audited helpers and checking builtin pinning.
It returns every physical page/object/view to zero. The other 19 builtins are
manifest entries and are not resident in this workload. `make CAP= apps` and
the final 21-builtin store verification pass; the report records the final
DLL inventory, manifest checks and ELF hash.
