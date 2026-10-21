# DOS title battery (f3-53)

This is a runtime battery against one canonical image, not a qualification of
twenty titles. The lead supplies archives, exact pins, calibrated checkpoints,
QEMU evidence and hardware evidence. No private manifest is read by a build.
No battery media or owner payload is a release input.

## Research and proposal, 2026-10-21

Tracked local battery editions: DOOM 1.9, Quake 1.06, Wolfenstein 3D 1.4,
Rise of the Triad 1.3, Commander Keen 4 1.4, Duke Nukem 3D 1.3d.
`config/dos-titles.json` is the lead's download/pin handoff. Null hashes are unavailable pins, never wildcards. Amendment 1 below records
the supplied pins and edition-specific licence verification for the last three.

| Title | Licence evidence and decision |
| --- | --- |
| DOOM | Original `DOOMS_19.DAT` banner and DOOM1.WAD ENDOOM grant sharing; exact clauses and hashes are in `third_party/games/doom-shareware/UPSTREAM.json`. Retain the original archive; the unrelated FAQ has separate restrictions. |
| Quake | [id Software SLICNSE.TXT, sections 6–7](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/data/SLICNSE.TXT): free whole-package distribution, compressed electronic distribution by providers. Never publish extracted PAK data alone. |
| Wolfenstein | Original `third_party/games/wolf3d-shareware/VENDOR.DOC`, sections 3A/3C: complete unmodified episode only; free sharing allowed, software bundles require signed permission. Private test installation only. |
| ROTT | Amendment 1 verifies the original `ROTTSW13.SHR!VENDOR.DOC`; complete unchanged free sharing, prior permission for bundles. |
| Keen 4 | Amendment 1 verifies the original `CK4SW14.SHR!VENDOR.DOC`; complete unchanged free sharing, prior permission for bundles. |
| Duke 3D | Amendment 1 verifies the original `DN3DSW13.SHR!LICENSE.TXT`; limited complete unchanged free sharing, other distribution requires permission. |

