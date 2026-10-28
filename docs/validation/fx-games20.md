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
   `ID-installed` directory (uppercase DOS 8.3 paths). Select SB16 220h/IRQ7/
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
