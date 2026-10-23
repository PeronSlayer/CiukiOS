# Build and run (Ciuki VMM, F0)

The canonical image is the 512 MiB FAT32 HDD image `build/f0/ciukios.img`
built from the Ciuki VMM kernel (`src/kernel/`) and the F0 loader
(`src/boot/mbr.asm`, `src/boot/ciukldr.asm`). The 0.8 image is built only on
branch `legacy-0.8` (`docs/design/foundations-transition.md`).

## Dependencies (Linux host)

| Purpose | Tools |
| --- | --- |
| Kernel and loader | `clang`, `ld.lld`, `llvm-objdump`, `nasm` (majors pinned in `config/toolchain.json`) |
| Image | `python3`, `mkfs.fat`, `fsck.fat`, `mtools`, `qemu-img` |
| DOS image DPMI clients (f3-04) | Open Watcom 2.0 under `/opt/watcom` (`binl64/wcl386`, `binw/dos4gw.exe` with the SHA-256 pinned in `scripts/build_dos_image.py`) |
| Network stack | lwIP 2.2.1 (archive size/hash recorded in [`src/kernel/net/lwip/import.json`](../src/kernel/net/lwip/import.json); offline build, no fallback) |
| Kernel entropy | jitterentropy-library 3.7.0 (archive size/hash and imported units in [`config/jitterentropy.json`](../config/jitterentropy.json); offline extraction under `build/external/`, no fallback) |
| NetSurf browser | NetSurf 3.11, zlib 1.3.1 and libpng 1.6.59 (archive hashes in `config/netsurf.json`; offline build, no fallback) |
| Windows installer fixtures | NSIS 2.51 source and binary archives (hashes in `config/nsis.json`; host fixture tooling, not a guest installer dependency) |
| Optional host guest runner | `libx86emu` (headers and library); skipped when absent |
| Tests | `qemu-system-i386`, `systemd-run` (user scopes with a memory controller) |
| Optical fixtures and independent extraction | `xorriso` |

## Commands

```bash
make build-full        # kernel (build/f0/VMM.ELF), apps, the cached upper Wine DLLs, then the image, with T1 checks and build-manifest.json
make upper             # only the reused Wine DLLs (cached; needs make apps first)
make test-host         # T0: kernel library, loader statics, runner, fixtures, loader model
python3 scripts/loader_budget.py   # loader bytes per section and free bytes in its fixed 48-sector extent
make qemu-test-full    # T2: f0-smoke through scripts/test/run.py
python3 scripts/test/run.py f0-core    # T3: ten-boot matrix and the nine non-fatal probes
python3 scripts/test/run.py f0-panic   # T3: destructive panic cases
make qemu-run-full     # interactive boot (scripts/run_f0.sh "f0:boot run=00000001" to pass a request)
```

`scripts/test/run.py` takes the first free runner slot (`.qemu.slot0`/`.slot1`;
`CIUKI_RUNNER_SLOTS`, default 2, max 4, so two suites may run at once), refuses
when all slots are busy, when a manual QEMU runs outside the runner scopes, or
when memory is short (2 GiB plus 2 GiB per other running slot), runs each
case under `systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0`
on a qcow2 overlay in `build/test-runs/<suite>/<run-id>/`, and writes
`result.json` plus the raw serial log. Profiles are in `tests/profiles/`,
suites in `tests/suites/` (`docs/design/test-architecture.md`,
`docs/design/f0-acceptance.md`).

Interactive runs use TCG by default; KVM is only for the `qemu-fast`
developer profile and never for evidence.

The general QEMU smoke and suite tiers are suspended for the current hardware
trial cycle (owner decisions 2026-10-19 and 2026-10-20): a merge needs green
host suites and a canonical `make build-full` of the merged tree; the resulting
image goes to hardware with its SHA-256, serial capture and disk logs as
evidence. Authorized QEMU uses are the DOS conformance battery of f3-48
(`f3:dos-conformance`), F6 network cases, F7 3D API development/tests and the
short boot smoke before a hardware image. Other QEMU evidence resumes on the
owner's request.

### Network QEMU cases (F6)

The `f6-net` runner suite selects one virtual NIC through the case's
`device_exceptions.network` value. For the user-mode backend, the runner adds
`-netdev user,id=ciuknet0,restrict=on` and the selected device with
`netdev=ciuknet0,mac=52:54:00:12:34:56,romfile=`. The user backend is
restricted and has no host port forwards. NIC variants are declared in
`tests/suites/f6-net.json`; the runner owns the QEMU arguments. These cases
currently exercise network-driver/ARP behavior, not browser site acceptance.
Network suites remain subject to the current QEMU authorization policy above.

The kernel's native networking uses the pinned lwIP 2.2.1 stack, DHCP for
primary-interface IPv4 configuration, DNS resolution and the socket API.
The public socket syscalls are implemented by CiukiOS; the [developer API
reference](developer-api.md) lists their numbers, records and limits. lwIP's
upstream BSD socket adapter is not used. The NetSurf M1 browser is currently
limited to `file:`
and `data:` content; HTTP/HTTPS fetching is a later browser milestone.

The NetSurf 3.11, zlib 1.3.1 and libpng 1.6.59 archive SHA-256 pins are in
[`config/netsurf.json`](../config/netsurf.json); the apps build verifies them
before extraction. The lwIP 2.2.1 archive hash and imported-file inventory are
in `src/kernel/net/lwip/import.json`. NSIS 2.51 source and binary pins are in
[`config/nsis.json`](../config/nsis.json), for host-side installer fixtures.
No normal build downloads these inputs or falls back to an unpinned version.

