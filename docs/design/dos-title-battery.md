# DOS title battery (f3-53)

This is a runtime battery against one canonical image, not a qualification of
twenty titles. The lead supplies archives, exact pins, calibrated checkpoints,
QEMU evidence and hardware evidence. No private manifest is read by a build.
No battery media or owner payload is a release input.

## Research and proposal, 2026-10-21

Proposed shareware editions: DOOM 1.9, Quake 1.06, Wolfenstein 3D 1.4,
Rise of the Triad 1.3, Commander Keen 4 1.4, Duke Nukem 3D 1.3d.
`config/dos-titles.json` is the lead's download/pin handoff. Null hashes are
unavailable pins, never wildcards. The last three need archive and
edition-specific licence verification by the lead before use.

| Title | Licence evidence and decision |
| --- | --- |
| DOOM | Original `DOOMS_19.DAT` banner and DOOM1.WAD ENDOOM grant sharing; exact clauses and hashes are in `third_party/games/doom-shareware/UPSTREAM.json`. Retain the original archive; the unrelated FAQ has separate restrictions. |
| Quake | [id Software SLICNSE.TXT, sections 6–7](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/data/SLICNSE.TXT): free whole-package distribution, compressed electronic distribution by providers. Never publish extracted PAK data alone. |
| Wolfenstein | Original `third_party/games/wolf3d-shareware/VENDOR.DOC`, sections 3A/3C: complete unmodified episode only; free sharing allowed, software bundles require signed permission. Private test installation only. |
| ROTT | Publisher's VENDOR.DOC reproduced in the [distribution discussion](https://lists.debian.org/debian-legal/2007/02/msg00055.html); edition-specific original still needs lead read-back. Same no-bundle restriction. |
| Keen 4 | Proposed original `4keen.zip`, VENDOR.DOC; edition-specific text not yet verified. Do not infer permission from its shareware label. |
| Duke 3D | Proposed original `3dduke13.zip`, VENDOR.DOC/LICENSE.DOC; edition-specific text not yet verified. Engine source GPL does not license the data. |

The [publisher download list mirror](https://ftpmirror.infania.net/pub/3drealms/downloads/Downloads.html)
identifies original Apogee shareware archives; it does not replace their licences.
The original legacy publisher site was unavailable during research. The three
new archive licences need edition-specific read-back; these candidates are
proposals, not redistribution clearance.

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

**Lead decision:** f3-53 specifies SB16 220/IRQ 5/DMA 1/5; current
`dosvm_sb16.c` and DOS BLASTER/config advertise IRQ 7. Keep the requested
IRQ 5 in proposed profiles and refuse a runtime mismatch. This task does not
change VM hardware or silently substitute IRQ 7.

## Manifest v1

Both manifests have `{ "schema_version": 1, "titles": [...] }`. Title IDs
are unique lowercase ASCII `[a-z][a-z0-9-]{0,22}`. Tracked rows must have
`licence.kind=redistributable`; owner rows use `owner-copy`. Private input is
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
licence (kind, citation, conditions), archive/layout/install steps, executable,
arguments, boot/menu/gameplay checkpoints, timed key edges for launch/play/exit,
audio expectation (`digital`, `opl`, `both`, `none`) and SB16 wiring, LAN
(`none`, `ipx`), conventional/EMS/XMS KiB and DPMI runtime. Archive layout is
either an existing pinned `build_games_disk.py` package, a safe ZIP, or a
fully pinned installed directory. Installation UI steps are documented;
automatic extraction supports ZIP and existing package adapters. Original
interactive installers require a lead-provided pinned installed directory;
they cannot be silently treated as executed by this battery.

| Field | Required representation |
| --- | --- |
| `licence` | `kind`, `citation`, `conditions`, boolean `verified`; false blocks execution |
| `install` | `layout` (`package`, `zip`, `directory`), `pin` ID, optional local `source`, DOS `target`, `files` and documentary `steps` |
| `install.files` | Directory layout: complete explicit list of `{path, sha256}`, relative uppercase 8.3 paths; no unpinned files copied |
| `executable`, `args` | Executable in target directory and argument array; command length at most 126 ASCII bytes, no shell metacharacters |
| `checkpoints` | Ordered boot/menu/gameplay: `{name, at_ms, deadline_ms, kind, value}`; `value=null` means pending calibration |
| `input` | Up to 128 ordered `{phase, at_ms, code, down}` entries; balanced set-1 edges, with an exit script |
| `audio` | `{expectation, base:544, irq:5, dma8:1, dma16:5}`; explicit IRQ 7 also supported for a lead-approved profile |
| `memory` | `{conventional_kib, ems_kib, xms_kib, dpmi}`; DPMI is `none`, `dos4gw`, `cwsdpmi` or `native`; informational, no VM limits changed |

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
30 s gameplay, cumulative SB16 late acks/IRQ latency, backend underruns,
OPL writes, normal child exit and restored VM ledger. Retain `[vm-audio]`
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
