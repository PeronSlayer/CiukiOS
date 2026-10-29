# fx-games20 Playbook — Session A

> **Draft (2026-10-29, Antigravity gemini-3.8-flash-high, host-only reading of the package documents and binary strings).** Nothing here was observed on screen. The lead verifies every screen and key during the QEMU calibration of `docs/validation/fx-games20.md` steps 3–5; a `documented` mark means sourced, not verified.

- **Author:** Antigravity (`gemini-3.8-flash-high`, host research)
- **Date:** 2026-10-29 11:24 CET
- **Scope:** 10 DOS titles in Session A: `doom`, `quake`, `wolf3d`, `rott`, `keen4`, `duke3d`, `heretic`, `hexen`, `descent`, `descent2`.
- **References:** [fx-games-playbook.md](../../docs/directives/fx-games-playbook.md), [fx-games20.md](../../docs/validation/fx-games20.md), [install20.json](../../tests/dos-titles/install20.json), [games20.json](../../tests/dos-titles/games20.json), [dos-titles.json](../../config/dos-titles.json).

---

## 1. DOOM (`doom`)

### 1.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title doom`
- **Output Directory:** `build/dos-games-reference/doom/payload` (20 files)
- **Members:** `DM.DOC`, `DM.EXE`, `DMFAQ66A.TXT`, `DMFAQ66B.TXT`, `DMFAQ66C.TXT`, `DMFAQ66D.TXT`, `DOOM.EXE`, `DOOM1.WAD`, `DWANGO.DOC`, `DWANGO.EXE`, `DWANGO.STR`, `HELPME.TXT`, `IPXSETUP.EXE`, `MODEM.CFG`, `MODEM.NUM`, `MODEM.STR`, `ORDER.FRM`, `README.TXT`, `SERSETUP.EXE`, `SETUP.EXE`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 1.2 Destination
- `C:\GAME\DOOM` (staged from `C:\MEDIA\DOOM`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L6-L7))

### 1.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\DOOM\INSTALL.BAT`
- **Outer Archive:** `third_party/games/doom-shareware/doom19s.zip`
- **Step 1 — DEICE.EXE Execution:**
  - `INSTALL.BAT` invokes `DEICE.EXE` which reconstructs `DOOMS_19.EXE` from split members `DOOMS_19.1` and `DOOMS_19.2`.
  - On-screen text: de-ICE progress meter (`de-ICE v1.4 (C) 1995 id Software`).
  - Keys: No keystroke required; runs automatically.
  - Confidence: `documented` (`INSTALL.BAT` line 2; `strings` from `DEICE.EXE`).
- **Step 2 — DOOMS_19.EXE Unpack:**
  - `INSTALL.BAT` executes `DOOMS_19.EXE` (LHA SFX).
  - Default configured path: `\DOOMS` (`DOOMS_19.DAT` line 1: `PATH=\DOOMS`).
  - Target destination prompt: change destination directory to `C:\GAME\DOOM`.
  - Keys: Edit string to `C:\GAME\DOOM`, press `ret`.
  - Confidence: `documented` (`DOOMS_19.DAT` lines 1-7; `install20.json` line 40).
- **Step 3 — Post-Install Batch Transition:**
  - `INSTALL.BAT` deletes `DOOMS_19.EXE` (`DEL DOOMS_19.EXE`) and automatically launches `SETUP.EXE`.
  - Keys: Automatic transition into `SETUP.EXE`.
  - Confidence: `documented` (`INSTALL.BAT` line 6).

### 1.4 Sound Setup
- **Program:** `C:\GAME\DOOM\SETUP.EXE` (LZEXE-compressed Borland C++ Turbo Vision binary).
- **Target Settings:** SB16 effects at 220h, IRQ 5, DMA 1; OPL/AdLib music.
- **Menu Route:**
  1. `Main Menu`:
     - Options: `Choose Music Sound Card`, `Choose Sound FX Card`, `Select Controller Type`, `Configure Controller`, `Save parameters and launch DOOM`, `Run a Network/Modem/Serial game`, `Restore a Network/Modem/Serial savegame`.
     - Press `down` once to highlight `Choose Sound FX Card`, press `ret`.
  2. `Select Sound FX Device`:
     - List: `Gravis UltraSound`, `Pro Audio Spectrum`, `Sound Blaster`, `PC Speaker`, `NO SOUND FX`.
     - Press `down` twice to highlight `Sound Blaster`, press `ret`.
  3. `Select Port Address`:
     - List: `210`, `220`, `230`, `240`, `250`, `260`, `280`.
     - Highlight `220` (default or `down`), press `ret`.
  4. `Select IRQ`:
     - List: `2`, `3`, `5`, `7`.
     - Press `down` twice to highlight `5`, press `ret`.
  5. `Select DMA channel`:
     - List: `1`, `3`, `5`, etc. Highlight `1`, press `ret`.
  6. `Sound FX to mix`:
     - Enter number (e.g. `4` or `8`), press `ret`. Returns to `Main Menu`.
  7. `Choose Music Sound Card`:
     - From `Main Menu`, highlight `Choose Music Sound Card` (press `up`), press `ret`.
  8. `Select Music Card`:
     - List: `General MIDI`, `Sound Blaster AWE32`, `Sound Canvas`, `WaveBlaster`, `Gravis UltraSound`, `Pro Audio Spectrum`, `Sound Blaster`, `Adlib`, `NO MUSIC`.
     - Highlight `Sound Blaster` (or `Adlib` for standard OPL2/OPL3 FM music), press `ret`.
     - Returns to `Main Menu`.
  9. `Save and Exit`:
     - Press `F10` (`Accept` / save) or select `Save parameters and launch DOOM`. Press `ret`.