### Reused Wine DLLs (f5-20D)

`make build-full` runs `make upper` between the apps and the image
(`kernel -> apps -> upper -> image`). The step is
`python3 scripts/win32/wine_upper.py cached-build`; `make apps` does not
depend on it. It keeps the sixteen reused Wine 10.0 PEs (the qualified entries
of `config/wine-upper.json`) in `build/win32/upper/pe`, which `make clean`
leaves alone, and `scripts/build_image.py` stages them in the builtin store
(they replace the census placeholders of comctl32, comdlg32, ole32 and
shell32).

- The PEs are rebuilt (allow about 1–6 minutes, `scripts/win32/build-wine-dlls.sh
  --upper-build`) only when the cache key changes: the SHA-256 of the pinned
  archive `build/downloads/wine-10.0.tar.xz`, or the patch set and recipe
  (`third_party/wine/patches`, the module list, the compiler flags). The
  reason is logged as `[upper] <reason>: rebuilding ...`. The build never
  downloads: with no cache and no archive it stops and says so. A rebuilt PE
  that differs from the SHA-256 in the record is refused.
- The import audit against the core DLLs reruns only when the core's export
  surface (names, ordinals, forwarders, W21 stub status) changes. A core
  rebuild that only moves PE bytes refreshes `core_pe_sha256` in
  `config/wine-upper.json` by itself; commit that file with the change that
  caused it. A semantic difference (an export gone or new, a decision row
  that changed) fails the build with the list of differences; after review,
  `python3 scripts/win32/wine_upper.py refresh-audit` rewrites the
  audit-derived parts of the record and its generated tables.
- `python3 scripts/win32/wine_upper.py check` validates the record and, when
  the built core and the cache are present, does the same audit and refresh.
- A warm `make upper` costs about 0.2 s.

The guest case `w32-upper-dlls-qemu-t23` (`f5:w32-upper-dlls`) loads the staged
set in one Win32 process (`/System/Win32/Fixtures/upper-exercise.exe`, source
`src/probes/win32_upper`); its host dry-run on the H1 adapter is in
`tests/host/win32/wine/test_wine_upper.py` (`FixtureTests`).

### Normal boot cases in f0-smoke

What a user boot shows on screen (quiet console until the desktop) and which
boots keep the verbose console: [boot modes](boot.md).

