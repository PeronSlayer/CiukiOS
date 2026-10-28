# Twenty DOS games: host qualification contract

The twenty entries in `config/dos-titles.json` are original DOS shareware or
demo packages. The fourteen additions are **test media**, with `lead-pins`
where the sandbox cannot fetch the package. Their coverage annotations are
qualification targets, not measured compatibility. An archive mirror's
download label is not a redistribution licence. Exact package terms, original
installer output, sound configuration and gameplay input must be reviewed
before acceptance. Existing bundling decisions are preserved.

Research and implementation decisions:

* Windows 98 Resource Kit, chapter 25, local `build/rskt/wrkc25_3.htm`,
  “Understanding the Apps.inf File” and “Running Games”: use the existing
  per-program screen preference and first-graphics automatic full screen.
  A request alone is not proof of full screen. QEMU's documented standard VGA
  refusal remains a blocking qualification failure for these new cases.
* [id Software IPXSETUP.C](https://github.com/id-Software/DOOM/blob/master/ipx/IPXSETUP.C),
  `LookForNodes`, and
  [IPXNET.C](https://github.com/id-Software/DOOM/blob/master/ipx/IPXNET.C),
  `InitNetwork`, `SendPacket`, `GetPacket`: the game discovers peers and uses
  ECB send/listen completion. A NIC link or an IPX detection call does not
  prove multiplayer. Require both guests' traffic and calibrated gameplay
  screens; do not substitute the NE2000 packet-driver path for the VMM IPX API.
* [QEMU invocation, socket netdev](https://www.qemu.org/docs/master/system/invocation.html):
  an isolated multicast bus with distinct MACs connects the pair at Ethernet
  level, carrying EtherType 0x8137 without slirp. Each guest retains its own
  capped scope, overlay and serial capture. Failure cancels the entire pair.
* [QEMU input QAPI](https://github.com/qemu/qemu/blob/master/qapi/ui.json),
  `InputEventKind`, `InputMoveEvent`, `input-send-event`: send relative mouse
  motion and balanced button presses for titles that use a mouse. QMP exposes
  keyboard/pointer/touch input, not a joystick-axis event. The existing native
  joystick and DOS virtual 201h/INT 15h paths are preserved; actual controller
  input and per-game calibration still need lead hardware evidence. Keyboard
  input is never described as a joystick test.
* [QEMU storage daemon export API](https://www.qemu.org/docs/master/interop/qemu-storage-daemon-qmp-ref.html):
  writable FUSE raw slices permit mtools to stage pinned test media and a
  temporary Games shortcut in the run overlay before any guest starts.
  The nested DOS volume must be contiguous; refuse otherwise. No whole-image
  copies, image rebuilds, guest probes or canonical-image writes are needed.
* [DP8390D/NS32490D vendor manual, July 1995](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D%2CNS32490D.pdf),
  sections 7–10, and [QEMU NE2000 model](https://github.com/qemu/qemu/blob/master/hw/net/ne2000.c):
  receive filtering and remote DMA are device semantics; this work reuses the
  existing f6-11 implementation. No device model or IPX wire format is changed.
* [Novell IPX counters](https://www.novell.com/documentation/developer/ncp/ncp__enu/data/a1vqwbh.html):
  send, listen and completed receive are distinct observations. Bounded serial
  diagnostics distinguish accepted outgoing frames from accepted incoming IPX.

Download provenance: publisher FTP mirrors for
[Heretic](https://www.gamers.org/pub/idgames/idstuff/heretic/) and
[Hexen](https://www.gamers.org/pub/idgames/idstuff/hexen/);
[3D Realms original share directory](https://www.gamers.org/pub/mirrors/ftp.3drealms.com/share/)
for the Apogee/3D Realms candidates. The latter directory and publisher legacy
site were inaccessible during research; filenames remain lead-check items.
The long-standing archive's original installer downloads for
[Descent](https://www.dosgamesarchive.com/file/descent/desc14sw),
[Descent II](https://www.dosgamesarchive.com/file/descent-ii/d2demo10) and
[Jazz](https://www.dosgamesarchive.com/file/jazz-jackrabbit/jazz) identify
candidates, not verified package terms. No game payload is added to git.

Lead procedure and exact outstanding evidence are in the implementation report
`build/codex/fx-games20-report.md`. Host fixtures prove runner/parser behavior;
they do not prove the twenty games play or exchange multiplayer traffic.

## Lead media and calibration procedure

1. Check each URL and original edition, then use
   `python3 -B scripts/test/dos_games.py fetch --title ID --lead-pin` for an
   unpinned candidate. For a pinned archive omit `--lead-pin`: a mismatch must
   fail, never silently update the expected digest. The only download output
   is `build/downloads/dos-titles`. `UPSTREAM.json` and the pin manifest are
   updated together; this does not mark package terms as reviewed.
2. Read the package's exact licence and manual, record its clauses and member
   hashes in its provenance record, retain all original documents, and review
   `terms_reviewed` in `tests/dos-titles/games20.json`. Keep a title test-media
   unless its terms permit the proposed bundle. Run the original installer
   and sound setup locally; do not use a DOSBox repack or registered edition.
3. Put the installed files and tested sound settings under the manifest's
   `ID-installed` directory (uppercase DOS 8.3 paths). Select SB16 220h/IRQ5/
   DMA1/5, OPL music, and General MIDI via MPU-401 for the Descent II music
   case. Run `python3 -B scripts/test/dos_games.py pin-installed --title ID`.
   Its per-file pins include the original executable, data, licence documents
   and the generated configuration. `G20RUN.BAT` is reserved to the fixture.
4. Calibrate `enter`, `play`, `quit`, and the gameplay-identifying screen
   region/hash. Set `calibration.image_sha256`, `profile: qemu-t23`, reviewer
   and `single` checkpoint. IPX titles also need the real driver executable
   (for example DOOM's IPXSETUP.EXE), host/join arguments, host/join input and
   separate reviewed multiplayer gameplay checkpoints. A broadcast alone
   cannot substitute for a multiplayer gameplay checkpoint.
5. Regenerate the static cases with
   `python3 -B scripts/test/dos_games.py write-cases`; run the host checks,
   `python3 -B scripts/test/dos_games.py validate --require-pins`, then use
   `scripts/test/acceptance.sh --case acc-dos-ID` and
   `scripts/test/acceptance.sh --case acc-ipx-ID` on the canonical image.
   `scripts/test/acceptance.sh --list` needs no image or QEMU.

The runner stages the pinned installed tree into the existing DOS FAT16
volume inside the **outer run overlay**, then updates that overlay's DOS
sidecar and temporary Games link. The canonical DOS kernel and image are not
rebuilt. A fragmented nested volume is refused. Full-screen refusal is
currently expected on standard QEMU VGA (fx-dosfull/f3-29), so these strict
cases can expose that gate without granting a false gameplay pass.

## fx-games20b: exact pinned package review

The fourteen archive sizes and SHA-256 pins were checked against the lead's
read-only downloads before reading their original licence/vendor members.
`config/dos-titles.json` records exact member paths, byte hashes and clause
summaries; each title's `UPSTREAM.json` repeats that evidence and records the
observed edition, executable and installer container. No payload or installed
tree is added. All fourteen remain `test-media`.

Primary sources are the original publisher documents in those pinned archives,
not the mirror's description or a source-code licence. The publisher FTP
[Heretic index](https://www.gamers.org/pub/idgames/idstuff/heretic/) and
[Hexen index](https://www.gamers.org/pub/idgames/idstuff/hexen/) confirm the
archive names. The [3D Realms mirror index](https://www.gamers.org/pub/mirrors/ftp.3drealms.com/share/)
was inaccessible to the web tool; the pinned packages themselves supply the
vendor terms. Decisions from reading those documents:

- Apogee VENDOR.DOC [3][A] requires all files unchanged; [3][C] requires prior
  signed permission for software/hardware bundles and CD-ROM distribution.
  Terminal Velocity LICENSE.TXT qualifies [3][C] with commercial distribution;
  its key points also restrict commercial locations after May 1, 1996.
- Shadow Warrior LICENSE.TXT grants no-charge individual sharing and specific
  online-service distribution, with every file unchanged. Its expired GTIS
  exclusivity does not grant general OS bundling permission.
- Heretic LICENSE.DOC permits royalty-free electronic distribution in
  compressed form, prohibits commercial exploitation, and refers vendors to
  VENDOR.DOC. This does not establish permission for an installed OS bundle.
- Hexen README.TXT identifies the October 18, 1995 four-level demo, explicitly
  distinguishes it from shareware, and contains no redistribution grant.
  No separate licence/vendor document exists in the ZIP; record this absence
  rather than substituting Heretic's terms.
- Descent LICENCE.TXT and Descent II README.TXT allow personal noncommercial
  copies and no-cost sharing with friends/acquaintances; their limitations
  expressly exclude bundling and promotional giveaways with another product.
- Jazz LICENSE.DOC allows unchanged modem distribution, single-game floppy
  sales below $9, small commercial CD/multi-disk collections with at most ten
  programs and three Epic titles plus packaging/credit/sample obligations,
  and free unchanged bundling with new hardware. These are conditional bundle
  grants for lead review, not an established grant for this OS image.

Container inspection used existing ZIP CRC checks and `shr_members` decoding
in memory. Heretic's two numbered parts concatenate to a ZIP self-extractor;
Cosmo and Bio Menace use the Apogee DCL SHR format; the other seven SHR
containers are ZIP self-extractors. Hexen and Jazz already contain direct game
files. These twelve payloads can be extracted without running a DOS installer,
though real sound configuration and qualification remain outstanding. Descent
and Descent II contain INSTALL.EXE and segmented SOW data; their installed
executables cannot be confirmed from the outer member list and require the
lead's original-installer run. No unmeasured automation or installed pins are
invented. The tracked runner retains its existing pending ZIP definitions;
installer kind and the required lead action are documented in provenance and
install steps until actual installer automation is calibrated.

Blake Stone 3.0 contains BS_AOG.EXE and BSTONE.BAT, not BLAKE.EXE; use the
observed executable. Bio Menace is 1.1, not a later freeware edition. Jazz's
FILE_ID.DIZ says 1.1 while VENDOR.DOC still says 1.0; retain both observations.
A completed document review does not imply installation or gameplay evidence.

## fx-games20c: original installation and extraction references

G20-1 requires the original installer wherever present, followed by the game's
own sound configuration. A host extraction is a read-back reference only.
Descent and Descent II SOW segments are excluded from host extraction. Direct
ZIPs (Hexen and Jazz) have no original installer in these editions.

Research before implementation compared the pinned packages and the existing
`build_games_disk.py` decoders with [PKWARE APPNOTE 6.3.10](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT),
[zlib's DCL decoder](https://github.com/madler/zlib/blob/develop/contrib/blast/blast.c)
and the [Creative hardware programming guide](https://www.phatcode.net/res/243/files/sbhwpg.pdf).
Reuse the existing DCL and LHA decoders; validate bounded ZIP paths and sizes
before extracting, including SFX and concatenated parts. ZIP extraction must
check CRC and reject links, duplicate DOS names and traversal. Every reference
retains all payload members, including documents excluded from older bundled
images. No reference becomes installed-tree evidence automatically.

The original manuals/readmes and their hashes are cited per title in
`tests/dos-titles/install20.json`. Executable help viewers need lead inspection;
compressed program strings are not a substitute for their rendered manual.
Only documented commands/keys may appear in proposed steps. Unknown desktop,
installer destination, sound menu and save/exit screens are explicitly
`needs-lead-calibration`; pending cases cannot grant acceptance.
The local Windows 98 Resource Kit `wrkc25_3.htm`, "Understanding the Apps.inf
File" and "Running Games", confirms per-program DOS settings; this change
reuses the desktop DOS VM path. It does not introduce NTVDM assumptions.
Sound selection uses the title-battery contract: SB16 220h IRQ5 DMA1/5,
AdLib/OPL music or MPU-401 330h General MIDI where supported. Hardware choices
are targets to verify on screen, never assumed menu indices.

Usage (all commands below are host-only):

```sh
python3 -B scripts/test/dos_games.py extract --title doom --archive-dir /path/to/read-only/pinned-cache
python3 -B scripts/test/dos_games.py pin-installed --title doom
python3 -B scripts/test/dos_games.py compare-installed --title doom --archive-dir /path/to/read-only/pinned-cache
python3 -B scripts/test/dos_games.py write-cases
```

`extract` writes `build/dos-games-reference/ID/{manifest.json,payload/}` and
refuses an existing destination. Its manifest labels the result
`extraction-reference-only`. It neither fills `ID-installed` nor updates pins.
`--archive-dir` is an explicit read-only input override; without it the normal
cache and existing package fallback are used. Original archive pins are always
checked before decompression. No download, DOS execution or SFX execution occurs.

Only after the lead's actual DOS installer/setup run, copy its measured target
contents to `build/downloads/dos-titles/ID-installed` using uppercase DOS paths,
then `pin-installed`. This command records bytes; it does not certify their
origin. Keep the guest run/image evidence with the lead's qualification record.
`compare-installed` rebuilds the reference in memory from the pinned archive
and rehashes the measured directory against those installed pins. The report
lists identical files, missing/changed/extra errors, original outer media and
exact exceptions. Exit 1 means a mismatch. Every exception must be entered
explicitly in the title's `comparison` object in `install20.json` after review:
`reference_only` for omitted original files, `installer_only` for added installer
outputs, and `config` for generated or modified configuration. No filename
suffix or wildcard exempts data; a stale exception fails. Do not exempt the
game executable. The default empty exception lists intentionally fail until
actual installer/config differences have been reviewed. SOW comparisons refuse
as not extractable rather than assert a reference match.

Twenty `acc-install-ID` cases preserve the cited screen-by-screen proposals.
They have unconditional pending declarations and are refused before a guest
launch. They must remain blocked until the lead has reviewed the executable
manuals, calibrated the named desktop/installer/sound/return screens, and
replaced the proposal with image-bound execution/evidence steps. The separate
gameplay cases still require measured installed pins and gameplay calibration.
Their overlay media, batch working directory and shortcut now consistently use
`C:\GAME\TARGET`, matching the original installation destination.

A wiring check remains essential during calibration: the general desktop VM
still initializes `DOSVM_SB_IRQ` to 7 (`dosvm_audio_state.h`); the older title
battery explicitly programs IRQ5 through the mixer. An environment variable
alone does not program mixer register 80h. The lead must verify IRQ5 in the
desktop installation VM (and keep it consistent when launching the installed
tree), or direct the scoped wiring implementation before accepting sound.
This change does not change the general VM's IRQ default or claim measured
IRQ5 support for the desktop cases.

Required pending cases set `pending.blocking: true`; precheck emits
`required pending` and the summary remains INCOMPLETE. This avoids the ordinary
pending-item exemption used for future optional work. The flag defaults to
false for existing pending cases. Synthetic tests cover both precheck refusal
and the runner/summary path without starting any guest.

All 18 exact pinned payloads passed the host read-back audit (including every
original document and nested Quake directory); all 42 cited package-member
hashes matched. Both SOW archives were pin-checked and excluded as specified.
The audit removed the generated reference bytes immediately. The LHA format
was also checked against the [upstream header specification](https://github.com/jca02266/lha/blob/master/header.doc.md).
FAT-created ZIPs can carry non-POSIX high attribute bits (observed in the
Wolfenstein archive); APPNOTE 4.4.2/4.4.15 makes attributes host-dependent.
Therefore Unix file-type/link validation applies to Unix-created entries;
DOS paths, budgets, CRCs and pin checks apply to every entry.