- **Files Written:** `DEFAULT.CFG` (contains `snd_sfxdevice`, `snd_musicdevice`, `snd_sbport 544` [0x220], `snd_sbirq 5`, `snd_sbdma 1`, `snd_mport -1`).
- **Confidence:** `strings` (recovered directly from `SETUP.EXE` via LZEXE decompression) and `documented` ([README.TXT](../../build/dos-games-reference/doom/payload/README.TXT#L102-L118)).

### 1.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips demo/intro; Enter opens menu, selects New Game, Episode 1, Hurt Me Plenty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl fires weapon).
- **Quit:** `['f10', 'y']` (F10 opens Quit verification prompt, 'y' confirms return to DOS).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/doom/payload/README.TXT#L277-L280) and [games20.json](../../tests/dos-titles/games20.json#L8-L11)).

### 1.6 Network / IPX Setup
- **Executable:** `C:\GAME\DOOM\IPXSETUP.EXE` ("DOOM Network Device Driver v1.22").
- **Parameters:**
  - `-nodes <#>`: sets number of players (default 2).
  - `-port <#>`: sets network socket (default 0x869C / 34460).
  - Optional: `-deathmatch`, `-altdeath`, `-skill <1-5>`, `-episode <1-3>`, `-warp <E> <M>`, `-nomonsters`, `-respawn`.
- **Execution Model:** Peer-to-peer rendezvous. Both host and join nodes execute `IPXSETUP.EXE -nodes 2` (optionally `-port <port>`). The driver reports `Looking for 2 players...`, listens and broadcasts over IPX until all nodes acknowledge, then launches `DOOM.EXE -net 1 2 ...`.
- **GUI Alternative:** Can be configured from `SETUP.EXE` -> `Run a Network/Modem/Serial game` -> `IPX-compatible Network` -> `Number of Players` -> `F10` (execs `IPXSETUP.EXE`).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/doom/payload/README.TXT#L295-L308)) and `strings` (`IPXSETUP.EXE`).

---

## 2. QUAKE (`quake`)

### 2.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title quake`
- **Output Directory:** `build/dos-games-reference/quake/payload` (16 files)
- **Members:** `CWSDPMI.EXE`, `GENVXD.DLL`, `HELP.TXT`, `ID1/PAK0.PAK`, `LICINFO.TXT`, `MGENVXD.VXD`, `ORDER.TXT`, `PDIPX.COM`, `Q95.BAT`, `QLAUNCH.EXE`, `QUAKE.EXE`, `QUAKEUDP.DLL`, `README.TXT`, `READV106.TXT`, `SLICNSE.TXT`, `TECHINFO.TXT`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 2.2 Destination
- `C:\GAME\QUAKE` (staged from `C:\MEDIA\QUAKE`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L65-L66))

### 2.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\QUAKE\INSTALL.BAT`
- **Outer Archive:** `third_party/games/quake-shareware/quake106.zip`
- **Step 1 — DEICE.EXE Execution:**
  - Reconstructs `RESOURCE.EXE` from `RESOURCE.1`.
  - Default path in `RESOURCE.DAT`: `PATH=\QUAKE_SW`.
  - Keys: Automatic.
  - Confidence: `documented` (`INSTALL.BAT` line 2; `RESOURCE.DAT` line 1).
- **Step 2 — RESOURCE.EXE Unpack:**
  - `RESOURCE.EXE` is an LHA self-extracting archive (`LHA's SFX 2.13L (c) Yoshi, 1991`).
  - Extracts game files and `ID1/PAK0.PAK` into target destination `C:\GAME\QUAKE`.
  - Keys: Press `ret` at path prompt if required.
  - Confidence: `strings` (`RESOURCE.1` offset 0-1945).
- **Step 3 — Post-Install Pause and Game Launch:**
  - On-screen text: `Prepare to enter Quake! Press any key to continue . . .`
  - Keys: Press `ret` (or space) to clear `pause`.
  - `INSTALL.BAT` invokes `quake` (`QUAKE.EXE`).
  - Confidence: `documented` (`INSTALL.BAT` lines 6-8).

### 2.4 Sound Setup
- **Program:** In-engine `C:\GAME\QUAKE\QUAKE.EXE` (no standalone setup utility).
- **Detection & Environment:**
  - Quake automatically detects Sound Blaster 16 hardware via the standard DOS environment variable:
    `SET BLASTER=A220 I5 D1 H5 P330 T6`
  - Quake's sound driver directly inspects port `A220`, IRQ `I5`, low DMA `D1`, and high 16-bit DMA `H5`.
- **In-Game Adjustment:**
  - In-game Options Menu: `Esc` -> Options -> Sound Volume / CD Music Volume.
  - In-game Console commands: `soundinfo` (displays DMA buffer address, DMA channel, sample rate, stereo mode), `sbinfo` (verifies Sound Blaster 16 DSP version and detected port).
- **Files Written:** `ID1/CONFIG.CFG` (written upon normal game exit; contains `volume`, `bgmvolume`, `_snd_mixahead`, controls).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/quake/payload/README.TXT#L87-L122) and [TECHINFO.TXT](../../build/dos-games-reference/quake/payload/TECHINFO.TXT#L611-L710)).

### 2.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips opening demo, opens Main Menu, Enter selects Single Player, New Game, Normal difficulty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl attacks/fires).
- **Quit:** `['grave_accent', 'q', 'u', 'i', 't', 'ret']` (tilde/backtick opens Quake console, type `quit` followed by Enter).
- **Confidence:** `documented` ([TECHINFO.TXT](../../build/dos-games-reference/quake/payload/TECHINFO.TXT#L862-L865) and [games20.json](../../tests/dos-titles/games20.json#L24-L27)).

### 2.6 Network / IPX Setup
- **Executable:** Built into `QUAKE.EXE`. For packet-driver environments, Quake includes the `PDIPX.COM` shim.
- **Parameters:**
  - `-listen <#>`: starts a listen server hosting up to `<#>` players (default 4; e.g. `QUAKE -listen 2`).
  - `-connect "<ipx_address>"`: connects client to host IPX address (e.g. `QUAKE -connect "00FADE23:00aa00b9b5b2"`).
  - `-ipxport <port#>`: overrides default IPX port (default 26000).
- **In-Game Menu Alternative:**
  - Main Menu -> `Multiplayer`:
    - `New Game`: Select IPX protocol, set Port (26000), Max Players, press Begin.
    - `Join Game`: Search for local IPX games, select host server, press Enter.
- **Confidence:** `documented` ([TECHINFO.TXT](../../build/dos-games-reference/quake/payload/TECHINFO.TXT#L984-L1007)).

---

## 3. WOLFENSTEIN 3D (`wolf3d`)

### 3.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title wolf3d`
- **Output Directory:** `build/dos-games-reference/wolf3d/payload` (12 files)
- **Members:** `AUDIOHED.WL1`, `AUDIOT.WL1`, `GAMEMAPS.WL1`, `MAPHEAD.WL1`, `ORDER.FRM`, `VENDOR.DOC`, `VGADICT.WL1`, `VGAGRAPH.WL1`, `VGAHEAD.WL1`, `VSWAP.WL1`, `W3DHELP.EXE`, `WOLF3D.EXE`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 3.2 Destination
- `C:\GAME\WOLF3D` (staged from `C:\MEDIA\WOLF3D`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L127-L128))

### 3.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\WOLF3D\INSTALL.EXE`
- **Outer Archive:** `third_party/games/wolf3d-shareware/1wolf14.zip`
- **Step 1 — Component Confirmation:**
  - Banner: `Wolfenstein 3D v1.4 Shareware Episode` / `Distributed by Apogee Software`.
  - On-screen component checkbox: `[X] Install Wolfenstein3D v1.4 Shareware Episode (1.3Mb of Hard Drive Space Required)`.
  - Keys: Press `ret` to accept.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 2 — Destination Directory:**
  - Prompt: `DEFAULTDIR: "C:\WOLF3D", prompt="Please select a drive and directory to install this game to, and press [Enter]."`
  - Keys: Backspace to clear `C:\WOLF3D`, type `C:\GAME\WOLF3D`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 3 — Extraction:**
  - Unpacks `W3DSW14.SHR` (PKWARE DCL compression) with progress gauge. Automatic.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 4 — Completion Screen:**
  - Text: `Thanks for installing the shareware episode of Wolfenstein 3D! To play the game, run WOLF3D.EXE from the game directory. By default, this is C:\WOLF3D.`
  - Prompt: `[Press any key to continue.]`
  - Keys: Press `ret` to exit installer to DOS prompt.
  - Confidence: `strings` (`INSTALL.EXE`).

### 3.4 Sound Setup
- **Program:** In-game Sound Menu within `C:\GAME\WOLF3D\WOLF3D.EXE`.
- **Target Settings:** Sound Blaster effects and AdLib FM music at 220h / IRQ 5 / DMA 1 (read from `BLASTER` environment variable).
- **Menu Route:**
  1. Title / Demo screen: Press `esc` to enter Main Menu.
  2. Main Menu: Highlight `Sound` (or `Sound Menu`), press `ret`.
  3. Sound Options:
     - `Sound Effects`: Options `None`, `PC Speaker`, `AdLib/Sound Blaster`. Toggle/select `AdLib/Sound Blaster`.
     - `Digitized Sound`: Options `None`, `Disney Sound Source`, `Sound Blaster`. Toggle/select `Sound Blaster`.
     - `Music`: Options `None`, `AdLib/Sound Blaster`. Toggle/select `AdLib/Sound Blaster`.
  4. Press `esc` to return to Main Menu.
- **Files Written:** `CONFIG.WL1` (stores sound effect mode, digitized sound mode, music mode, joystick/mouse settings).
- **Confidence:** `strings` (decompressed from `WOLF3D.EXE` via LZEXE) and `documented` ([W3DHELP.EXE](../../build/dos-games-reference/wolf3d/payload/W3DHELP.EXE)).

### 3.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips title/Apogee screens; Enter selects New Game, Episode 1: Escape from Wolfenstein, Can I play, Daddy? difficulty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl fires weapon).
- **Quit:** `['f10', 'y']` (F10 opens quit prompt, 'y' confirms return to DOS).
- **Confidence:** `documented` ([W3DHELP.EXE](../../build/dos-games-reference/wolf3d/payload/W3DHELP.EXE) and [games20.json](../../tests/dos-titles/games20.json#L38-L41)).

### 3.6 Network / IPX Setup
- Wolfenstein 3D v1.4 is single-player only (`lan: null`). No network driver or multiplayer parameters.
- **Confidence:** `documented` ([games20.json](../../tests/dos-titles/games20.json#L42)).

---

## 4. RISE OF THE TRIAD (`rott`)

### 4.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title rott`
- **Output Directory:** `build/dos-games-reference/rott/payload` (18 files)
- **Members:** `DEMO1_3.DMO`, `DEMO2_3.DMO`, `DEMO3_3.DMO`, `DEMO4_3.DMO`, `HUNTBGIN.RTC`, `HUNTBGIN.RTL`, `HUNTBGIN.WAD`, `MODEM.PCK`, `ORDER.FRM`, `README.EXE`, `REMOTE1.RTS`, `ROTT.EXE`, `ROTTHELP.EXE`, `ROTTIPX.EXE`, `ROTTSER.EXE`, `SETUP.EXE`, `SNDSETUP.EXE`, `VENDOR.DOC`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 4.2 Destination
- `C:\GAME\ROTT` (staged from `C:\MEDIA\ROTT`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L183-L184))

### 4.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\ROTT\INSTALL.EXE`
- **Outer Archive:** `build/downloads/dos-titles/1rott13.zip`
- **Step 1 — Component Confirmation:**
  - Title: `Rise of the Triad v1.3 Shareware Episode` / `(c) 1995 Apogee Software`.
  - Component selection: `[X] Install Rise of the Triad v1.3 Shareware (8.1Mb of Hard Drive Space Required)`.
  - Keys: Press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 2 — Destination Directory:**
  - Prompt: `DEFAULTDIR: "C:\ROTT"`.
  - Keys: Edit string to `C:\GAME\ROTT`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 3 — Extraction:**
  - Unpacks `ROTTSW13.SHR` (ZIP-SFX) with progress bar. Automatic.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 4 — Completion Screen:**
  - Text: `Thanks for installing the shareware episode of Rise of the Triad! To do this, run SETUP.EXE from the game directory. By default, this is C:\ROTT. After setup, you need to run ROTT.EXE to play!`
  - Prompt: `[Press any key to continue.]`
  - Keys: Press `ret` to exit to DOS prompt.
  - Confidence: `strings` (`INSTALL.EXE`).

### 4.4 Sound Setup
- **Program:** `C:\GAME\ROTT\SNDSETUP.EXE` (which invokes `ROTT.EXE SOUNDSETUP`).
- **Target Settings:** SB16 effects at 220h, IRQ 5, 8-bit DMA 1, 16-bit DMA 5; Sound Blaster FM music.
- **Menu Route:**
  1. `Sound Setup`:
     - Select `Select Sound FX Card`, press `ret`.
  2. `Sound FX Card`:
     - List: None, PC Speaker, Sound Blaster, Gravis UltraSound, Pro Audio Spectrum, General MIDI.
     - Select `Sound Blaster`, press `ret`.
  3. `Sound Blaster Setup`:
     - `Select Sound Blaster Type`: Select `Sound Blaster 16 or AWE32`, press `ret`.
     - `Select Sound Blaster Port`: Select `220`, press `ret`.
     - `Select Sound Blaster IRQ`: Select `5`, press `ret`.
     - `Select 8-bit DMA Channel`: Select `1`, press `ret`.
     - `Select 16-bit DMA Channel`: Select `5`, press `ret`.
     - Press `esc` to return to `Sound Setup`.
  4. `Select Music Card`:
     - Select `Sound Blaster` (for AdLib/OPL FM synthesis) or `General MIDI`. Press `ret`.
  5. `Save Changes`:
     - Press `esc`. Prompt: `Do you wish to save your changes?`
     - Select `Yes` (press `y` or `ret`).
- **Files Written:** `SOUND.ROT` (and `ROTT.ROT` / `CONFIG.ROT`).
- **Confidence:** `strings` (recovered from `ROTT.EXE` and `SETUP.EXE` via LZEXE decompression).

### 4.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips Apogee/Dopefish/intro animations, Enter selects New Game, Episode 1: Hunt Begins, difficulty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl fires weapon).
- **Quit:** `['esc', 'up', 'ret', 'y']` (ESC opens Main Menu, Up arrow selects Quit to DOS at bottom of menu, Enter selects, 'y' confirms).
- **Confidence:** `documented` ([ROTTHELP.EXE](../../build/dos-games-reference/rott/payload/ROTTHELP.EXE) and [games20.json](../../tests/dos-titles/games20.json#L53-L56)).

### 4.6 Network / IPX Setup
- **Executable:** `C:\GAME\ROTT\ROTTIPX.EXE` ("ROTT NETWORK DEVICE DRIVER").
- **Parameters:**
  - Server Mode (Host): `ROTTIPX.EXE -server -nodes 2` (optionally `-socket <hex>`).
  - Client Mode (Join): `ROTTIPX.EXE` (optionally `-socket <hex>`).
- **Operation:**
  - Server node displays `SERVER MODE: Attempting to find all clients for 2 player NETROTT`.
  - Client node displays `CLIENT MODE: Attempting to find server`.
  - Once synchronized, driver launches `ROTT.EXE -net player <n> ...`.
- **Menu Alternative:** `SETUP.EXE` -> `Start a game of ROTT on an IPX network` -> choose `Start as a server` or `Start as a client`, enter number of players (`2`).
- **Confidence:** `strings` (`ROTTIPX.EXE`, `SETUP.EXE`) and `documented` ([ROTTHELP.EXE](../../build/dos-games-reference/rott/payload/ROTTHELP.EXE)).

---

## 5. COMMANDER KEEN 4 (`keen4`)

### 5.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title keen4`
- **Output Directory:** `build/dos-games-reference/keen4/payload` (7 files)
- **Members:** `AUDIO.CK4`, `DOPEFISH.ANS`, `EGAGRAPH.CK4`, `GAMEMAPS.CK4`, `KEEN4E.EXE`, `ORDER.FRM`, `VENDOR.DOC`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 5.2 Destination
- `C:\GAME\KEEN4` (staged from `C:\MEDIA\KEEN4`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L244-L245))

### 5.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\KEEN4\INSTALL.EXE`
- **Outer Archive:** `build/downloads/dos-titles/4keen.zip`
- **Step 1 — Component Confirmation:**
  - Title: `Commander Keen: Goodbye Galaxy! v1.4 Shareware Episode` / `Distributed by Apogee Software`.
  - Component selection: `[X] Install Commander Keen: Galaxy v1.4 Shareware (770Kb of Hard Drive Space Required)`.
  - Keys: Press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 2 — Destination Directory:**
  - Prompt: `DEFAULTDIR: "C:\KEEN"`.
  - Keys: Edit string to `C:\GAME\KEEN4`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 3 — Extraction:**
  - Unpacks `CK4SW14.SHR` (PKWARE DCL compression) with progress gauge. Automatic.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 4 — Completion Screen:**
  - Text: `Thanks for installing the shareware episode of Commander Keen! To play the game, run KEEN4E.EXE from the game directory. By default, this is C:\KEEN.`
  - Prompt: `[Press any key to continue.]`
  - Keys: Press `ret` to return to DOS prompt.
  - Confidence: `strings` (`INSTALL.EXE`).

### 5.4 Sound Setup
- **Program:** In-game configuration within `C:\GAME\KEEN4\KEEN4E.EXE`.
- **Target Settings:** AdLib/Sound Blaster FM effects and music (standard AdLib OPL ports 388h / 220h sensed automatically).
- **Menu Route:**
  1. Title screen: Press `space` or `ret` to reach demo/game, press `F1` or `esc` to open Main Menu.
  2. Main Menu: Select `Configure` -> `Options` (or `Sound`).
  3. Sound Options:
     - `Sound Effects`: Options `NO SOUND EFFECTS`, `PC SPEAKER`, `ADLIB/SOUNDBLASTER`, `QUIET ADLIB/SOUNDBLASTER`. Select `ADLIB/SOUNDBLASTER`.
     - `Music`: Options `NO MUSIC`, `ADLIB/SOUNDBLASTER`. Select `ADLIB/SOUNDBLASTER`.
  4. Press `esc` to return to menu and game.
- **Files Written:** `CONFIG.CK4` (written upon exit).
- **Confidence:** `strings` (recovered from `KEEN4E.EXE` via LZEXE decompression).

### 5.5 Game Keys
- **Enter:** `['ret', 'ret', 'ret']` (press Enter to advance through title and credit screens into gameplay).
- **Play:** `['up', 'ctrl']` (Up arrow looks up / enters doors / activates switches; Ctrl jumps / pogos).
- **Quit:** `['esc', 'q', 'y']` (ESC opens menu, 'q' triggers Quit to DOS, 'y' confirms).
- **Confidence:** `documented` ([VENDOR.DOC](../../build/dos-games-reference/keen4/payload/VENDOR.DOC) and [games20.json](../../tests/dos-titles/games20.json#L67-L70)).

### 5.6 Network / IPX Setup
- Commander Keen 4 is single-player only (`lan: null`).
- **Confidence:** `documented` ([games20.json](../../tests/dos-titles/games20.json#L71)).

---

## 6. DUKE NUKEM 3D (`duke3d`)

### 6.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title duke3d`
- **Output Directory:** `build/dos-games-reference/duke3d/payload` (17 files)
- **Members:** `COMMIT.EXE`, `DEFS.CON`, `DEMO1.DMO`, `DEMO2.DMO`, `DEMO3.DMO`, `DN3DHELP.EXE`, `DUKE.RTS`, `DUKE3D.EXE`, `DUKE3D.GRP`, `GAME.CON`, `LICENSE.TXT`, `MODEM.PCK`, `README.DOC`, `SETMAIN.EXE`, `SETUP.EXE`, `ULTRAMID.INI`, `USER.CON`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 6.2 Destination
- `C:\GAME\DUKE3D` (staged from `C:\MEDIA\DUKE3D`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L300-L301))

### 6.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\DUKE3D\INSTALL.EXE`
- **Outer Archive:** `build/downloads/dos-titles/3dduke13.zip`
- **Step 1 — Component Confirmation:**
  - Title: `Duke Nukem 3D v1.3d Shareware Episode` / `(c) 1996 3D Realms`.
  - Component checkbox: `[X] Install Duke Nukem 3D v1.3d Shareware (12.8Mb of Hard Drive Space Required)`.
  - Keys: Press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 2 — Destination Directory:**
  - Prompt: `DEFAULTDIR: "C:\DUKE3D"`.
  - Keys: Edit string to `C:\GAME\DUKE3D`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 3 — Extraction:**
  - Unpacks `DN3DSW13.SHR` (ZIP-SFX) with progress bar. Automatic.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 4 — Completion Screen:**
  - Text: `Thanks for installing the shareware episode of Duke Nukem 3D! To do this, run SETUP.EXE from the duke game directory. By default, this is C:\DUKE3D.`
  - Prompt: `[Press any key to continue.]`
  - Keys: Press `ret` to exit to DOS prompt.
  - Confidence: `strings` (`INSTALL.EXE`).

### 6.4 Sound Setup
- **Program:** `C:\GAME\DUKE3D\SETUP.EXE` (spawns `SETMAIN.EXE`).
- **Target Settings:** SB16 effects at 220h, IRQ 5, 8-bit DMA 1, 16-bit DMA 5; Sound Blaster FM music (or General MIDI at 330h).
- **Menu Route:**
  1. `Main Menu`:
     - Highlight `Sound Setup` ("Setup sound and music for Duke Nukem 3D"), press `ret`.
  2. `Sound Setup Menu`:
     - Select `Choose Sound FX Card`, press `ret`.
     - List: None, Sound Blaster, Gravis Ultrasound, Sound Man 16, SoundScape, Disney Sound Source, Tandy Sound Source.
     - Highlight `Sound Blaster` ("You have a Sound Blaster, SB Pro, SB 16 or SB Compatible"), press `ret`.
  3. `Sound Blaster Configuration`:
     - `Sound Blaster Address`: Select `Set Sound Blaster Port Address to 0x220`, press `ret`.
     - `Sound Blaster Type`: Select `Sound Blaster 16 or AWE32`, press `ret`.
     - `Sound Blaster Interrupt`: Select `IRQ 5`, press `ret`.
     - `Sound Blaster 8-bit DMA`: Select `DMA channel 1`, press `ret`.
     - `Sound Blaster 16-bit DMA`: Select `DMA channel 5`, press `ret`.
     - Select `Use the settings above and continue with sound setup`, press `ret`.
  4. `Choose Music Card`:
     - Select `Choose Music Card`, press `ret`.
     - List: None, Sound Blaster, Ultrasound, Ensoniq SoundScape, Wave Blaster, General Midi, Sound Canvas.
     - Select `Sound Blaster` (for OPL FM synthesis) or `General Midi` (Midi Port 0x330). Press `ret`.
  5. `Save Settings`:
     - Press `esc` to return to `Main Menu`.
     - Highlight `Save Settings` ("Save current setup and exit setup program"), press `ret`.
- **Files Written:** `DUKE3D.CFG`.
- **Confidence:** `strings` (recovered from `SETMAIN.EXE` via LZEXE decompression) and `documented` ([DN3DHELP.EXE](../../build/dos-games-reference/duke3d/payload/DN3DHELP.EXE)).

### 6.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips 3D Realms logo and titles; Enter selects New Game, Episode 1: L.A. Meltdown, Piece of Cake / Let's Rock difficulty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl fires weapon / kicks).
- **Quit:** `['esc', 'up', 'ret', 'y']` (ESC opens menu, Up arrow selects Quit at bottom of list, Enter selects, 'y' confirms).
- **Confidence:** `documented` ([README.DOC](../../build/dos-games-reference/duke3d/payload/README.DOC) and [games20.json](../../tests/dos-titles/games20.json#L81-L84)).

### 6.6 Network / IPX Setup
- **Executable:** `C:\GAME\DUKE3D\COMMIT.EXE` (multiplayer launcher).
- **Operation:**
  - Launch via `SETUP.EXE` -> `Network Game`:
    - `Setup Network Socket Number` (sets IPX socket).
    - `Change number of players in network game` (sets 2 players).
    - `Launch a network game of Duke Nukem 3D`: launches `COMMIT.EXE -launch DUKE3D.EXE`.
  - Command line execution: `COMMIT.EXE` initializes IPX (`OpenIPXSocket`, `ListenForIPXPacket`), broadcasts `Attempting to find all players for a 2 player network game. Press ESC to exit.`, synchronizes nodes, and executes `DUKE3D.EXE -net player <n> ...`.
- **Confidence:** `strings` (`COMMIT.EXE`, `SETMAIN.EXE`) and `documented` ([DN3DHELP.EXE](../../build/dos-games-reference/duke3d/payload/DN3DHELP.EXE)).

---

## 7. HERETIC (`heretic`)

### 7.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title heretic`
- **Output Directory:** `build/dos-games-reference/heretic/payload` (22 files)
- **Members:** `DM.EXE`, `DM.TXT`, `DWANGO.EXE`, `DWANGO.SRV`, `DWANGO.STR`, `DWANGO.TMP`, `DWANGO.TXT`, `FILE_ID.DIZ`, `HELPME.TXT`, `HERETIC.EXE`, `HERETIC1.WAD`, `IPXSETUP.EXE`, `LICENSE.DOC`, `MODEM.CFG`, `MODEM.NUM`, `MODEM.STR`, `ORDER.FRM`, `README.TXT`, `SERSETUP.EXE`, `SETUP.EXE`, `VENDOR.DOC`, `VIOHT.EXE`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 7.2 Destination
- `C:\GAME\HERETIC` (staged from `C:\MEDIA\HERETIC`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L361-L362))

### 7.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\HERETIC\INSTALL.BAT`
- **Outer Archive:** `build/downloads/dos-titles/htic_v12.zip`
- **Step 1 — DEICE.EXE Execution:**
  - `INSTALL.BAT` executes `DEICE.EXE` which reconstructs `HTIC_V12.EXE` from parts `HTIC_V12.1` and `HTIC_V12.2`.
  - Keys: Automatic.
  - Confidence: `documented` (`INSTALL.BAT` line 2; `HTIC_V12.DAT`).
- **Step 2 — HTIC_V12.EXE Unpack:**
  - Executes `HTIC_V12.EXE` self-extractor. Default path in `HTIC_V12.DAT` is `PATH=\HERETIC`.
  - Keys: Edit destination to `C:\GAME\HERETIC`, press `ret`.
  - Confidence: `documented` (`HTIC_V12.DAT` lines 1-7).
- **Step 3 — Post-Install Batch Transition:**
  - Batch file removes `HTIC_V12.EXE` and automatically executes `SETUP.EXE`.
  - Keys: Automatic.
  - Confidence: `documented` (`INSTALL.BAT` lines 6-7).

### 7.4 Sound Setup
- **Program:** `C:\GAME\HERETIC\SETUP.EXE` (LZEXE-compressed Borland C++ utility).
- **Target Settings:** Sound Blaster effects at 220h, IRQ 5, DMA 1; Sound Blaster OPL FM music (or General MIDI at 330h).
- **Menu Route:**
  1. `Main Menu`:
     - Highlight `Choose Sound FX Card`, press `ret`.
  2. `Select Sound FX Device`:
     - Select `Sound Blaster`, press `ret`.
     - Port: Select `220`, press `ret`.
     - IRQ: Select `5`, press `ret`.
     - DMA: Select `1`, press `ret`.
     - Channels: Enter `8`, press `ret`. Returns to `Main Menu`.
  3. `Choose Music Sound Card`:
     - Highlight `Choose Music Sound Card`, press `ret`.
     - List: General MIDI, Sound Blaster AWE32, WaveBlaster, Gravis UltraSound, Sound Blaster, Adlib, NO MUSIC.
     - Select `Sound Blaster` (or `Adlib`), press `ret`.
  4. `Save and Exit`:
     - Press `F10` (or select `Save parameters and launch HERETIC`).
- **Files Written:** `HERETIC.CFG` (and `MODEM.CFG`).
- **Confidence:** `strings` (recovered from `SETUP.EXE` via LZEXE decompression) and `documented` ([README.TXT](../../build/dos-games-reference/heretic/payload/README.TXT)).

### 7.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips demo/intro, Enter opens menu, New Game, Episode 1: City of the Damned, Thou art a smite-meister difficulty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl fires weapon).
- **Quit:** `['f10', 'y']` (F10 opens quit prompt, 'y' confirms).
- **Confidence:** `documented` ([HELPME.TXT](../../build/dos-games-reference/heretic/payload/HELPME.TXT) and [games20.json](../../tests/dos-titles/games20.json#L95-L98)).

### 7.6 Network / IPX Setup
- **Executable:** `C:\GAME\HERETIC\IPXSETUP.EXE` ("HERETIC Network Device Driver v1.22").
- **Parameters:**
  - `-nodes <#>`: sets number of players (default 2).
  - `-port <#>`: sets socket.
  - Same peer-to-peer syntax as DOOM: both nodes run `IPXSETUP.EXE -nodes 2`.
- **GUI Alternative:** `DM.EXE` (DeathManager v1.4 included in payload) provides graphical IPX selection and launches `IPXSETUP.EXE`.
- **Confidence:** `documented` ([DM.TXT](../../build/dos-games-reference/heretic/payload/DM.TXT) / `README.TXT`) and `strings` (`IPXSETUP.EXE`).

---

## 8. HEXEN (`hexen`)

### 8.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title hexen`
- **Output Directory:** `build/dos-games-reference/hexen/payload` (11 files)
- **Members:** `DM.DOC`, `DM.EXE`, `HEXEN.EXE`, `HEXEN.WAD`, `IPXSETUP.EXE`, `MODEM.CFG`, `MODEM.NUM`, `MODEM.STR`, `README.TXT`, `SERSETUP.EXE`, `SETUP.EXE`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L150-L165))

### 8.2 Destination
- `C:\GAME\HEXEN` (staged directly into `C:\GAME\HEXEN`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L420-L421))

### 8.3 Installer Screens & Key Sequence
- **Installer Command:** None (direct ZIP package).
- `hexndemo.zip` is a flat archive containing the runnable demo files; no `INSTALL.BAT` or `INSTALL.EXE` exists.
- Runner stages files directly into `C:\GAME\HEXEN` (step `direct-zip` in `install20.json`).
- Confidence: `documented` ([install20.json](../../tests/dos-titles/install20.json#L429-L435)).

### 8.4 Sound Setup
- **Program:** `C:\GAME\HEXEN\SETUP.EXE` (LZEXE-compressed setup utility).
- **Target Settings:** Sound Blaster effects at 220h, IRQ 5, DMA 1; Sound Blaster FM music (or General MIDI at 330h).
- **Menu Route:**
  1. `Main Menu`:
     - Highlight `Choose Sound FX Card`, press `ret`.
  2. `Select Sound FX Device`:
     - Select `Sound Blaster`, press `ret`.
     - Port: `220`. IRQ: `5`. DMA: `1`. Channels: `8`. Returns to `Main Menu`.
  3. `Choose Music Sound Card`:
     - Highlight `Choose Music Sound Card`, press `ret`.
     - Select `Sound Blaster` (or `General MIDI`), press `ret`.
  4. `Save Settings`:
     - Press `F10` (or select `Save parameters and launch HEXEN`).
- **Files Written:** `HEXEN.CFG` (and `MODEM.CFG`).
- **Confidence:** `strings` (recovered from `SETUP.EXE` via LZEXE decompression) and `documented` ([README.TXT](../../build/dos-games-reference/hexen/payload/README.TXT)).

### 8.5 Game Keys
- **Enter:** `['esc', 'ret', 'ret', 'ret']` (ESC skips demo/intro, Enter opens menu, New Game, Class selection: Fighter/Cleric/Mage, Difficulty).
- **Play:** `['up', 'ctrl']` (Up arrow moves forward, Ctrl attacks/fires).
- **Quit:** `['f10', 'y']` (F10 opens quit prompt, 'y' confirms return to DOS).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/hexen/payload/README.TXT) and [games20.json](../../tests/dos-titles/games20.json#L109-L112)).

### 8.6 Network / IPX Setup
- **Executable:** `C:\GAME\HEXEN\IPXSETUP.EXE`.
- **Parameters:** `IPXSETUP.EXE -nodes 2` (optionally `-port <port>`).
- **GUI Manager:** `DM.EXE` (DeathManager v1.4 included in archive; documents multiplayer in `DM.DOC`).
- **Confidence:** `documented` ([DM.DOC](../../build/dos-games-reference/hexen/payload/DM.DOC)) and `strings` (`IPXSETUP.EXE`).

### 8.7 Hexen Terms (Draft for the Lead)
- **Status in games20.json:** `terms_reviewed: false` (must remain false until lead review).
- **Package Archive:** `build/downloads/dos-titles/hexndemo.zip` (size: 5664734, sha256: `6e6fc8e03e7c8ec1782cbcf078972e276f57ee1375d84c172bcad7ce26a42a27`).
- **Package Member Inventory:**
  - `HEXEN.EXE`: size 875885, sha256 `9a29f526d7e307ccb4157e9672c0542fd90c4393304bda354392fada7830de8a`
  - `HEXEN.WAD`: size 10644136, sha256 `c26d6e5ca01039d40f9e53fbc15a242616c633f4bbc0fca0b3f73b871101311d`
  - `SERSETUP.EXE`: size 20273, sha256 `a31765094000c3151129f87b3b51be43bb5a7b573c9fd428bababa361bc8aac9`
  - `IPXSETUP.EXE`: size 18003, sha256 `cc4d27016b970869235716d1c5c2d8d5ca56317c9187857858e04cfc5f733e68`
  - `MODEM.CFG`: size 67, sha256 `0eb17ba5df3f319a02325401c9fbe212a3aa9065d63d4b7bd49d75bd2bb0268b`
  - `MODEM.NUM`: size 67, sha256 `604a8033fa26c634a758006e318573206d123419fefc9df8cf76f2b1e67ba105`
  - `MODEM.STR`: size 3405, sha256 `84a5af6596ce180bdb03a0bee414421538c27df5b0722041213cec953099d1d2`
  - `DM.EXE`: size 21484, sha256 `cb51bfaa01ab2c2cb9c57b228159b0a5afd42258de95f8dcb6f83a494a3008d4`
  - `DM.DOC`: size 6437, sha256 `6a3f6d66880fe5d9b4798713385acf5d0f1a1d2711e8712aefbe486defae12f3`
  - `SETUP.EXE`: size 47682, sha256 `ed413dc72dea897416060892cbd9565e2db38f28c3d60ce62c5be9d1bdf20c1d`
  - `README.TXT`: size 3417, sha256 `052ab6d0f513ebef8484b359d3a744cb7af8a519b68b68361a02ba32eee3afdb`
- **Text & Licence Analysis:**
  - The package contains **no** `LICENSE.DOC`, `VENDOR.DOC`, or `ORDER.FRM`.
  - The only descriptive document is `README.TXT` (lines 1-13, 56-62). Verbatim clauses:
    ```
    Hexen: Beyond Heretic DEMO
    Re-released October 18th, 1995
    --------------------------------------
    Created by Raven Software (C) 1995
    Published by id Software
    Distributed by GT Interactive Software
    --------------------------------------
    Worldwide retail release date: Oct 30th
    Hexen is NOT a shareware product.

    Please direct all bug reports to support@gtinteractive.com. Do NOT email
    id Software or Raven Software with bug reports.

    This is a 4-level demo of the new action-adventure Hexen: Beyond Heretic...
    ...
    Enjoy the demo. Then rush out and get the game when it hits the stores!
    You won't be sorry.
    ```
  - **Findings for Lead Decision:** Unlike id's shareware titles (`doom`, `heretic`), which explicitly declare "FREELY DISTRIBUTE" or contain distributor guidelines, `hexndemo.zip` is a commercial retail demo distributed by GT Interactive Software with an explicit declaration: `Hexen is NOT a shareware product.` There are no express redistribution grants or vendor permissions in the package. Retaining `test-media` classification and keeping `terms_reviewed: false` until explicit owner/lead determination is fully justified.

---

## 9. DESCENT (`descent`)

### 9.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title descent`
- **Result:** `segmented_sow_not_extractable: descent` (host extract intentionally refuses multi-part segmented SOW archive).
- **Archive Members:** `build/downloads/dos-titles/desc14sw.zip` contains:
  - `INSTALL.EXE`: 44773 bytes, sha256 `36a8ef688941dd93ae07c1bb4be0fc8a176041a6ae930ba65780a18e583cbfec`
  - `DESCENT1.SOW`: 1411591 bytes (multi-volume ARJ archive part 1, `DESCENT.ARJ`)
  - `DESCENT2.SOW`: 1449231 bytes (multi-volume ARJ archive part 2, `DESCENT.A01`)
  - `LICENCE.TXT`: 2000 bytes, sha256 `c81d83dc08b98bdf20a1ceb15ce801932fbbfa6f5296fa2b8ba379201f92e542`
- **Internal SOW Payload Members:** `4GWBIND.EXE`, `4GWPRO.EXE`, `DESCENT.HOG`, `DESCENT.PIG`, `DESCENT.BAT`, `DESCENTG.INI`, `HMIDET.386`, `HMIDRV.386`, `HMIMDRV.386`, `ORDERFRM.TXT`, `README.TXT`, `REFCARD.TXT`, `SETUP.UBN`, `DESCENT.UBN`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L123-L135) and [fx-games20.md](../../docs/validation/fx-games20.md#L149-L167)).

### 9.2 Destination
- `C:\GAME\DESCENT` (staged from `C:\MEDIA\DESCENT`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L470-L471))

### 9.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\DESCENT\INSTALL.EXE`
- **Step 1 — Drive Selection:**
  - Banner: `Descent Installation (v1.4) -- Copyright(C) 1994, Parallax Software Corp.`
  - Menu: `Select Drive to Install to`. Highlight drive `C:`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 2 — Installation Directory:**
  - Prompt: `Select installation directory`. Default is `GAMES\DESCENT`.
  - Keys: Backspace/type `C:\GAME\DESCENT`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 3 — Install Confirmation:**
  - Menu option: `Install Descent` (or `Install Descent over existing files`). Highlight `Install Descent`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 4 — File Extraction:**
  - Screen displays `Extracting files from archives`. Unpacks `DESCENT1.SOW` and `DESCENT2.SOW`. Automatic.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 5 — Linear Executable Binding:**
  - `INSTALL.EXE` invokes `4GWBIND.EXE` to bind `SETUP.UBN` into `SETUP.EXE` and `DESCENT.UBN` into `DESCENT.EXE`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 6 — Setup Launch:**
  - Prompt: `Installation successful. Press any key to enter setup utility.`
  - Keys: Press `ret` (or any key) -> launches `SETUP.EXE`.
  - Confidence: `strings` (`INSTALL.EXE`) and `documented` ([README.TXT](../../build/dos-games-reference/descent/payload/README.TXT#L151-L155)).

### 9.4 Sound Setup
- **Program:** `C:\GAME\DESCENT\SETUP.EXE` (bound from `SETUP.UBN`, HMI Sound Operating System).
- **Target Settings:** Sound Blaster 16/AWE32 at 220h, IRQ 5, DMA 1; Sound Blaster FM music at 220h (or General MIDI at 330h).
- **Menu Route:**
  1. `Main Menu`:
     - Options: `Auto Detect Sound Hardware`, `Change Digital Sound Card`, `Change Music Card`, `Test Digital Sound Card`, `Test Music Card`.
     - Highlight `Change Digital Sound Card`, press `ret`.
  2. `Select Sound Card`:
     - List: Sound Blaster, Sound Blaster Pro, Sound Blaster 16/AWE32, Microsoft Sound System, Gravis Ultrasound, Gravis Ultrasound Max, Ensoniq Soundscape.
     - Highlight `Sound Blaster 16/AWE32`, press `ret`.
  3. Digital Parameters:
     - `Select Port`: Select `220`, press `ret`.
     - `Select IRQ`: Select `5`, press `ret`.
     - `Select DMA`: Select `1`, press `ret`.
     - Returns to `Main Menu`.
  4. `Change Music Card`:
     - Highlight `Change Music Card`, press `ret`.
     - List: Sound Blaster FM (OPL2), General MIDI, Sound Blaster AWE32, Sound Master II, Gravis Ultrasound, Adlib Gold, Ensoniq Soundscape.
     - Select `Sound Blaster FM (OPL2)` (or `General MIDI`), press `ret`.
     - Select Port: `220` (or `330` for General MIDI), press `ret`.
  5. `Save Settings`:
     - Press `esc` (or select Return / Exit). Prompt: `Save changes?`
     - Select `Yes` (press `y` or `ret`).
- **Files Written:** `DESCENT.CFG` (in `C:\GAME\DESCENT`).
- **Confidence:** `strings` (recovered directly from `SETUP.UBN`) and `documented` ([README.TXT](../../build/dos-games-reference/descent/payload/README.TXT#L151-L160)).

### 9.5 Game Keys
- **Enter:** `['ret', 'ret', 'ret', 'ret']` (press Enter through opening titles, credits, and briefing screens).
- **Play:** `['up', 'ctrl']` (Up arrow accelerates/moves forward, Ctrl fires primary laser cannons).
- **Quit:** `['esc', 'esc', 'up', 'ret', 'ret']` (ESC pauses/opens in-game menu, Up arrow selects Quit Game, Enter confirms, Enter confirms final dialog).
- **Confidence:** `documented` ([REFCARD.TXT](../../build/dos-games-reference/descent/payload/REFCARD.TXT) and [games20.json](../../tests/dos-titles/games20.json#L123-L126)).

### 9.6 Network / IPX Setup
- **Executable:** Built into `DESCENT.EXE` (uses DOS IPX interrupts `INT 2Fh AX=7A00h` / `INT 7Ah`).
- **In-Game Menu Route:**
  - Main Menu -> `Multiplayer Game` -> `IPX`:
    - `Start a Netgame` (Host): Select mission, starting level, game mode (Anarchy, Team Anarchy, Robo-Anarchy, Cooperative), press Enter to wait for players.
    - `Join a Netgame` (Join): Scans network on IPX socket, lists available active games, select game and press Enter.
  - Default IPX socket: `0x5100` (custom socket configurable with `-socket <hex>` parameter).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/descent/payload/README.TXT#L636-L700) and [f6-11-dos-network.md](../../docs/validation/f6-11-dos-network.md#L133-L141)).

---

## 10. DESCENT II (`descent2`)

### 10.1 Reference Tree
- **Extraction Command:** `python3 -B scripts/test/dos_games.py extract --title descent2`
- **Result:** `segmented_sow_not_extractable: descent2` (host extract refuses multi-part segmented SOW archive).
- **Archive Members:** `build/downloads/dos-titles/d2demo10.zip` contains:
  - `INSTALL.EXE`: 54348 bytes, sha256 `576dbe6db132d73352936746f369ef9d592db33605a914b1ba2f30663435c2fa`
  - `D2_1.SOW`: 1396460 bytes (multi-volume ARJ part 1)
  - `D2_2.SOW`: 1456292 bytes (multi-volume ARJ part 2)
  - `D2_3.SOW`: 1432314 bytes (multi-volume ARJ part 3)
  - `README.TXT`: 2061 bytes, sha256 `9bf7eb10fe7eefba624ef16ca11e039bf877c8e9f80b27b5003507d9b9365287`
  - `FILE_ID.DIZ`: 199 bytes
- **Internal SOW Payload Members:** `4GWBIND.EXE`, `4GWPRO.EXE`, `D2.BAT`, `D2DEMO.HOG`, `D2DEMO.HAM`, `D2DEMO.PIG`, `DESCENTG.INI`, `HMIDET.386`, `HMIDRV.386`, `HMIMDRV.386`, `README.TXT` (full 88 KiB manual), `ORDERFRM.TXT`, `D2DEMO.UBN`, `SETUP.UBN`.
- **Confidence:** `documented` ([dos_games.py](../../scripts/test/dos_games.py#L123-L135) and [fx-games20.md](../../docs/validation/fx-games20.md#L149-L167)).

### 10.2 Destination
- `C:\GAME\DESCENT2` (staged from `C:\MEDIA\DESCENT2`)
- **Confidence:** `documented` ([install20.json](../../tests/dos-titles/install20.json#L523-L524))

### 10.3 Installer Screens & Key Sequence
- **Installer Command:** `C:\MEDIA\DESCENT2\INSTALL.EXE`
- **Step 1 — Drive Selection:**
  - Banner: `Descent 2 Interactive Demo v1.0 Installation`.
  - Menu: `Select Drive to Install to`. Select `C:`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 2 — Installation Directory:**
  - Prompt: `Select installation directory` / `Install Descent 2 Demo to:`.
  - Keys: Enter `C:\GAME\DESCENT2`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 3 — Install Confirmation:**
  - Menu: `Install Descent 2 Demo` (or `Install Descent 2 Demo over existing files`). Highlight `Install Descent 2 Demo`, press `ret`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 4 — File Extraction:**
  - Progress: `Extracting files from archives` (unpacks `d2_1.sow`, `d2_2.sow`, `d2_3.sow`). Automatic.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 5 — Linear Executable Binding:**
  - `INSTALL.EXE` invokes `4GWBIND.EXE` to bind `setup.ubn` -> `setup.exe` and `d2demo.ubn` -> `d2demo.exe`.
  - Confidence: `strings` (`INSTALL.EXE`).
- **Step 6 — Setup Launch:**
  - Prompt: `Installation successful. Press any key to enter setup utility.`
  - Keys: Press `ret` (or any key) -> launches `SETUP.EXE`.
  - Confidence: `strings` (`INSTALL.EXE`) and `documented` ([README.TXT](../../build/dos-games-reference/descent2/payload/README.TXT#L453-L458)).

### 10.4 Sound Setup (Descent II Music Case)
- **Program:** `C:\GAME\DESCENT2\SETUP.EXE` (bound from `SETUP.UBN`, HMI Sound Operating System).
- **Target Settings (Explicit Contract):**
  - **Digital Sound Effects:** Sound Blaster 16/AWE32 at port 220h, IRQ 5, DMA 1 (and high DMA 5).
  - **Music:** General MIDI on MPU-401 at port 330h (Descent II music qualification case per `docs/validation/fx-games20.md`).
- **Menu Route:**
  1. `Main Menu`:
     - Highlight `Change Digital Sound Card`, press `ret`.
  2. `Select Sound Card`:
     - Highlight `Sound Blaster 16/AWE32`, press `ret`.
     - Port: `220`. IRQ: `5`. DMA: `1`. Press `ret`.
     - Returns to `Main Menu`.
  3. `Change Music Card`:
     - Highlight `Change Music Card`, press `ret`.
     - Highlight `General MIDI`, press `ret`.
     - Port: Select `330` (standard MPU-401 port), press `ret`.
     - Returns to `Main Menu`.
  4. `Save Settings`:
     - Select Return / Exit, press `esc`. Prompt: `Save changes?`
     - Select `Yes` (press `y` or `ret`).
- **Files Written:** `DESCENT.CFG` (in `C:\GAME\DESCENT2`).
- **Confidence:** `documented` ([fx-games20.md](../../docs/validation/fx-games20.md#L80), [fx-games-playbook.md](../../docs/directives/fx-games-playbook.md#L21), and [README.TXT](../../build/dos-games-reference/descent2/payload/README.TXT#L429-L460)).

### 10.5 Game Keys
- **Enter:** `['ret', 'ret', 'ret', 'ret']` (press Enter to clear opening splash, demo titles, and mission screens).
- **Play:** `['up', 'ctrl']` (Up arrow accelerates/moves forward, Ctrl fires lasers).
- **Quit:** `['esc', 'esc', 'up', 'ret', 'ret']` (ESC pauses/opens menu, Up selects Quit Game, Enter confirms, Enter confirms final dialog).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/descent2/payload/README.TXT) and [games20.json](../../tests/dos-titles/games20.json#L137-L140)).

### 10.6 Network / IPX Setup
- **Executable:** Built into `D2DEMO.EXE` (uses DOS IPX interrupts `INT 2Fh AX=7A00h` / `INT 7Ah`).
- **In-Game Menu Route:**
  - Main Menu -> `Multiplayer Game` -> `IPX`:
    - `Start a Netgame` (Host) / `Join a Netgame` (Join).
    - Socket: Default 0x5100 (or custom socket via command line parameter `-socket <hex>`).
- **Confidence:** `documented` ([README.TXT](../../build/dos-games-reference/descent2/payload/README.TXT#L1179-L1350)).

---

## 11. Cross-Title Synthesis Matrix

| Title ID | Staged Media | Target Directory | Installer Executable | Sound Setup Utility | Snd FX Target | Music Target | Config File Written | IPX Executable |
|---|---|---|---|---|---|---|---|---|
| `doom` | `C:\MEDIA\DOOM` | `C:\GAME\DOOM` | `INSTALL.BAT` | `SETUP.EXE` | SB16 (220/5/1) | SB / AdLib OPL | `DEFAULT.CFG` | `IPXSETUP.EXE` |
| `quake` | `C:\MEDIA\QUAKE` | `C:\GAME\QUAKE` | `INSTALL.BAT` | In-engine (`QUAKE.EXE`) | SB16 (`BLASTER` env) | In-engine / CD | `ID1/CONFIG.CFG` | `QUAKE.EXE` |
| `wolf3d` | `C:\MEDIA\WOLF3D` | `C:\GAME\WOLF3D` | `INSTALL.EXE` | In-engine (`WOLF3D.EXE`) | SB (`BLASTER` env) | AdLib OPL | `CONFIG.WL1` | None |
| `rott` | `C:\MEDIA\ROTT` | `C:\GAME\ROTT` | `INSTALL.EXE` | `SNDSETUP.EXE` | SB16 (220/5/1/5) | SB / AdLib OPL | `SOUND.ROT` | `ROTTIPX.EXE` |
| `keen4` | `C:\MEDIA\KEEN4` | `C:\GAME\KEEN4` | `INSTALL.EXE` | In-engine (`KEEN4E.EXE`) | AdLib OPL | AdLib OPL | `CONFIG.CK4` | None |
| `duke3d` | `C:\MEDIA\DUKE3D` | `C:\GAME\DUKE3D` | `INSTALL.EXE` | `SETUP.EXE` (`SETMAIN`) | SB16 (220/5/1/5) | SB / AdLib OPL | `DUKE3D.CFG` | `COMMIT.EXE` |
| `heretic` | `C:\MEDIA\HERETIC` | `C:\GAME\HERETIC` | `INSTALL.BAT` | `SETUP.EXE` | SB16 (220/5/1) | SB / AdLib OPL | `HERETIC.CFG` | `IPXSETUP.EXE` |
| `hexen` | `C:\MEDIA\HEXEN` | `C:\GAME\HEXEN` | None (`direct-zip`) | `SETUP.EXE` | SB16 (220/5/1) | SB / AdLib OPL | `HEXEN.CFG` | `IPXSETUP.EXE` |
| `descent` | `C:\MEDIA\DESCENT` | `C:\GAME\DESCENT` | `INSTALL.EXE` | `SETUP.EXE` (`SETUP.UBN`) | SB16 (220/5/1) | SB FM (OPL2) | `DESCENT.CFG` | In-engine (`DESCENT.EXE`) |
| `descent2` | `C:\MEDIA\DESCENT2` | `C:\GAME\DESCENT2` | `INSTALL.EXE` | `SETUP.EXE` (`SETUP.UBN`) | SB16 (220/5/1) | General MIDI (330h) | `DESCENT.CFG` | In-engine (`D2DEMO.EXE`) |