After its two probe boots, `f0-smoke` boots the image twice the way a user
does (`normal-boot-qemu-t23`, `normal-boot-qemu-desktop-2002`): QEMU gets no
fw_cfg test request, the loader menu times out (about 3 s) and the kernel
takes the production path up to `/Programs/desktop`. A case passes when
`serial.log` shows, in order and at the start of a line, `L:CPU signature=`,
`L:SELECT_SOURCE=menu`, the `Ciuki VMM F0 build` banner,
`[supervisor] bootstrap: spawning /Programs/desktop` and
`[supervisor] desktop pid=<n> pgid=<n>` within 300 s, with no panic,
CIUKI_TEST record, `desktop unavailable` or `desktop stopped` line, and the
screendump taken 8 s after the desktop line shows the bottom bar, the Ciuki
orb and the top bar, and a work area free of the kernel console colours
(the default wallpaper is a photograph, so no background colour is required).
The run directory keeps `screen.ppm` also on a pass; `result.json` has
`evidence_kind: "normal-boot"`, `normal_boot` (milestones, desktop PID,
timings) and `screen` (PPM SHA-256, measured colour fractions). A failure
keeps the overlay, `screen.ppm` when taken and `screen-fail.ppm`. Because
`f0-smoke` is the first prerequisite of the other suites, a failed normal
boot stops the campaign there. The case kind and its rules are in
[test architecture](design/test-architecture.md#normal-boot-cases-amendment-2026-10-11).

## Version

`config/version.json` is the single source of the CiukiOS version
(`1.0.0`, stage `alpha`). `scripts/ciuki_version.py` validates it and:

- `scripts/build_kernel.py` passes `-DCIUKI_VERSION="1.0.0 alpha"` to every
  kernel object and records `version` in `build/f0/build-clock.json`. The kernel
  logs `CiukiOS <version>` on the line after the `Ciuki VMM F0 build <id> - CiukiOS`
  banner (that banner is parsed by `scripts/test/physical.py` and is unchanged)
  and `uname` returns it in `version` (`release` stays the build ID); the desktop
  About window shows it as `Version <version>` (header of the Credits tab and
  the General tab).
- `scripts/build_image.py` refuses a kernel built with another version, passes
  `-dCIUKI_VERSION='...'` to the loader (menu title `CiukiOS <version>: N Normal /
  ...`) and writes `version` in `build/f0/build-manifest.json` (schema_version
  stays 1).
- `VERSION`, `CHANGELOG.md` and `README.md` are written by hand;
  `tests/host/test_version.py` checks that they agree with the file, and that
  `docs/release-notes-1.0-alpha.md` and `docs/credits.md` carry the current
  version and identity.

Bump the version by editing `config/version.json` and those three files, then
`make build-full`. Publication of `main` prereleases stays governed by
`config/release-policy.json`.

## Boot tokens, the splash bar and the 2D indicator

The image's `BOOT.CFG` is a line of exact tokens that the loader copies for the
kernel (default `safe=0 serial=1`). The ones a hardware trial uses
(details in [boot modes](boot.md)):

| Token | Effect |
| --- | --- |
| `serial=1` | kernel log on COM1 as well as on the disk (`C:\SYSTEM\LOGS`) |
| `safe=1` | safe mode (no optional drivers) |
| `usbscan=0` | keeps the first USB enumeration, stops the periodic rescans (f1-87 diagnostic) |
| `ecpoll=1` / `ecpoll=0` | ThinkPad EC volume poll: off by default since f1-100 (each EC transaction costs the T23 two 2.1 ms SMIs); `ecpoll=1` restores the 1 Hz poll for diagnosis, `ecpoll=0` keeps it off and wins. The disk log counts the transactions (`[ec] thinkpad io= transactions=`) and `[kernel] smi` shows the blackouts. A planned change (f2-96) polls the EC every 2 s so the hardware volume keys reach the mixer |
| `accel=0` | disables native 2D drivers (`reason=boot_disabled`) |

The splash screen shows the original Ciuki photograph with a progress bar
under it. Since f0-55b the bar starts with the sweeping segment centred on the
first frame, the phases reported before the first paint are applied, and the
console worker is woken by its own 40 ms timer (priority `SCHED_FOREGROUND + 2`
while the splash is up); it appears as at most 14 serial records
`[console] splash phase=<n> permille=<p>`. Under QEMU TCG the phase reports
still arrive in a burst, so judge the bar on the T23 or by the record count
(`handover updates=15 claims=17` on the third image).

Under the bar the splash prints the 2D engine state (f1-97, f2-92):
`2D engine: NO (<reason>)` when the accelerator was refused, `2D engine: OK
(drawing: software)` while the engine is ready but has not drawn yet and
`2D acceleration: YES` after it has drawn a presenter request. The tray
notice and the log say the same: `[accel] summary accelerated=<0|1> driver=
reason= used=<0|1>` and `[desktop] accel fills= copies= writes= fallback=
frames= lfb_bytes=`. What to read after a T23 trial (dev diary 2026-10-22-06):
`[accel] summary ... used=1` with `[desktop] accel copies>0`, `[net] dhcp
state=bound ip=...`, `[usb-storage] bind ... max_sectors=128`, `DFV2 IFS
active` with a DOS game and `[sched] desktop_share` during the game.

## NTFS host fixtures (f4-03)

Install ntfs-3g's `mkntfs`, `ntfscp`, `ntfsls`, `ntfscat`, `ntfstruncate`
and `ntfsinfo`, plus its shared `libntfs-3g`. No root privileges or filesystem
mounts are required. The library is used only by the generator for mkdir,
unlink, interleaved writes and DOS aliases, which the installed CLI lacks;
it is never linked into the Ciuki kernel.

```bash
python3 scripts/test/ntfs_fixtures.py --out build/host/ntfs
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -p test_ntfs_ro.py -v
```

The test creates fixtures when the generator changes; cached inputs must match
their SHA-256 manifest. It compiles the production parser for the host and
runs a separate ASan/UBSan walk. Listings and streamed file hashes come from
independent ntfs-3g tools, including the >4-GiB file (no whole-file allocation).
A second generation verifies deterministic image hashes and is deleted.
`--entries` accepts 1000–10000; the canonical host set uses 2048 entries in
one directory. Images, temporary inputs, binaries and manifests stay under
`build/host/ntfs/`; the generator refuses output outside a build subdirectory.
Delete that directory when its investigation closes; tests regenerate it.

These are generated nonboot data volumes, separate from the canonical Ciuki
image. Bounds, refusal reasons, license references and qualification limits
are in [F4 acceptance, f4-03](design/f4-acceptance.md#f4-03).

### NTFS through the VFS (f4-04)

The generator also builds the `names` (names the VFS folds differently or
cannot hold, a DOS alias for a non-BMP name) and `attributes` (hidden, system
and read-only bits) kinds, and accepts a subset for QEMU cases:
`build(out, only={"clean"})` makes just that kind and the base it derives from.
`disk_image()` wraps one volume, byte for byte, in an MBR disk with a single
type 07h partition at LBA 2048 (the volume is not patched; its own
hidden-sectors field stays 0 and the driver ignores it).

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ntfs_vfs tests.host.test_ntfs_runner -v
bash scripts/test/host_fs_tests.sh          # includes the cache layout test
bash scripts/test/host_kernel_tests.sh      # includes the pmm_alloc_contig test
```

`test_ntfs_vfs` compiles `mount.c`, the VFS, `ntfs_fs.c` and `ntfs_ro.c` under
ASan/UBSan and drives the disk through `storage_add_disk`: trees and hashes
against the ntfsls/ntfscat manifest, refusals, unsupported content, large
directory bound, unmount/remount/stale handles, and failure injection at every
device read and at the workspace allocation. `test_ntfs_runner` covers IDE slot
allocation, QEMU drive arguments, the evidence checker and the f4-media
predicates without QEMU.

`python3 scripts/test/run.py f4-media` (the lead, after the canonical build)
includes twelve `ntfs-*` cases: `clean`, `fragmented`, `names`, `compressed`,
`dirty` and `hibernated` on `qemu-t23` and `qemu-desktop-2002`. A case declares
`{"generator":"mkntfs","kind":<kind>,"seed":1}`; the runner generates that
kind, wraps it in the MBR disk and attaches it as a raw IDE disk named
`ciuki-ntfs` after any FAT fixtures (`mkfs.fat` peers take the lower indices;
one NTFS disk per case, three disks in all, at most two with a CD). The runner
refuses a changed fixture hash and requires QMP block statistics that show no
write or flush on `ciuki-ntfs` during the run. `f4:ntfs-ro run=<8 hex>` is the
selector; with no NTFS partition mounted it records
`not_run reason=fixtures_absent`.

### F4 optical fixtures

`scripts/test/iso_fixtures.py` generates deterministic data CDs, without Rock
Ridge, at ISO 9660 Levels 1–3 with optional Joliet. It fixes timestamps and
ownership, declares source hashes and extracts the resulting image with
`xorriso -osirrox` to obtain independent on-media names, sizes and SHA-256
hashes. Source/extraction scratch trees are removed before it returns. For a
host-only fixture or deliberately malformed root extent:

```bash
python3 scripts/test/iso_fixtures.py --out build/host/optical --level 3 --joliet
python3 scripts/test/iso_fixtures.py --out build/host/optical --level 3 --malformed
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -p test_optical.py -v
```

The fixture image and its manifest remain in the requested build directory.
The production parser/VFS host harness checks all six Level/Joliet combinations,
synthetic multi-extent records, malformed descriptors/extents, stale mount
identities and zero block writes/flushes. Native file and mocked ATA packet
checks are also part of `make test-host`.

The lead runs `python3 scripts/test/run.py f4-media` after the canonical build.
Its eight cases cover lawful Levels 1–3 and malformed media on `qemu-t23` and
`qemu-desktop-2002`. A CD occupies IDE index 3 (secondary slave); at most two
FAT fixtures occupy indices 1 and 2, leaving index 2 as the same-channel ATA
peer. Colliding fixtures are refused. `f4:optical run=<8 hex>` is the runtime
selector, including the loader menu. At the probe's `media_reinsert` ARM the
runner issues QMP eject/change for `ciuki-optical` using the generated image.
It compares both directory/hash cycles against the independent manifest and
checks the fixture image hash is unchanged. Passing run artifacts follow the
runner's ordinary cleanup rules. A missing packet device records
`not_run reason=device_absent`; this does not qualify an optical case.

### Local DOS game payloads (f3-06)

The F3 workload suite (`tests/suites/f3-workloads.json`) runs the owner's own
copies of DOS DOOM and Wolfenstein 3-D. They are commercial files and stay
local: the directories `third_party/Doom/` and `third_party/WOLF3D/` are
ignored by git and never enter `build/f0/ciukios.img`, `UNIFIED.IMG` or a
release (`scripts/build_dos_image.py` and `scripts/build_image.py` refuse a
payload whose name or SHA-256 matches a file there). To declare them, copy
the game files into these directories:

| Workload | Directory | Required files (top level, 8.3 names) |
| --- | --- | --- |
| DOOM (DOS, v1.9 tested) | `third_party/Doom/` | `DOOM.EXE`, `DOOM.WAD` or `DOOM1.WAD` |
| Wolfenstein 3-D (WL6) | `third_party/WOLF3D/` | `WOLF3D.EXE`, `VSWAP.WL6`, `VGAGRAPH.WL6`, `GAMEMAPS.WL6` (and the other `.WL6` files) |

For a case that declares one, the runner calls `scripts/test/dos_fixtures.py`
inside that run's directory under `build/test-runs/`: it builds a private
DOS VM image with the `UNIFIED.IMG` geometry (the DOS system read back from
`build/f0/dos/UNIFIED.IMG` and checked against its manifest, plus the game
directory as `\DOOM` or `\WOLF3D` and a generated `\DOOM\CIUKI.CFG`), puts it
as `\CIUKIGAM.IMG` on a sparse 160 MiB FAT16 superfloppy and attaches that disk
as the next IDE index. Only top-level regular files with plain 8.3 names
(letters, digits, `_`, `-`) are copied; the others are listed as skipped in
the manifest. Every file is hashed (`redistribution=local-only`, the config
`free`); the probe hashes the same files on the VM's disk and the runner
compares the two. With a directory or required file missing, the case is
`not_run reason=fixtures_absent` and QEMU is not started. A host-only build of
the fixture, for inspection:

```bash
python3 scripts/test/dos_fixtures.py doom build/host/f306-doom
python3 scripts/test/dos_fixtures.py wolf3d build/host/f306-wolf
```

Delete `build/host/f306-*` afterwards (each holds a 160 MiB sparse image).
For an interactive DOOM window with the owner's registered data, attach that
`fixture-0.img` as a further IDE disk of a manual QEMU run (or copy its
`CIUKIGAM.IMG` to the root of another FAT drive on a test PC): a DOS window
(`/Programs/dosbox`) then boots the workload image, with the kernel DPMI host,
instead of the bundled shareware games disk below; type
`\RUNREC.COM \DOOM DOOM.EXE` or `CD \DOOM` and `DOOM`.
The probes are `f3:dos4gw`, `f3:doom`, `f3:doom-sound`, `f3:wolf3d` and
`f3:vm-starve` (also in the loader menu and the F3 sweep, where the game
probes end `NOT_RUN reason=fixtures_absent` without the fixture disk).

### Pre-installed shareware games

Owner decisions 2026-10-12/14: Wolfenstein 3-D, DOOM and Quake are pre-installed in
their shareware releases, the only ones whose packages may be passed on
(the registered data above stays local). The original archives are vendored
under `third_party/games/` with their licence documents and an
`UPSTREAM.json` that pins the archive (size, SHA-256, SHA-1, MD5, two
published references) and every installed member:

| Package | Archive | SHA-256 | Installed as |
| --- | --- | --- | --- |
| `wolf3d-shareware` | `1wolf14.zip` (Wolfenstein 3-D 1.4, episode 1) | `cb2a2ef7…59451a8` | `\WOLF3D` (all 12 files of `W3DSW14.SHR`) |
| `doom-shareware` | `doom19s.zip` (DOOM 1.9, episode 1) | `cacf0142…1b486e6` | `\DOOM` (16 files of `DOOMS_19.1`+`.2`, without the DOOM FAQ) plus `CIUKI.CFG` |
| `quake-shareware` | `quake106.zip` (Quake 1.06, episode 1) | `ec6c9d34…7553239` | `\QUAKE` and `\QUAKE\ID1` (all 16 original members) |

The image build never downloads: `scripts/build_image.py` calls
`scripts/build_games_disk.py`, which checks each archive against its pins,
unpacks it (ZIP; Apogee's `.SHR` container with PKWARE DCL "implode"
members, decoded by a port of zlib's `blast.c`; Quake's LHA self-extractor
with the pure-Python `-lh5-` decoder and CRC-16 verification), checks every member,
refuses registered data names (`DOOM.WAD`, `*.WL6`, …) and any other
match with the owner's local files that is not a pinned shareware file, and
installs the games inside `build/f0/dos/UNIFIED.IMG`. This is the sole DOS
base in the canonical image, at `/system/dos/UNIFIED.IMG`, with a partitioned
FAT16 guest and the qualified fork/CIUKIDF.SYS. Trailing unused zero sectors
are cut; the VMM exposes the existing 128 MiB geometry with zeros beyond the
stored prefix. Licence documents and `UPSTREAM.json` files remain
at `/system/licenses/<package>-<file>`. To build or inspect it alone:

```bash
python3 scripts/build_dos_image.py
python3 scripts/build_games_disk.py
mdir -/ -i build/f0/games/CIUKIGAM.IMG ::/
```

Every DOS window (`/Programs/dosbox`) boots this disk with the kernel DPMI host,
unless the owner's local fixture disk above is attached (it takes
precedence); the f3-06 workload probes use only the local fixture disk.
The F3-18 Quake probe uses the bundled disk directly.
The Ciuki menu's Programs > Games lists "Wolfenstein 3D (shareware)" and
"DOOM (shareware)" and "Quake (shareware)": each opens a DOS window titled
after the game, which
types `CD \WOLF3D` and `WOLF3D.EXE` (or `CD \DOOM` and
`DOOM.EXE -config CIUKI.CFG`, or `CD \QUAKE` and `QUAKE.EXE`) at the DOS
prompt once the window has keyboard focus. The same works from Run…: `dosbox \WOLF3D\WOLF3D.EXE`
(`dosbox [--name=TITLE] [COMMAND...]`; letters, digits and `\ . - _ : / = ,`
and spaces). Licence status, including the Wolfenstein 3-D bundling clause
still open with the owner: `docs/design/f3-acceptance.md`, f3-06 amendment.

Quake's original `SLICNSE.TXT` and `LICINFO.TXT` also ship under
`/system/licenses/quake-shareware-*`. The native DPMI server and existing
24 MiB shared pool exceed the game's 8 MB minimum/16 MB recommendation.
Use default VGA 320x200, INT 33h mouse and SB16 `A220 I7 D1 H5 T6`; higher
VBE modes and CD audio remain unavailable to the guest. Research and
requirements: [Quake shareware design](design/quake-shareware.md).

The lead builds once and runs the T23 workload profile, including
`quake-qemu-t23` (or selects that case through the runner's `run_case` API):

```bash
nice -n 19 python3 scripts/test/run.py f3-workloads --profile qemu-t23
```

This case needs no local commercial fixture. It pins all 16 members and
requires changing game frames, native DPMI, 16-bit stereo auto-init DMA5
PCM, keyboard-driven console quit, RUNREC exit and resource cleanup. The
owner must still verify visible gameplay, keyboard/mouse, audible output,
window responsiveness and repeat launch/exit on the T23.

## Test requests

The loader reads the QEMU fw_cfg item `opt/it.alcybercloud.ciukios/test`
(`-fw_cfg name=opt/it.alcybercloud.ciukios/test,string="f0:all run=0000abcd"`).
Grammar: `f0:<probe|all> run=<8 hex> [platform=e500]`; `platform=e500` is
accepted only from fw_cfg and forces the firmware-first input policy. On a
physical PC the loader menu (`P`) accepts the same request. Without a request
the kernel boots to its scaffold prompt on COM1 (38400 8N1) and the screen.

## Physical machines

Write `build/f0/ciukios.img` to an expendable disk with
`sudo scripts/test/write_physical.sh /dev/disk/by-id/<stable-name> [image]`:
it refuses the root disk, partitions and mounted disks, asks for the disk's
exact serial number, writes with `dd`, reads the image back and compares
SHA-256, and leaves a JSON record beside the image. The evidence rules are
in `docs/design/f0-acceptance.md`. For a screen-only run choose `P` in the
loader menu and type `f0:core run=<8 hex>`: every probe but `panic` runs,
then the screen pages through the recorded records (8 s per page) for
photographs. With an RS-232 to USB adapter and a null-modem cable on the
laptop's serial port, `scripts/test/serial_capture.sh <run-id>` records
COM1 (38400 8N1) into `legacy/local/physical/<run-id>/serial.log`; start
it before power-on, then `f0:all run=<run-id>` also covers `panic`.

A user boot (no test request) is what the image's default `BOOT.CFG`
(`safe=0 serial=1`, `DEFAULT_CONFIG` in `scripts/build_image.py`) gives: the
quiet boot of [boot.md](boot.md) to the desktop, with the log on COM1 and in
`C:\SYSTEM\LOGS`. The builder writes `build-manifest.json` beside the image;
a physical run is evidence only for the SHA-256 recorded there, which goes in
the validation record of that test
(`docs/validation/2026-10-11-integrated-image-f0-regression.md`).

### Installed kernel and the complete ELF (fx-6)

`build/f0/VMM.ELF` is the complete kernel (DWARF included, about 15.7 MB) and
stays the file for `addr2line`/`llvm-symbolizer` and the probe-table readers of
the runner tests. The image installs a different file as `/system/VMM.ELF`:
the same ELF through `llvm-objcopy --strip-debug` (about 4.0 MB), made in the
build scratch directory, byte-identical for the same input. Entry point,
program headers, loaded segments and `.symtab`/`.strtab` are unchanged;
nothing at run time reads the debug sections (the kernel does not symbolise
itself; `cdview_verify_volume` compares build-identity bytes through the
PT_LOAD headers, which are the same in both files). `build_image.py` checks
the result against the complete kernel and fails the build when the installed
file is above 75 % of the loader's limit
(`loader_kernel_limit()` reads `0x1000000` from `src/boot/ciukldr/elf.inc`).

`build-manifest.json` fields (schema_version stays 1):

| Field | Value |
| --- | --- |
| `kernel` | path of the complete ELF |
| `kernel_sha256`, `kernel_installed_bytes` | SHA-256 and size of the installed `/system/VMM.ELF` (equal to its `payloads` record; the file the CD and the Hardware Report quote) |
| `kernel_full_sha256`, `kernel_full_bytes` | SHA-256 and size of the complete ELF (the value `build-clock.json:kernel_sha256` binds) |
| `kernel_loader_limit` | the loader's kernel file limit in bytes |

`/System/BUILD.TXT` carries `kernel_elf_sha256=` (installed kernel, the one on
the disk) and `kernel_full_elf_sha256=` (complete ELF; the Hardware Report
ignores keys it does not know). `build_cd.py` compares `build-clock.json`
with `kernel_full_sha256` and copies both hashes into its build record.
Host tests: `tests/host/test_image_kernel.py`.

### Installed programs without debug sections (fx-12)

The same rule holds for every native ELF the image installs (`/Programs/*`,
`/System/Win32/w32srv`, `/System/Win32/shell-launch`, NetSurf, TinyCC, Lua, the
SDK test programs) and for `/Programs/setup` on the Setup CD:
`strip_payloads()` in `scripts/build_image.py` replaces each little-endian
ELF32 executable (ET_EXEC/ET_DYN with program headers) by its
`llvm-objcopy --strip-debug` copy in the build scratch directory
(`.image-strip-*` beside the image). `strip_elf()` checks every copy against
the complete file: same type, entry, program headers and header fields except
the section table, byte-identical file-backed segment bytes, no `.debug*`
section left (`.symtab` stays). The `build/apps/...` outputs keep the complete
files. Relocatable objects and archives (SDK `.o`/`.a`), PE files and data are
not touched. Measured on the current `build/apps` outputs: the on-disk programs
shrink from 57.1 MB to 14.1 MB (NetSurf 13.5 MB to 2.9 MB; `/Programs/setup`
on the CD 2.45 MB to 0.62 MB).

Consumers use the installed bytes: `payloads` records (hash and size read back
from the final disk by `check_payloads`), the app-gate `P path sha256` lines
(`application_provenance`, written after stripping), `build_cd.py`
(`stage_payloads` verifies the disk's files against the manifest; the Setup
program is stripped and `setup_program_sha256` is the installed file,
`setup_program_full_sha256` the complete one). `build-manifest.json` gains
`native_elf`: per stripped file `path`, `full_sha256`, `full_bytes`,
`installed_sha256`, `installed_bytes`. The per-file `elf` evidence under
`lua`/`desktop`/`tcc`/`netsurf` still describes the complete build outputs.
Host tests: `tests/host/test_image_native_strip.py`.


### Unattended hardware sweep

Before the single canonical build/write, supply a bounded BOOT.CFG containing
ordinary options and this line (choose one eight-hex-digit run id):

```text
safe=0 serial=1
probe=all:sweep run=12345678
```

Use the image builder's existing `--boot-cfg` argument; this changes runtime
configuration, not build-time probe variants. Verify/write/read back that image
once using the existing physical write procedure. Start the serial capture
before power-on. No P entry is required; N/S can cancel the configured request
in the three-second loader window. Normal steps and cold-reopen/cut steps reset
automatically. After an intentional panic or hang, observe the halted screen
for at least five seconds and power-cycle the machine. The cursor continues;
panic at the end of an F0-only sweep requires this recovery boot for SWEEP_END.
Do not edit step/state counters by hand. Completion removes only the probe line.
Read-only cursor refusal leaves evidence on screen and does not reset.

For the `input` and real-desktop `crash-isolation` steps, stay at the machine
and watch for the `[operator]` prompt on screen and serial. The first event
must arrive within 60 seconds. For `input` (f1-34, `criteria=operator`): press
and release A at least ten times, move the pointer, click and release the
left button at least once, then release everything and pause for one second;
the probe stops after the thresholds and one quiet second, or at its
120-second deadline. Loss, duplicates, stuck keys and the input lease are
still checked; typematic repeats from a held key are recorded, not failures;
being present alone does not pass it. For the desktop interaction, press and
release A, move the pointer and click and release the left button within its
normal 15-second window; observe that the desktop redraws. With no first event, only `stimulus` or `interaction` is
`status=not_run reason=operator_absent`; setup/lease, completed isolation cycles
and cleanup keep their verdicts. The sweep counts the incomplete step as
not_run and continues. Physical import preserves this reason and marks
`operator_confirmation_required: true`; a case confirmation cannot turn an
absent stimulus into a pass. Record real operator observations in
`case_confirmations` and keep the existing independent screen/disk evidence.

### Operator safe-mode boot after a sweep

The sweep's `safe` step normally boots with `safe=0`: its durable cursor needs
volume writes. It preserves its observations but records the required subcase,
END and SWEEP as not_run with `reason=safe_mode_not_selected`. It does not
qualify `safe-menu-t23` or `safe-menu-e500`, and the importer does not treat
this absent mode as a failed prerequisite for later cases.

After SWEEP_END, the on-disk `probe=` line has been removed; keep the disk and
the same image. Start a **separate** capture with a fresh eight-hex-digit run id
before powering on. In the three-second `SELECT_READY` window, press **S**
(upper- or lowercase, laptop keyboard or COM1). The loader cancels any configured
probe request, sets safe mode, prints `Safe mode; P for a probe, or wait`
and `L:SAFE=1`, and starts a fresh three-second menu window. Within that new
window, press **P**, type **`f1:safe run=<fresh id>`** (replace `<fresh id>` with
the chosen eight hex digits), and press **Enter** within the selector's
30-second entry deadline. The exact sequence is **S, P,
`f1:safe run=12345678`, Enter**, with a fresh id for each acquisition and no
`safe=1` suffix. The loader passes both the safe flag and the selector to the
kernel, and records `L:SELECT_SOURCE=menu`.

After S, expiry or N boots safe mode without a probe; N preserves the safe flag.
L can still toggle serial logging before P; keep logging enabled for this
acquisition. P alone does not add safe mode, and the ` safe=1` selector suffix
remains QEMU-only. Research and source comparison for the bounded menu restart
are recorded in [the boot contract](design/boot-memory.md#sources); this is the
F1-37 loader flow, with host checks distinct from the lead's QEMU and physical
qualification.

Observe `[safe] Console ready.` and the input/console evidence, then retain the
original serial bytes in that run's own capture directory as `f0.log`. Its
`acquisition.json` must contain the verified image/build identity,
`selector: "f1:safe run=<fresh id>"`, `selector_source: "menu"`,
`operator_confirmed: true`, and `case_confirmations: {"safe-menu-t23": true}`
(or the E500 id), confirming the actual operator observation. Qualification
requires END PASS, `safe_mode=1`, `source=menu`, zero optional activations and
mounts, and working console/input; an operator confirmation cannot replace those
records. A synthetic acquisition exercises the importer but establishes no
physical qualification.

The scoped importer can evaluate the declared physical case by passing it in
the `cases` argument of `physical.import_sweep`, together with the suite's
`physical_cases`. The current `run.py --physical-capture` summary enumerates
only the suite's QEMU `cases`; the lead must route/retain the separate physical
menu case there before the command-line procedure can qualify it. Do not merge
the operator boot into the sweep acquisition or relabel a QEMU safe case as
menu evidence.

Place the serial bytes as `f0.log` beside `acquisition.json` in the capture
folder (copy `serial.log` byte for byte; keep CRLF and UART noise). The JSON
object has the following schema. Inventory fields may contain `"unknown"`
when unmeasured; hashes, selectors, confirmation and build identity must be
verified, never inferred from the importer's current checkout.

| Required key | Value |
| --- | --- |
| `model` | Machine model, string; used for physical target predicates. |
| `unit_identity` | Operator-recorded identity of the particular unit, string. |
| `bios_version` | BIOS version, string. |
| `cpuid` | Measured CPU signature, hex string. |
| `installed_ram` | Installed memory, string with units. |
| `pci_ids` | Array of PCI identity strings, or `"unknown"`. |
| `target_disk_identity` | Identity of the disk written and read back, string. |
| `write_sha256`, `readback_sha256` | Matching 64-character lowercase SHA-256 strings for the exact image extent. |
| `capture_settings` | Serial/screen acquisition settings, string; serial uses 38400 8N1. |
| `operator_confirmed` | Boolean `true`, confirming model/unit and acquisition identity. |
| `build_id` | Exact embedded human build identity, e.g. `abcd67745e5f`; match every kernel banner, including any `-dirty` suffix. `embedded_build_id` is not an alias. |
| `selector` | Initial request, e.g. `all:sweep run=66666666`; alternatively supply `selectors`. |

| Optional or conditional key | Value |
| --- | --- |
| `selector_source` | Multi-boot import defaults to `"cfg"`, single-probe import to `"menu"`. A sweep requires `"cfg"`; damaged boot provenance follows the continuity rule below. |
| `devices` | Optional object declaring the physical configuration, e.g. `{"vga":"none"}`. Every case `device_exceptions` key/value must match explicitly; an absent declaration does not prove an absent device. |
| `machine_variants` | Optional object declaring matching case `fault` or `disk_cache` configuration. Values must match exactly, including all fields of a fault object. Never copy QEMU settings into acquisition metadata without physical evidence. |
| `selectors` | Nonempty array of explicitly approved requests for historical mixed-run captures; replaces `selector`. |
| `panic_observations` | Required to qualify each intentional panic: object keyed by the one-based boot number in the complete capture, e.g. `{"10":{"external_halt_seconds":10,"resumed":false}}`. Halt duration must be at least 5 seconds; `resumed:false` means no spontaneous execution resumed before the power-cycle. |
| `external_halt_seconds`, `resumed` | Legacy single-panic observation, or sweep fallback when no per-boot entry exists. |
| `case_confirmations` | Object mapping suite case ids to boolean `true`; required for operator-confirmation cases. Model confirmation does not confirm a case. |
| `disk_log` | Defaults to `"unavailable"`. Any other value requires `storage_qualified:true`; serial import still cannot replace independent disk/screen checks. |
| `storage_qualified` | Boolean, defaults to `false`. |
| `suite`, `utc_start`, `utc_end`, `image_size`, `build_manifest_hash`, `build_git_revision`, `build_dirty`, `runner_revision`, `external_seconds` | Optional single-probe provenance fields retained in its result; omitted values are `"unknown"`. Sweep import retains the entire acquisition object. |

Metadata is bounded to 128 KiB and sweep captures to 4 MiB.
Never infer an operator observation or checker result from the serial stream.
Import once with:

```bash
python3 scripts/test/run.py f2-all --physical-capture legacy/local/physical/12345678
```

For a historical image, explicitly select its verified hash:

```bash
python3 scripts/test/run.py f2-all --physical-capture legacy/local/physical/44444444 \
  --image-sha256 cb33aec38d502e57b966bca55ab0bad59a90f429b8d841e76ba85f9b93481a09
```

`--image` can locate the current canonical image in another worktree. The
override must match both acquisition hashes; it preserves the current canonical
hash, selected historical hash and mismatch in the summary, and does not relax
build/run/probe verification. It applies only to physical import.

The splitter uses the earliest surviving startup stage in each boot: `L:CPU`
(even if its diagnostic fields lost bytes), `SELECT_READY`, selector provenance,
then the kernel build banner. Later stages in that startup do not split again;
a sequence reset alone never establishes a boot boundary. Import uses original
bytes and CR/LF framing. Research: Python's
[bytes.splitlines documentation](https://docs.python.org/3/library/stdtypes.html#bytes.splitlines)
defines CR, LF and CRLF boundaries; unlike `str.splitlines`, it does not split
at form feed. This preserves the real capture's serial bytes and boot numbering.
Sequence gaps and damaged controller records become per-boot `records_lost`,
sequence ranges and original damaged bytes in hex. If an envelope is damaged,
the loss count is at least the number of unusable records; no missing identity
or sequence is invented. Such boots remain unqualified; duplicate/decreasing
sequences and identifiable wrong identities refuse
the acquisition. The anonymous host fixtures retain original controller,
startup and console lines; the earlier fixtures omit unrelated diagnostic
lines, and the historical fixture keeps only boots 1, 10 and 33. The local
`77777777` fixture retains all 35 boots byte for byte; its ignored `f0.log`
is not published, and the real-capture tests skip when the log is absent.

An intact `L:SELECT_SOURCE=cfg` retains the existing provenance rule. A damaged
value such as `fg`, or a damaged marker such as `L:ELECT_SOURCE=cfg`, qualifies
only when the immediately previous verified sweep boot has the same run id
and phase and the intact `probe=sweep event=BEGIN step=<n>` advances by exactly
one. A repeated step qualifies only for the same two-boot probe (`fat-write`,
`mount-crash`, `bootlog`) after its prior reboot/cut ARM. A completely missing
line additionally requires a damaged controller record or a sequence gap
starting at 1. Import retains `provenance: "damaged"`, the raw line and its
hex bytes (null if absent), and `provenance_inferred_from: "sweep continuity"`
in `boot_evidence`. Without continuity, that boot's results remain
`not_run reason=provenance_damaged`. An intact alternate source (`menu`,
`fw_cfg`, `none`), or a clean boot missing provenance, refuses the acquisition.
Record loss still prevents qualification even when provenance is inferred.

Cases requiring an undeclared or different physical device/machine variant
are listed separately by `device_exceptions_not_applicable` and
`not_run reason=device_exception_not_applicable:<key>=<value>` in the summary's
case entries. For example, `video-fallback` requires `devices: {"vga":"none"}`;
a T23 with an LFB does not satisfy its QEMU text-mode predicates. Matching
acquisition declarations enable the unchanged predicates; they do not supply
missing operator, disk or screen evidence. Inapplicable variants do not block
later cases as failed prerequisites. Sources and implementation comparison:
[QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html)
defines the VGA/serial absence and host block settings used by run.py;
[Python bytes.splitlines](https://docs.python.org/3/library/stdtypes.html#bytes.splitlines)
supports retaining the original damaged loader line bytes without text repair.

The command imports only and writes one profile=physical summary at
`build/test-runs/<suite>/summary.json`. Each case retains direct
`evidence_outcome`/`evidence_reason`, original sweep completions and
`not_run_reasons`, plus the prerequisite decision used for its qualification
`outcome`. A failing prerequisite marks subsequent cases not_run while keeping
their direct results. The sweep's reported totals are retained separately;
they are not a claim that suite predicates or physical qualification passed.
Exit 0 means all cases qualify and the sweep completed, 1 means evidence was
imported with failed/not_run cases or an incomplete sweep, and 2 means refusal.
Missing
fixture/disk checker/screen/operator evidence remains not_run. The normal sweep
safe step is `not_run reason=safe_mode_not_selected`; dedicated safe menu
qualification uses a separate acquisition as described above. QEMU suites remain unchanged; the lead
can run `python3 scripts/test/run.py sweep-smoke` to traverse F0 on one bounded,
reused overlay under -no-reboot and recover its final panic.