The [publisher download list mirror](https://ftpmirror.infania.net/pub/3drealms/downloads/Downloads.html)
identifies original Apogee shareware archives; it does not replace their licences.
The original legacy publisher site was unavailable during research. The three new archive licences were initially pending; Amendment 1 below
records their edition-specific read-back and limited free-sharing grant.

The [QEMU fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html)
supports an `opt/` runtime selector; the existing 64-byte loader field is
preserved. [QEMU image documentation](https://www.qemu.org/docs/master/system/images.html)
specifies backing-file overlays. Reuse the canonical runner's locks, caps,
identity recording, qcow2 and cleanup rather than introducing a QEMU process
manager. [QMP input documentation](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
distinguishes key down/up; title scripts likewise carry separate set-1 edges.
The production presenter already provides `dosvm_frame_digest`, including text.
Use that digest, with a fixed cursor/blink clock of zero.
The [Microsoft FAT specification, sections 3, 4 and 6](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
defines the BPB, allocation chains and directory entries. Reuse the existing
FAT16 writer/embedder and independently read back both levels with mtools and
fsck; do not introduce another filesystem implementation for this battery.

Original [DOOM M_LoadDefaults](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/m_misc.c),
[Quake Host_Quit_f](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/host_cmd.c)
and [Wolfenstein ReadConfig](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/WL_MAIN.C)
confirm config and exit paths differ. Scripts and goldens are edition-specific;
elapsed time alone cannot prove a menu or gameplay. Existing `RUNREC.COM`
wraps the unmodified executable and reports its DOS exit status.

**Amendment 1 decision:** the title battery selects SB16 220/IRQ 5/DMA 1/5
through the existing per-VM mixer interface and BLASTER setting; see below.

## Manifest v1

Both manifests have `{ "schema_version": 1, "titles": [...] }`. Title IDs
are unique lowercase ASCII `[a-z][a-z0-9-]{0,22}`. Tracked rows must have
`licence.class=bundled` or `test-media`; private owner rows use `owner-copy`. Private input is
selected only when explicitly running the battery: `CIUKI_DOS_TITLES`, or
`legacy/local/piano/dos-titles.json`. It is never needed for host tests.
Private manifests use opaque IDs. Names/year/publisher appear only in the
local report, never kernel records. Proposed/unpinned/uncalibrated titles
produce `not_run`, never success.

`run` combines tracked rows with the private manifest when it exists. An
explicit `CIUKI_DOS_TITLES` or `--private` path must exist; duplicate IDs fail.
`validate` reads tracked metadata only unless a manifest/private path is given.

The executable validator in `scripts/test/dos_titles.py` defines the exact
required fields, types and bounds. Each title declares name, year, publisher,
licence (class, citation, conditions), archive/layout/install steps, executable,
arguments, boot/menu/gameplay checkpoints, timed key edges for launch/play/exit,
audio expectation (`digital`, `opl`, `both`, `none`) and SB16 wiring, LAN
(`none`, `ipx`), conventional/EMS/XMS KiB and DPMI runtime. Archive layout is
either an existing pinned `build_games_disk.py` package, a safe ZIP, or a
fully pinned installed directory. Installation UI steps are documented;
automatic extraction supports ZIP and existing package adapters. Amendment 1
adds original installer automation and capture/verify modes, described below.

| Field | Required representation |
| --- | --- |
| `licence` | `class` (`bundled`, `test-media`, `owner-copy`), `citation`, `conditions`; describes the packaging boundary, not runtime qualification |
| `install` | `layout` (`package`, `zip`, `directory`, `installer`), `pin` ID, optional local `source`, DOS `target`, `files` and documentary `steps`; private archives may add `archive_pin:{size,sha256}` with an explicit `source` |
| `install.files` | Directory layout: complete explicit list of `{path, sha256}`, relative uppercase 8.3 paths; no unpinned files copied |
| `executable`, `args` | Executable in target directory and argument array; command length at most 126 ASCII bytes, no shell metacharacters |
| `checkpoints` | Ordered boot/menu/gameplay: `{name, at_ms, deadline_ms, kind, value}`; `value=null` means pending calibration |
| `input` | Up to 128 ordered `{phase, at_ms, code, down}` entries; balanced set-1 edges, with an exit script |
| `audio` | `{expectation, base:544, irq:5, dma8:1, dma16:5}`; explicit IRQ 7 also supported for a lead-approved profile |
| `memory` | `{conventional_kib, ems_kib, xms_kib, dpmi}`; DPMI is `none`, `dos4gw`, `cwsdpmi` or `native`; optional `dpmi_fallback` is `none` or `cwsdpmi`; informational, no VM limits changed |

The tracked scripts, setup notes and memory estimates are proposals awaiting
edition-specific calibration. The synthetic manifest in
`tests/dos-titles/fixtures/synthetic.json` is a schema example, not a game.

Host-only commands (run sequentially with `nice -n 19`):

```sh
python3 -B scripts/test/dos_titles.py validate
python3 -B scripts/test/dos_titles.py validate --manifest /path/to/private.json
python3 -B scripts/test/dos_titles.py parse --manifest tests/dos-titles/fixtures/synthetic.json --title synthetic --run-id 12345678 --serial tests/dos-titles/fixtures/serial.txt
```

After the lead pins and calibrates the profiles, the lead runs
`python3 -B scripts/test/dos_titles.py run --title <id>` against the existing
canonical image. Output lives under `build/test-runs/dos-titles/`: the
canonical runner's `result.json` and serial capture, `title-result.json`, plus
`summary.json` and `summary.md`. No QEMU command is run by validation or parsing.
The summary names the target count of 20 and never qualifies that goal from a
smaller subset. Per-title success is independent of the pending LAN gate.

Screen checkpoints use exact SHA-256, or an ASCII substring of the actual
80x25 VGA text screen. Each has a start time and a deadline after executable
launch. Boot, menu and gameplay must all be observed in order; menu must be
within 60 s of VM start. Missing goldens are explicit calibration work.
Play/exit input times are relative to the observed gameplay checkpoint.

## Evidence and acceptance

Selector: `f3:title run=<8 hex> title=<opaque id>`, no other suffixes. It is
selector-only, excluded from sweeps and `all/core`; reserve `f3:title` for this
controller. A bounded binary `DTITLE.PLN` on the private fixture volume binds
the selected ID, command, checkpoint expectations and input plan. The volume
also contains `CIUKIGAM.IMG`, generated with existing DOS FAT16/system paths.

`CIUKI_TEST ... probe=title ... group=dos_title title=<id>` records include
VM identity, checkpoint timing and frame digest, delivered input count,
30 s gameplay, gameplay-interval SB16 counter deltas/IRQ latency maximum, backend underrun deltas,
OPL writes and sustained key-on activity, normal child exit and restored VM ledger. Retain `[vm-audio]`
cadence lines as diagnostics; absent audio evidence cannot mean zero errors.
Records are bound to the selected run/title, the initial VM key and the exact
plan hash; duplicate or missing title subcases and partial logs fail. Global
sequence numbers must increase, but UART gaps are allowed: `core/output.c`
also numbers disk-only diagnostics. They are not missing title observations.

A title passes only after a measured menu within 60 s, its calibrated gameplay
checkpoint, at least 30 s without a VM fault, all input delivered, expected
audio activity with an attached backend, zero underruns, max IRQ latency
20,000 us or less, normal zero child exit and restored resource ledger.
Report `late_acks` without inventing an acceptance threshold.
LAN is **pending f6-11**: two independently identified VMs/peers must join the
same IPX session, exchange gameplay traffic for 30 s, retain per-peer audio
and fault acceptance, and exit cleanly. Serial/modem are outside scope.

## Amendment 1 — verified packages and runtime calibration (2026-10-21)

The supplied archives were read locally, checked against the lead's size and
SHA-256, and unpacked under `build/codex/f3-53-extracted/`. No download was made.
The [publisher's original download page, preserved by its mirror](https://ftpmirror.infania.net/pub/3drealms/downloads/Downloads.html)
identifies the shareware releases. Permission below comes from each package's
own notice, rather than the shareware label or an engine source licence.

| Title | Original archive, bytes, SHA-256 | Notice member and its SHA-256 |
| --- | --- | --- |
| ROTT 1.3 | `1rott13.zip`, 3668139, `eea67a85845d52fa0ee7272e3ae24b79769ed1e8bdff7b1360904078bc51ad1e` | `ROTTSW13.SHR!VENDOR.DOC`, `a8a73af0f5f46e975d005becfac692c48c097dc43b9230a32c0734801c9df701` |
| Keen 4 1.4 | `4keen.zip`, 729683, `f7c63feb8bbec817646d95aa26313df1654c01eda8f7ccc5cfd0c0bc0b5e305e` | `CK4SW14.SHR!VENDOR.DOC`, `4902b35ac79c7c6efcc76d8c91c0a6f9b1ba4d4fd7384f669dae2dddd9b3eafc` |
| Duke 3D 1.3d | `3dduke13.zip`, 5924374, `c67efd179022bc6d9bde54f404c707cbcbdc15423c20be72e277bc2bdddf3d0e` | `DN3DSW13.SHR!LICENSE.TXT`, `38cd7edf73ba672db785a2c30ae0f2cceded7f94a6297397548ee8ce5312547d` (identical outer notice) |

ROTT and Keen's respective `VENDOR.DOC [V.07.15.94]`, KEY POINTS:

> Everyone can -- and is encouraged! -- to copy, upload and generally pass
> around this Program without charging for it.

Both notices, section [3][A][1]:

> All of the Program's files, including this one, as released by us must be
> included without modification.

Both notices require prior signed permission for bundles, CD-ROMs and retail
racks ([3][C]); the free sharing grant does not grant those rights.

Duke's `LICENSE.TXT [V.7.16.98]`, KEY POINTS:

> Individuals can -- and are encouraged! -- to give away copies of the Game
> without charge.

Section [3][C]:

> all of the Game's files as released by 3D Realms will be included without
> modification

Its [3][A–D] restrict recipients/channels; other distribution requires written
permission and a fee/agreed benefit. Amendment 2 classifies all three as
`test-media`: these limited sharing grants do not qualify software bundles.
Licence metadata never asserts runtime verification. Wolfenstein is also
`test-media`; the two existing approved bundled packages are qualified below.

`config/dos-titles.json` also records the complete extracted file inventory
and a tree hash. Keen uses the existing PKWARE DCL `.SHR` decoder. ROTT and
Duke have ZIP self-extractors in `.SHR`; Python's ZIP reader validates their
CRC without executing the executable prefix. All outer and inner files are
preserved in the local research extraction. The runtime fixture contains the
complete outer archive contents in `C:\MEDIA`; it does **not** preinstall the
unpacked game in place of running `INSTALL.EXE`.

### Installer automation and IRQ 5

`install.layout=installer` adds `install.automation`, up to four stages:
`{directory, executable, args, timeout_ms, input, tree_sha256}`. A stage has
1..128 balanced, ordered set-1 key edges `{at_ms,code,down}`, a 1..300 s timeout,
and a root-level working directory. The controller launches the unchanged
installer/setup with `RUNREC.COM`, delivers keys, checks zero child status and
return to the shell, and hashes the actual target tree through the existing
private VFS mount before launching the next stage. Missing files, unexpected
files, timeout, nonzero exit, wrong pinned tree or missing key edges fail.
The manifest holds proposed installer/setup keys; their actual UI navigation
and timings need the lead's runtime calibration. There is no manual-install
fallback. Keen configures its supported sound devices in the game; it has no
separate `SETUP.EXE` in this edition.

The tree digest is SHA-256 over files sorted by uppercase relative DOS path:
`path ASCII || NUL || size LE32 || content SHA-256 (32 raw bytes)`. It covers
hidden/system files, excludes directory entries/timestamps and is bounded to
127 files/directories beneath the target and 96 MiB. Empty/missing trees fail.
Each installer has a pinned original tree; setup stages additionally capture
configuration output during calibration. No hand-supplied directory replaces
the original installers for the three amended profiles.

The [Creative Sound Blaster Hardware Programming Guide, mixer registers 80h/81h](https://www.phatcode.net/res/243/files/sbhwpg.pdf)
assigns IRQ 5 to bit 1 of 80h and DMA 1/5 to bits 1/5 of 81h. The repository
already implements those writes in `dosvm_sb16.c::sb_mixer_write`. The title
controller programs that existing interface for its VM before boot and sets
`BLASTER=A220 I5 D1 H5 T6` after the shell starts; the setup key sequences select
IRQ 5. This confines the amended wiring to the title battery and leaves other
committed profiles at their existing defaults. Cadence verification still
requires the actual selected IRQ to match.

### Calibration and verification

```sh
# Lead only: use the existing canonical runner/image, never rebuild per title.
python3 -B scripts/test/dos_titles.py --calibrate rott --profile qemu-t23
python3 -B scripts/test/dos_titles.py --accept-calibration rott --reviewer <lead-name> --profile qemu-t23
python3 -B scripts/test/dos_titles.py --verify --title rott --profile qemu-t23
# Import a lead-collected physical serial log; no QEMU process is started.
python3 -B scripts/test/dos_titles.py --calibrate rott --serial /path/to/serial.txt --run-id 12345678 --profile t23 --image-sha256 <64-hex> --captures /path/to/title-captures
python3 -B scripts/test/dos_titles.py --accept-calibration rott --reviewer <lead-name> --profile t23 --image-sha256 <64-hex>
python3 -B scripts/test/dos_titles.py --verify --title rott --serial /path/to/verify.txt --run-id 87654321 --profile t23 --image-sha256 <64-hex>
```

Each manifest's parent contains `<id>/calibration.json`; this keeps private
calibrations beside their private manifest. The file has separate profile
entries and binds them to the manifest definition, original definition plan,
actual capture plan, run ID and
canonical image SHA-256. It stores actual VGA frame hashes and both VM-relative
and launch-relative checkpoint times, plus installer tree hashes/times. A
changed script, pin recorded in the manifest, profile or image requires a new
calibration. After explicit acceptance, verification fills only null expectations and searches
for those captured hashes within the original timing windows, while retaining
observed launch-relative times as evidence, and preserves
every acceptance threshold. A calibration is
reported as `calibrated`, never `pass`, and never counts toward twenty titles.
Known screen/text expectations and extracted-tree hashes are checked even during
calibration; only null expectations are learned. Capture does not assert a semantic title/menu:
the lead reviews the scripted scenes before committing a calibration.

The optional `D53PLAN2` wire format retains the v1 prefix, adds a capture flag
and up to four bounded installer stages; unchanged v1 plans remain accepted.
Null capture checkpoints use kind 2 and emit `matched=0`; known checkpoints
retain their original kind/value/timing and emit `matched=1` after comparison.
Verification plans cannot contain kind 2. The selector stays `f3:title run=<id> title=<id>` with no new
token. Serial evidence binds the complete plan hash and exact mode; installer
metrics and separate `install-tree` digest records occur in stage order before the VM start record used by the existing
acceptance evaluator. Time remains measured from VM creation, including
installation: the amendment does not relax the 60 s menu limit. The lead must
calibrate scripts that meet it; a slower install remains a failed criterion.

The [QEMU fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html),
[backing image documentation](https://www.qemu.org/docs/master/system/images.html),
[QMP key-edge contract](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
and [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
were checked against the existing selector/runner/fixture code. Reuse those
paths; no second process manager, filesystem or build variant was introduced.
The original [DOOM configuration code](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/m_misc.c),
[Quake exit code](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/host_cmd.c)
and [Wolfenstein configuration code](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/WL_MAIN.C)
remain the basis for their unchanged edition-specific profiles.

### Optional FreeDOS regression media

The three release-dependent regression tests look only in
`third_party/freedos/1.3`. The worktree's tracked `UPSTREAM.json` remains
authoritative. The mistaken nested provisioning symlink is not used. A missing archive produces a unittest skip listing the
exact missing files and searched locations. Different pins/corrupt media fail;
absence is never passed off as reference evidence.

The [PKWARE APPNOTE, 4.4.2 and 4.4.15](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)
defines external attributes according to the originating host system. The
supplied DOS archives have host ID 0 and non-Unix high attribute words. Apply
Unix symlink-mode rejection only for Unix host ID 3; retain path, size,
duplicate, CRC and pin checks for every host. This prevents falsely rejecting
Keen's DOS-origin `INSTALL.EXE` as a Unix symbolic link.

## Amendment 2 — boundaries and evidence protection (2026-10-21)

This supersedes Amendment 1's licence verification terminology. The battery
ships no title: its generated disks are private workstation test inputs,
never inputs to image/release builders. There is no `redistributable` licence
kind or licence `verified` flag. The six tracked rows and their tracked pins
have these classes:

| Title | Class | Original clause and exact boundary |
| --- | --- | --- |
| DOOM 1.9 | `bundled` | `third_party/games/doom-shareware/UPSTREAM.json`: ENDOOM/installer sharing grant, plus [John Carmack's 1999 clarification preserved by Debian](https://tracker.debian.org/media/packages/d/doom-wad-shareware/copyright-1.9.fixed-4). This classification covers the approved installed shareware package with `DMFAQ66A.TXT` through `DMFAQ66D.TXT` excluded: their own II(d) prohibits inclusion within software without permission. It does not make the complete archive an unrestricted OS bundle. |
| Quake 1.06 | `bundled` | Original [SLICNSE.TXT section 6](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/data/SLICNSE.TXT) and f3-18 qualification: whole unmodified shareware, agreement accompanying it, free electronic distribution in compressed form for providers; the end-user grant is whole-package free sharing. No standalone extracted PAK redistribution or physical-media grant. Carmack's DOOM clarification is not a Quake grant. |
| Wolfenstein 3D 1.4 | `test-media` | `third_party/games/wolf3d-shareware/VENDOR.DOC` [3][A] requires complete unchanged files; [3][C] requires prior signed Apogee permission for hardware/software bundles, CD-ROMs and retail racks. Local battery use only. |
| Rise of the Triad 1.3 | `test-media` | Pinned `ROTTSW13.SHR!VENDOR.DOC`, SHA-256 above: [3][A] requires complete unchanged files; [3][C] requires prior signed permission for bundles, CD-ROMs and racks. Local battery use only. |
| Commander Keen 4 1.4 | `test-media` | Pinned `CK4SW14.SHR!VENDOR.DOC`, SHA-256 above: same [3][A]/[3][C] boundary. Local battery use only. |
| Duke Nukem 3D 1.3d | `test-media` | Pinned `DN3DSW13.SHR!LICENSE.TXT`, SHA-256 above: [3][A]/[B] limited sharing/download channels; [3][C] requires all original files without modification; [3][D] requires written permission and fee/agreed benefit for all other distribution. Local battery use only. |

The Apogee notices say in [3][C]:

> must get PRIOR signed written permission from Apogee

Duke [3][D] states:

> REQUIRES WRITTEN PERMISSION

These are publisher notices read from the already provisioned pinned archives;
the [original publisher download page preserved on its mirror](https://ftpmirror.infania.net/pub/3drealms/downloads/Downloads.html)
identifies editions, not additional permissions. No packaging policy outside
this battery is changed. In particular, the pre-existing games-disk package
list still contains Wolfenstein: the lead must reconcile that separate image
input with this amendment's bundle boundary before publication.

An `owner-copy` archive supplies `install.archive_pin={size,sha256}` beside
its local `install.source`; the tracked config is neither extended nor used
as its pin authority. Readiness, file generation and calibration definition
binding all resolve that private pin at runtime. A changed private pin
invalidates calibration. Existing pinned-directory layouts remain supported.

`--calibrate` checks every non-null manifest screen/text/tree expectation;
only null checkpoints get capture kind 2. Applying calibration preserves known
values and kinds, and every original timing window. Recapture also checks existing captured expectations.
Each profile record begins with `status=calibrated`, which cannot qualify the
twenty-title goal. The lead reviews the saved frames, then explicitly invokes
`--accept-calibration <id> --reviewer <name>` for the same profile/image. That
step checks bindings and artifact hashes and records `status=verified`,
`accepted_by` and UTC `accepted_at`. `--verify` refuses an unaccepted record.
New capture requires acceptance again.

During capture, the controller writes `BOOT.PPM`, `MENU.PPM`, `GAMEPLAY.PPM`
and matching `.TXT` files only to the disposable fixture volume. P6 images
are 640x480 RGB from the exact presenter rows used for the recorded XRGB
digest; `.TXT` is 4000 bytes of raw little-endian VGA character/attribute cells,
also retained when graphics mode is active. No desktop screenshot substitutes
for a VM checkpoint. After guest teardown, the runner independently checks
the embedded original game disk and plan digests, exports the six files to
`title-captures/`, and retains them on successful calibration. The CLI verifies
image-to-digest consistency and copies them beside `calibration.json` under
`<profile>/<run-id>/`. Physical serial imports require `--captures <directory>`
with the same files exported by the lead from that run's disposable fixture.
This is diagnostic retention required for review; no whole-image copy or RAM
dump is made. Verification fixtures remain byte-for-byte immutable.

Gameplay begins after the gameplay checkpoint. PCM frames, blocks, IRQ entries,
late acks, backend starvation and OPL writes are end-minus-start deltas for
that interval. A dedicated observer, cleared at gameplay start, records its
IRQ latency maximum; it does not subtract lifetime maxima or reset diagnostic
cadence. The [Creative guide](https://www.phatcode.net/res/243/files/sbhwpg.pdf)
remains the basis for SB16 IRQ 5/DMA 1/5. Startup/menu sound cannot satisfy
gameplay activity. Music requires at least three qualifying key-on/frequency
writes and activity in each of the interval's three ten-second thirds
(`case=music key_writes=... windows=7`). The [upstream Linux OPL3 register definitions](https://raw.githubusercontent.com/torvalds/linux/master/include/sound/opl3.h)
confirm A0-A8 frequency, B0-B8 KEYON/BLOCK and key-on bit 0x20. Count B0-B8
writes with key-on set and A0-A8 writes while the corresponding key is on,
in either bank; detection/timer writes and key-off resets do not qualify.

Quake expects `digital` SFX only: this archive contains no CD audio tracks.
Its own README's CD-audio instructions describe a separate CD/MSCDEX path;
it does not use OPL music. Metadata identifies the native DPMI server as
primary and the original `CWSDPMI.EXE` as fallback, matching f3-18.

Checkpoint attempts and gameplay pixel samples share a 500 ms throttle
(at most two complete hashes per second). Gameplay polls existing VGA
change/display counters and VBE dirty state between hashes, skipping scans
when unchanged. The complete digest API remains deterministic for its other
callers; only calibration adds an optional row sink. Installer metrics and
the 64-digit tree digest use separate ordered `install`/`install-tree`
records. Host tests expand every title DATA format with the longest ID,
UINT32 sequence/metrics and UINT64 counters and assert the full envelope is
at most 240 bytes. Acceptance thresholds and the pending f6-11 LAN gate stay.
