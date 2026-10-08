# Framebuffer cache investigation, 3 October 2026

The owner still observes severe lag with the 15:11 image. In a capped KVM
run, 20 of 100 instruction samples landed on `framebuffer_rows`'s `rep movsd`
at runtime address F8407459. A walked Jemm PTE for the destination was
FD28D477: PCD=1, PWT=0, PAT=0, selecting PAT entry 2. Binding succeeds and
row calls return success. These observations identify transfer cost, but do
not establish the effective CPU memory type or a suitable replacement.

Sources checked before implementation:

- [Intel SDM, volume 3A, memory cache control and PAT](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf):
  effective memory type depends on MTRR and PAT, and changes require cache
  and translation synchronization. Overlapping UC and WC variable MTRRs
  resolve to UC, so adding a WC range inside the existing UC range is insufficient.
- [Linux PAT documentation](https://docs.kernel.org/arch/x86/pat.html):
  mappings of the same physical range must avoid incompatible cache types.
- [Linux device I/O documentation](https://docs.kernel.org/driver-api/device-io.html):
  write combining is useful for framebuffer writes; register MMIO has
  different ordering requirements.
- [SeaBIOS MTRR initialization](https://github.com/coreboot/seabios/blob/master/src/mtrr.c):
  firmware configures the PCI hole separately from RAM.

Decision: first record CPUID, CR0/CR4, the actual framebuffer PTE, PAT,
MTRRCAP, MTRR_DEF_TYPE and up to 32 variable MTRR base/mask pairs at bind.
`CVFBCACH` is a 576-byte resident record; unbind clears its live field.
`CVFBTIME` counts completed native row/page-copy calls and TSC cycles spent
inside their copy loops. No PAT, MTRR or PTE cache policy is changed by
these diagnostics. Read a few words of the resident records, rather than
producing unbounded traces or full guest-RAM dumps.

## Measured firmware layout and cache ownership

`lag-cache-diagnostic-2026-10-03/cache.txt` records the actual configuration:
PAT is the reset table, MTRRCAP=508h (eight variable ranges, WC supported),
MTRR_DEF_TYPE=C06h (WB default, fixed/variable ranges enabled). Variable
range 0 marks 80000000h–FFFFFFFFh UC; the other seven are disabled. BAR0
is FD000000h. The native mapping selects PAT entry 2 (UC-).

The full new-game probe reports shell motion consumption of 300–444 ms,
versus 2.6 ms before the launch and after confirmed normal teardown. It
also measures 2.389 billion TSC cycles in native copies during a 2.50-second
gameplay sampling interval. The scope permits one host CPU in this baseline;
host throttling can lengthen an individual recorded copy, so this is elapsed
copy time rather than a CPU-only microbenchmark.

The cache correction is restricted to the verified single-CPU hypervisor
profile, Bochs/QEMU PCI VGA 1234:1111 with a matching prefetchable BAR and
DISPI-reported power-of-two VRAM size of at least 16 MiB. It requires the
firmware layout above. Repartition its UC upper-half range into the aligned
UC siblings of the framebuffer plus one WC framebuffer range. For 16 MiB,
seven UC siblings plus one WC range fit exactly into eight MTRRs. Every
address outside VRAM retains its former effective memory type. Select PAT
entry 0 (verified WB) on the bound framebuffer pages, yielding WC under the
new MTRR. Unsupported layouts retain the original mapping.

The manager owns this change until unbind, restores the original variable
ranges, and drains WC writes at transfer/presenter boundaries. Updates run
with interrupts and caching disabled, with WBINVD/TLB invalidation and PGE
handling following the Intel MTRR update sequence. PAT itself is untouched.

The page-table format is Jemm's existing 32-bit non-PAE contract, including
its recursive `PAGE_MAP` window. The pre-existing binding and guest-memory
validation already depend on this layout. Before changing cache policy, the
new path verifies each owned PTE is present/writable and maps its expected
physical VRAM page, then rejects other mapped aliases of the VRAM aperture.

## Before/after runtime evidence

Both comparison runs used one QEMU/KVM CPU, a 256 MiB guest, snapshot disk,
silent AC97 output and a systemd scope capped at 768 MiB, no swap and one
host CPU. The game command was `C:\DESKTOP\TestGames\DOOM.COM -warp 1 1`;
screenshots confirm actual E1M1 gameplay. Input is injected through QEMU's
monitor as PS/2 motion. The reported interval ends when SHELL consumes that
motion, not when the physical host display presents the cursor.

| Phase | Original cache policy | Write combining |
| --- | --- | --- |
| Idle desktop, four samples | 2.59–2.62 ms | 2.57–2.66 ms |
| E1M1, four samples | 300.10–443.89 ms | 2.66–71.39 ms |
| After confirmed normal Doom exit | 2.58–2.77 ms | 2.53–2.57 ms |

The gameplay median changed from 417.30 to 6.06 ms in this pair. This is a
small sample under a host CPU quota, not a guaranteed latency bound. The
copy record changed from 2,388,993,390 TSC cycles / 1,302 calls to
3,791,004 cycles / 1,977 calls: about 1,834,864 versus 1,918 elapsed TSC
cycles per call. Different rectangle mixes and host throttling preclude
treating that ratio as a hardware-independent bandwidth benchmark.

Reports:

- `build/tests/lag-cache-diagnostic-2026-10-03/runtime/report.json`
- `build/tests/lag-write-combining-2026-10-03/runtime/report.json`

The first canonical-image repetition measured 2.64–76.10 ms in gameplay
and 2.55–2.58 ms after exit. All 12 sampled gameplay frames contained one
complete cursor at the expected coordinates. Its optional DOS-mode cycle
did not execute because the ended DOS window still owned keyboard focus;
the report correctly remains incomplete for that additional check. The
probe now explicitly focuses the desktop and requires `[DESKTOP] DOS`
before checking framebuffer release. Evidence is retained in
`build/tests/lag-final-full-2026-10-03/`.

The repeated release check with explicit desktop focus exposed a separate
keyboard-dispatch defect: `app_key` calls a module with `AX=APP_EV_KEY`; when
the module declines the key, its zero return replaces the original BIOS key
in AX. `ui_event` then checks that zero against F3/F4 and other global keys.
The [SeaBIOS keyboard implementation](https://raw.githubusercontent.com/coreboot/seabios/master/src/kbd.c)
confirms the INT 16h service returns the complete keycode in AX. Decision:
preserve that original word on the stack in `app_key` and restore it on the
unhandled path; retain the module's action only on the handled path. This
keeps focused applications' first refusal while allowing global fallbacks.
The `lag-final-release-2026-10-03` run passes gameplay/cursor/normal-exit
checks but remains incomplete for the DOS-mode cycle, which exposed this bug.

## Final shipping-image validation

`build/tests/lag-final-validated-2026-10-03/report.json` completes every
requested probe phase on the rebuilt canonical image. One 256 MiB guest
still ran alone under a 768 MiB memory cap, no swap and a 240-second deadline.
This final run used `CPUQuota=200%` so the single guest vCPU and QEMU/test
host work would not compete for one CPU's quota. It must not replace the
like-for-like 100% quota comparison above.

| Phase | Four motion-consumption samples |
| --- | --- |
| Idle | 2.55, 2.67, 2.67, 2.62 ms |
| E1M1 gameplay | 2.73, 2.61, 4.85, 2.65 ms |
| After `[DOSVM] ended` | 2.54, 2.57, 2.67, 2.59 ms |
| After console DOS and desktop restart | 2.56, 2.67, 2.65, 2.65 ms |

All 12 sampled gameplay frames contain exactly one complete arrow at the
expected coordinates. This sampling checks actual pixels; it does not prove
that every frame on a physical SDL display is flicker-free. F4 now enters
the full-screen DOS console after desktop focus, clears framebuffer live/WC
ownership from 1/1 to 0/0, and `desktop` successfully rebinds it to 1/1.
The rebind's strict original-MTRR checks also verify restoration of the
expected firmware layout before WC can be enabled again.

The canonical full build and Windows package completed sequentially in
3 GiB memory / 1 GiB swap scopes. The packager verified every ZIP member's
CRC. An additional capped read of the archive verified its image against
the embedded manifest, exact SHELL/CVSESSION contents against the build,
and absence of the excluded private guest paths. The Windows launcher was
not run on Windows. Artifact identities are retained in `artifacts.json`
and `release-verification.json` next to the runtime report:

- Full image SHA-256: `67c0eec854bb864d5261f366949fa564c7570ee50092a45c78e59470e07bee3f`.
- Windows ZIP SHA-256: `23cc798dec89922fb6461fc6de307fe2a5eca4e35e29f1a9aa3281eacf424730`.
- SHELL size: 60,912 bytes; the existing COM limit was not raised.

No simultaneous builds/QEMU runs, full guest-RAM dumps, host-wide indexing
or unbounded tracing were used. The temporary candidate image and redundant
PPMs are removed after verification; reports, PNGs, small diagnostics and
the final matching SHELL listing remain available.

## Read-only physical cache diagnostics (2026-10-08)

The Linux v6.2 Savage driver maps the framebuffer and aperture write-combined
while keeping its MMIO register mapping separate; its SuperSavage path relies
on the platform's normal MTRR setup. Intel's SDM defines the effective type as
the combination of the PTE's PAT index and the physical range's MTRR type, and
requires cache/TLB coordination when changing those attributes. Linux's PAT
documentation also warns against conflicting aliases and recommends paired
attribute changes. Sources: [Linux v6.2 `savage_bci.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/savage/savage_bci.c),
[Intel SDM volume 3A, §§4.9, 11.5.2 and 11.12.4](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf),
[Linux 6.2 PAT documentation](https://docs.kernel.org/6.2/x86/pat.html), and
[Linux 6.2 device-I/O documentation](https://docs.kernel.org/6.2/driver-api/device-io.html).

Decision: export the existing `CVFBCACH` bind-time snapshot and `CVFBTIME`
copy counters through a bounded, system-VM-only read API. The query reads
already captured records; it performs no new MSR/MMIO/PTE access and changes
no memory type. `DISPLAY.APP` persists one snapshot per process at first
properties open in `SYSTEM/VIDEO/CACHE.LOG`, outside the desktop paint path.
This provides physical evidence before considering any cache-policy change.
After a valid packet, the process attempts persistence once even when the disk
is full or read-only; a rejected write cannot create periodic diagnostic I/O.

The new `VM_OP_FB_DIAGNOSTICS` (17h) packet is 624 bytes: `CVFD`, ABI
version 0100h, total size, the 576-byte cache record size and 32-byte timing
record size, followed by those two records byte-for-byte. A short caller
buffer, a non-system VM, or an invalid destination is rejected by the JLM.
`scripts/inspect_boot_hardware.py --cache CACHE.LOG` validates and decodes
the packet. Physical log collection must retain `SYSTEM/VIDEO/CACHE.LOG` in
addition to the existing display and GPU logs.

The direct caller gate is `scripts/qemu_test_cache_diagnostics_api.py`. Its
two NASM fixture variants test short length, invalid ES:DI, adjacent canaries,
the exact valid 624-byte header and embedded record magics in the system VM,
then repeat the rejection cases in a VMFORK child. It installs only into a
private HDD copy. Assembly, Python syntax and the real QEMU protocol gate
pass, including invalid destinations, short buffers and child isolation:
[2026-10-08 report](../../build/full/t23-next/qemu-cache-api-20261008/report.json).
The source image remains unchanged.
