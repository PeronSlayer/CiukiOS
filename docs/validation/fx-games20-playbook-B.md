# Twenty DOS Games: Installer & Sound-Setup Playbook — Session B

> **Draft (2026-10-29, Antigravity gemini-3.8-flash-high, host-only reading of the package documents and binary strings).** Nothing here was observed on screen. The lead verifies every screen and key during the QEMU calibration of `docs/validation/fx-games20.md` steps 3–5; a `documented` mark means sourced, not verified.

- **Date:** 2026-10-29 11:20 CET
- **Scope:** Session B titles: `shadowwarrior`, `wacky`, `cosmo`, `duke2`, `raptor`, `hocus`, `biomenace`, `blakestone`, `terminal`, `jazz`
- **Purpose:** Host-only reverse-engineering and research of original DOS installation routines, sound setups, configuration files, gameplay keys, and IPX multiplayer drivers from pinned distribution archives. Serves as the calibrated playbook for the lead's automated and scripted QEMU qualification runs (`docs/validation/fx-games20.md`, `tests/dos-titles/install20.json`, `tests/dos-titles/games20.json`).
- **Research methodology:** Archive examination via `unzip -l` and `unzip -p`, reference extraction via `scripts/test/dos_games.py extract`, binary string recovery via `strings` on `INSTALL.EXE` and `SETUP.EXE`, static reachability/LZEXE inspection via `tests/host/dos/reachability.py`, and manual analysis of vendor documentation.

---

## 1. Shadow Warrior (`shadowwarrior`)

### Reference Tree Summary
- **Archive:** `3dsw12.zip` (13,299,147 bytes, SHA-256: `6e41b3769d0373fc160c2113469c6f8bc2932dc1a77b69c81c1bd9644a189a8f`)
- **Outer media (4 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `LICENSE.TXT`, `SWSW12.SHR`
- **Extraction recipe:** `zip-sfx` over `SWSW12.SHR`
- **Extracted payload (13 files):** `COMMIT.EXE`, `CREDITS.TXT`, `LICENSE.TXT`, `MODEM.PCK`, `ORDER.FRM`, `SETMAIN.EXE`, `SETUP.EXE`, `SW.EXE`, `SW.GRP`, `SW.RTS`, `SWHELP.EXE`, `TENGAME.INI`, `ULTRAMID.INI`
- **Destination directory:** `build/dos-games-reference/shadowwarrior/`

### Installer Playbook
- **Command:** `C:\MEDIA\SW\INSTALL.EXE` [documented: `install20.json:604`]
- **Destination:** `C:\GAME\SW` [documented: `install20.json:576`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Shadow Warrior v1.2 Shareware Episode`
     - Text:
       ```text
        Shadow Warrior v1.2 
         Shareware Episode
        (c) 1997 3D Realms 
       ```
     - Action / Keys: Press `Enter` (or any key) to proceed.
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Shadow Warrior v1.2 Shareware 
               29.6Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\SW`
     - Action / Keys: Backspace default, type `C:\GAME\SW`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Installer renames `SWSW12.SHR` to `SWSW12.EXE`, self-extracts files into `C:\GAME\SW`.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about Shadow Warrior, 
             please visit our Official Web page at:
                    http://www.3drealms.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
         Thanks for installing the shareware episode of Shadow Warrior!
         Before you play the game, you must first run the setup program.
                 To do this, run SETUP.EXE from the game directory.
                           By default, this is C:\SW.
           If you run into any trouble, please consult the swhelp.exe
             file that has been installed into your game directory.
       ```
     - Action / Keys: Press any key to exit to DOS. Installer executes `rename $sourcedir$\swsw12.exe swsw12.shr` and terminates cleanly.

### Sound Setup Playbook
- **Program:** `C:\GAME\SW\SETUP.EXE` (16-bit DPMI launcher that executes `SETMAIN.EXE`) [documented: `install20.json:611`]
- **Menu path (recovered from `SETMAIN.EXE` strings):**
  1. Main Menu: Navigate down to `Sound Setup` ("Setup sound and music for Shadow Warrior"), press `Enter`.
  2. Sound Setup Menu:
     - Select `FX Device`: choose `Sound Blaster 16 or AWE32` [strings: `SETMAIN.EXE`].
     - Select `Sound Blaster Address`: choose `0x220` ("Set Sound Blaster Port Address to 0x220").
     - Select `Sound Blaster Type`: choose `Sound Blaster 16 or AWE32`.
     - Select `Interrupt`: choose `IRQ 5` ("Your Sound Blaster is set to Interrupt 5").
     - Select `8-bit DMA`: choose `DMA 1`.
     - Select `16-bit DMA`: choose `DMA 5`.
     - Select `Music Device`: choose `Sound Blaster` / `AdLib` (OPL FM synthesis; target in `games20.json`) or `General Midi` at port `0x330`.
     - Press `Esc` to return to Main Menu.
  3. Exit & Save:
     - Navigate to `Save and launch Shadow Warrior` (or `Save and exit`): prompt `Save Settings before exiting?` -> select `Save current setup and exit setup program` (or press `Y`).
- **Configuration files written:** `SW.CFG` [strings: `SETMAIN.EXE`].

### Gameplay Keys
- **Enter:** `esc`, `ret`, `ret`, `ret` [documented: `games20.json:513-518`]
- **Play:** `up`, `ctrl` (walk forward, fire primary weapon) [documented: `games20.json:519-522`]
- **Quit:** `esc`, `up`, `ret`, `y` [documented: `games20.json:523-528`]

### IPX Multiplayer
- **Driver Executable:** `COMMIT.EXE` [strings: `SETMAIN.EXE`, `COMMIT.EXE`]
- **Launch path:** Via `SETUP.EXE` -> Main Menu -> `Play a network game of Shadow Warrior` -> `Network Game` menu:
  - `Player Name`
  - `Number of Players` (select 2)
  - `Setup Network Socket Number` (default or custom)
  - `Launch Shadow Warrior` (spawns `COMMIT.EXE` via `COMMIT.DAT`).
- **Command-line arguments / inputs:** `SW.EXE -net` or `COMMIT.EXE` with `COMMIT.DAT` specifying `COMMTYPE 3` (Network), `NUMPLAYERS 2`, `SOCKETNUMBER 0x869C`.
- **Packet protocol:** Discovers nodes via `_LookForNodes` broadcast, exchanges packets via ECB listen/send on IPX socket.

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:576`).
- Sound setup menus: `strings` (recovered from `SETMAIN.EXE`).
- Config file: `strings` (`SW.CFG` cited in `SETMAIN.EXE`).
- Gameplay keys: `documented` (`games20.json:513-528`).
- IPX network driver: `strings` (`COMMIT.EXE` and `COMMIT.DAT` in `SETMAIN.EXE`).

---

## 2. Wacky Wheels (`wacky`)

### Reference Tree Summary
- **Archive:** `1wacky.zip` (1,655,316 bytes, SHA-256: `3c3c35bb696194b420c5156ac9641819213f01812801f5c1683817c8bb70f4a3`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `WWSW11.SHR`
- **Extraction recipe:** `zip-sfx` over `WWSW11.SHR`
- **Extracted payload (7 files):** `MODEM.PCK`, `ORDER.FRM`, `SETUP.EXE`, `VENDOR.DOC`, `WACKY.DAT`, `WW.EXE`, `WWHELP.EXE`
- **Destination directory:** `build/dos-games-reference/wacky/`

### Installer Playbook
- **Command:** `C:\MEDIA\WACKY\INSTALL.EXE` [documented: `install20.json:660`]
- **Destination:** `C:\GAME\WACKY` [documented: `install20.json:632`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Wacky Wheels v1.1 Shareware Episode`
     - Text:
       ```text
              Wacky Wheels v1.1 
              Shareware Episode
             (c)1994 Beavis Soft
        Distributed by Apogee Software
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Wacky Wheels v1.1 Shareware  
               5.4Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\WACKY`
     - Action / Keys: Backspace default, type `C:\GAME\WACKY`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Renames `WWSW11.SHR` to `WWSW11.EXE`, runs self-extraction into `C:\GAME\WACKY`.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
        Thanks for installing the shareware episode of Wacky Wheels! 
          Before you play the game, you must run SETUP.EXE from the
              game directory.  Be default, this is C:\WACKY.
            To play the game, run WW.EXE from the game directory.
         If you run into any trouble, please consult the wwhelp.exe
           file that has been installed into your game directory.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\WACKY\SETUP.EXE` [documented: `install20.json:668`]
- **Menu path (recovered from `SETUP.EXE` strings):**
  1. Main Menu ("WACKY WHEELS VER 1.1 setup. Copyright BEAVIS-SOFT 1994"):
     - Options: `Sound setup`, `Detail levels`, `Modem/Serial setup`, `Reset score tables`, `Exit setup program`.
     - Action: Select `Sound setup`, press `Enter`.
  2. "MUSIC/FX CONFIG" Menu:
     - Select `Sound fx device`: choose `SoundBlaster`.
     - Select mixing quality: choose `Sixteen bit mixing (16 bit cards)` (or `Eight bit mixing`).
     - Select voices: choose `Four voices (Default)` (or `Six voices` / `Eight voices`).
     - Select `Music device`: choose `Adlib` (OPL music; target in `games20.json`) or `General midi`.
     - Select `Leave menu...` to return to Main Menu.
  3. Exit & Save:
     - In Main Menu, select `Exit setup program` (which writes configuration and terminates).
- **Configuration files written:** `WACKY.CFG` [strings: `SETUP.EXE`].

### Gameplay Keys
- **Enter:** `esc`, `ret`, `ret`, `ret` [documented: `games20.json:563-568`]
- **Play:** `up`, `ctrl` (accelerate, fire item) [documented: `games20.json:569-572`]
- **Quit:** `esc`, `up`, `ret`, `y` [documented: `games20.json:573-578`]

### IPX Multiplayer
- None supported (`lan: null` in `games20.json`; game supports 2-player split screen and modem/null-modem serial connection only).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:632`).
- Sound setup menus: `strings` (recovered from `SETUP.EXE`).
- Config file: `strings` (`WACKY.CFG` in `SETUP.EXE`).
- Gameplay keys: `documented` (`games20.json:563-578`).

---

## 3. Cosmo's Cosmic Adventure (`cosmo`)

### Reference Tree Summary
- **Archive:** `1cosmo.zip` (597,300 bytes, SHA-256: `45e30c651e2676b818187d466a1135701b0b26145c7ad2f40775ed2d43acec4c`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `COSSW12.SHR`
- **Extraction recipe:** `apogee-shr` over `COSSW12.SHR`
- **Extracted payload (5 files):** `COSMO1.EXE`, `COSMO1.STN`, `COSMO1.VOL`, `ORDER.FRM`, `VENDOR.DOC`
- **Destination directory:** `build/dos-games-reference/cosmo/`

### Installer Playbook
- **Command:** `C:\MEDIA\COSMO\INSTALL.EXE` [documented: `install20.json:716`]
- **Destination:** `C:\GAME\COSMO` [documented: `install20.json:688`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Cosmo's Cosmic Adventure v1.2 Shareware Episode`
     - Text:
       ```text
                 Cosmo v1.2
              Shareware Episode
        (c) 1992 Apogee Software, Ltd.
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Cosmo v1.2 Shareware 
               2.0Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\COSMO`
     - Action / Keys: Backspace default, type `C:\GAME\COSMO`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Decompresses `cossw12.shr` into destination directory via internal Apogee SHR routine.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
          Thanks for installing the shareware episode of Cosmo!
        To play the game, run COSMO1.EXE from the game directory. 
                     By default, this is C:\COSMO.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\COSMO\COSMO1.EXE` (No standalone setup utility exists; configured in-game) [documented: `install20.json:723`]
- **Audio configuration & controls (recovered from decompressed `COSMO1.EXE` strings via `reachability.py`):**
  - Cosmo automatically detects AdLib/Sound Blaster FM synthesis at I/O port 388h on launch.
  - In-game toggles:
    - Press `S` to toggle sound effects ("Sound Toggle: The sound is now ON! / The sound is now OFF!").
    - Press `M` to toggle background music ("Music Toggle: The music is now ON! / The music is now OFF!").
  - Options Menu (`HELP MENU`, via `F1` or `Esc`):
    - `S)ave your game`
    - `R)estore a game`
    - `H)elp`
    - `G)ame redefine` (Keyboard / Joystick configuration)
    - `V)iew High Scores`
    - `Q)uit Game`
    - Sound testing: option `T)est sound`, enter sound number, press `Enter` to hear sound.
- **Configuration files written:** `COSMO1.CFG` [strings: `COSMO1.EXE`].

### Gameplay Keys
- **Enter:** `ret`, `ret`, `ret` [documented: `games20.json:606-610`]
- **Play:** `up`, `ctrl` (look up, jump) [documented: `games20.json:611-614`]
- **Quit:** `esc`, `q`, `y` [documented: `games20.json:615-619`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:688`).
- Sound setup & in-game menu: `strings` (recovered from unpacked LZEXE load-module of `COSMO1.EXE`).
- Config file: `strings` (`COSMO1.CFG` in `COSMO1.EXE`).
- Gameplay keys: `documented` (`games20.json:606-619`).

---

## 4. Duke Nukem II (`duke2`)

### Reference Tree Summary
- **Archive:** `4duke.zip` (1,146,465 bytes, SHA-256: `c94479ace93a8923615ddebea96bcd18242a3a31f54cdcbc84e02ca0ab685829`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `DN2SW10.SHR`
- **Extraction recipe:** `zip-sfx` over `DN2SW10.SHR`
- **Extracted payload (10 files):** `DN2HELP.EXE`, `NUKEM2.CMP`, `NUKEM2.EXE`, `NUKEM2.F1`, `NUKEM2.F2`, `NUKEM2.F3`, `NUKEM2.F4`, `NUKEM2.F5`, `ORDER.FRM`, `VENDOR.DOC`
- **Destination directory:** `build/dos-games-reference/duke2/`

### Installer Playbook
- **Command:** `C:\MEDIA\DUKE2\INSTALL.EXE` [documented: `install20.json:772`]
- **Destination:** `C:\GAME\DUKE2` [documented: `install20.json:744`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Duke Nukem II v1.0 Shareware Episode`
     - Text:
       ```text
              Duke Nukem II v1.0 
              Shareware  Episode
        (c) 1993 Apogee Software, Ltd.
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Duke Nukem II v1.0 Shareware 
               3.6Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\DUKE2`
     - Action / Keys: Backspace default, type `C:\GAME\DUKE2`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Renames `dn2sw10.shr` to `dn2sw10.exe`, self-extracts into destination, renames back.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
        Thanks for installing the shareware episode of Duke Nukem II!
          To play the game, run NUKEM2.EXE from the game directory.
                       By default, this is C:\DUKE2.
          If you run into any trouble, please consult the dn2help.exe 
            file that has been installed into your game directory.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\DUKE2\NUKEM2.EXE` (Configured automatically via BLASTER environment variable and in-game options menu) [documented: `install20.json:779`]
- **Audio configuration & controls (recovered from decompressed `NUKEM2.EXE` strings):**
  - Duke Nukem II parses the standard `BLASTER` environment variable (`SET BLASTER=A220 I5 D1 H5 P330 T6`) on startup.
  - Digitized sound effects are played via Sound Blaster DSP, and background music via AdLib/OPL FM synthesis.
  - In-game Options Menu (via `Esc` or `F1`):
    - `Music_On` / `Music_Off`
    - `Sound_On` / `Sound_Off`
    - `Key_Config`
    - `Save_Game` / `Restore_Game`
- **Configuration files written:** `NUKEM2.MNI` [strings: `NUKEM2.EXE`].

### Gameplay Keys
- **Enter:** `ret`, `ret`, `ret` [documented: `games20.json:647-651`]
- **Play:** `up`, `ctrl` (look up, fire weapon) [documented: `games20.json:652-655`]
- **Quit:** `esc`, `q`, `y` [documented: `games20.json:656-660`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:744`).
- Sound setup & BLASTER variable detection: `strings` (recovered from unpacked `NUKEM2.EXE`).
- Config file: `strings` (`NUKEM2.MNI` in `NUKEM2.EXE`).
- Gameplay keys: `documented` (`games20.json:647-660`).

---

## 5. Raptor: Call of the Shadows (`raptor`)

### Reference Tree Summary
- **Archive:** `1rap12.zip` (2,013,010 bytes, SHA-256: `7d6b062dcdc76d9ea02d8d71af14e5043223581d792f6ead4eb1316fd0351552`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `RAPSW12.SHR`
- **Extraction recipe:** `zip-sfx` over `RAPSW12.SHR`
- **Extracted payload (7 files):** `FILE0000.GLB`, `FILE0001.GLB`, `ORDER.FRM`, `RAP.EXE`, `RAPHELP.EXE`, `SETUP.EXE`, `VENDOR.DOC`
- **Destination directory:** `build/dos-games-reference/raptor/`

### Installer Playbook
- **Command:** `C:\MEDIA\RAPTOR\INSTALL.EXE` [documented: `install20.json:828`]
- **Destination:** `C:\GAME\RAPTOR` [documented: `install20.json:800`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Raptor v1.2 Shareware Episode`
     - Text:
       ```text
                 Raptor v1.2 
              Shareware Episode
            (c)1994 Cygnus Studios
        Distributed by Apogee Software
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Raptor v1.2 Shareware  
               5.1Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\RAPTOR`
     - Action / Keys: Backspace default, type `C:\GAME\RAPTOR`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Renames `rapsw12.shr` to `rapsw12.exe`, self-extracts into `C:\GAME\RAPTOR`, renames back.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
             Thanks for installing the shareware episode of Raptor!
         Before you play the game, you must first run the setup program.
                 To do this, run SETUP.EXE from the game directory.
                           By default, this is C:\RAPTOR.
                    After setup, enter RAP.EXE to play the game!
           If you run into any trouble, please consult the raphelp.exe
             file that has been installed into your game directory.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\RAPTOR\SETUP.EXE` [documented: `install20.json:835`]
- **Menu path (recovered from decompressed `SETUP.EXE` strings):**
  1. Main Setup Menu:
     - Select `Music Card`: choose `Adlib` (OPL FM music; target in `games20.json`) or `Sound Blaster` / `General Midi`.
     - Select `Sound FX Card`: choose `Sound Blaster` (or `Sound Blaster AWE 32`).
     - Base Port (`SBPORT`): select `220`.
     - IRQ (`SBIRQ`): select `5`.
     - DMA Channel (`SBDMA`): select `1`.
     - Channels (`Channels` / `NUMDIG`): select `4` (default) or `8`.
     - Save and Exit.
  2. The setup program also reads the `BLASTER` environment variable for defaults.
- **Configuration files written:** `SETUP.INI` [strings: `SETUP.EXE`].

### Gameplay Keys
- **Enter:** `esc`, `ret`, `ret`, `ret` [documented: `games20.json:688-693`]
- **Play:** `up`, `ctrl` (fly forward, fire laser) [documented: `games20.json:694-697`]
- **Quit:** `esc`, `up`, `ret`, `y` [documented: `games20.json:698-703`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:800`).
- Sound setup menus & options: `strings` (recovered from unpacked LZEXE load-module of `SETUP.EXE`).
- Config file: `strings` (`SETUP.INI` in `SETUP.EXE`).
- Gameplay keys: `documented` (`games20.json:688-703`).

---

## 6. Hocus Pocus (`hocus`)

### Reference Tree Summary
- **Archive:** `1hp11.zip` (937,575 bytes, SHA-256: `83352f33279c05f8e6137c285e2432d0500cb07623a9d3875a17829ec7a342a4`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `HPSW11.SHR`
- **Extraction recipe:** `zip-sfx` over `HPSW11.SHR`
- **Extracted payload (10 files):** `HOCUS.DAT`, `HOCUS.EXE`, `HOCUSG.BAT`, `HPHELP.EXE`, `ORDER.FRM`, `SETUP.EXE`, `ULTRAMID.EXE`, `ULTRAMID.INI`, `VENDOR.DOC`
- **Destination directory:** `build/dos-games-reference/hocus/`

### Installer Playbook
- **Command:** `C:\MEDIA\HOCUS\INSTALL.EXE` [documented: `install20.json:884`]
- **Destination:** `C:\GAME\HOCUS` [documented: `install20.json:856`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Hocus Pocus v1.1 Shareware Episode`
     - Text:
       ```text
               Hocus Pocus v1.1 
              Shareware  Episode
        (c) 1994 Apogee Software, Ltd.
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Hocus Pocus v1.1 Shareware 
               3.3Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\HOCUS`
     - Action / Keys: Backspace default, type `C:\GAME\HOCUS`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Renames `hpsw11.shr` to `hpsw11.exe`, self-extracts into destination, renames back.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
        Thanks for installing the shareware episode of Hocus Pocus! 
         Before playing, you need to run the SETUP.EXE file that is
            in the game directory.  By default, this is C:\HOCUS.
          To play the game, run HOCUS.EXE from the game directory.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\HOCUS\SETUP.EXE` [documented: `install20.json:891`]
- **Menu path ("Hocus Pocus Setup - Version 1.1", recovered from `SETUP.EXE` and `HOCUS.EXE` strings):**
  1. Main Menu:
     - Select `Select Music Card`: choose `AdLib` (or `Sound Blaster` / `General MIDI`).
     - Select `Select Sound FX Card`: choose `Sound Blaster`.
     - Select `Voices`: choose `Three` (or four).
     - Select `Select Controller`: choose `Keyboard`.
     - Select `Save Settings & Exit`.
  2. Hardware parameters (Port 220h, IRQ 5, DMA 1) are read directly from the `BLASTER` environment variable by `HOCUS.EXE` (`Missing Sound Blaster address in BLASTER evironment variable`).
- **Configuration files written:** `HOCUS.SAV` [strings: `HOCUS.EXE`].

### Gameplay Keys
- **Enter:** `ret`, `ret`, `ret` [documented: `games20.json:731-735`]
- **Play:** `up`, `ctrl` (look up, shoot magic bolt) [documented: `games20.json:736-739`]
- **Quit:** `esc`, `q`, `y` [documented: `games20.json:740-744`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:856`).
- Sound setup menus: `strings` (recovered from `SETUP.EXE`).
- Config file: `strings` (`HOCUS.SAV` cited in `HOCUS.EXE`).
- Gameplay keys: `documented` (`games20.json:731-744`).

---

## 7. Bio Menace (`biomenace`)

### Reference Tree Summary
- **Archive:** `1bio11.zip` (717,127 bytes, SHA-256: `3c5e4e5774b13f2d233b9b532cd34b5708b8fcec14b27824b75d16563298b719`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `BMSW11.SHR`
- **Extraction recipe:** `apogee-shr` over `BMSW11.SHR`
- **Extracted payload (14 files):** `AUDIOHED.BM1`, `AUDIOT.BM1`, `BMENACE1.EXE`, `BMHELP.EXE`, `EGADICT.BM1`, `EGAGRAPH.BM1`, `EGAHEAD.BM1`, `GAMEMAPS.BM1`, `GFXINFOE.BM1`, `MAPHEAD.BM1`, `MAPTEMP.BM1`, `MAPTHEAD.BM1`, `ORDER.FRM`, `VENDOR.DOC`
- **Destination directory:** `build/dos-games-reference/biomenace/`

### Installer Playbook
- **Command:** `C:\MEDIA\BIOMEN\INSTALL.EXE` [documented: `install20.json:940`]
- **Destination:** `C:\GAME\BIOMEN` [documented: `install20.json:912`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Bio Menace v1.1 Shareware Episode`
     - Text:
       ```text
               Bio Menace v1.1
              Shareware Episode
        (c) 1993 Apogee Software, Ltd.
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Bio Menace v1.1 Shareware 
               1.0Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\BMENACE`
     - Action / Keys: Backspace default, type `C:\GAME\BIOMEN`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Decompresses `bmsw11.shr` into destination directory via internal Apogee SHR routine.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
        Thanks for installing the shareware episode of Bio Menace!
        To play the game, run BMENACE1.EXE from the game directory. 
                    By default, this is C:\BMENACE.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\BIOMEN\BMENACE1.EXE` (In-game setup; no external utility) [documented: `install20.json:947`]
- **Menu path (recovered from decompressed `BMENACE1.EXE` strings via `reachability.py`):**
  1. Main Menu: Navigate down to `CONFIGURE`, press `Enter`.
  2. Configure Menu:
     - Select `SOUND`: choose `ADLIB/SOUNDBLASTER` (or `QUIET ADLIB/SOUNDBLASTER`).
     - Select `MUSIC`: choose `ADLIB/SOUNDBLASTER`.
  3. Return to Main Menu (automatically persists changes to disk).
- **Configuration files written:** `CONFIG.BM1` [strings: `BMENACE1.EXE`].

### Gameplay Keys
- **Enter:** `ret`, `ret`, `ret` [documented: `games20.json:772-776`]
- **Play:** `up`, `ctrl` (look up, fire gun) [documented: `games20.json:777-780`]
- **Quit:** `esc`, `q`, `y` [documented: `games20.json:781-785`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:912`).
- Sound setup menus: `strings` (recovered from unpacked LZEXE load-module of `BMENACE1.EXE`).
- Config file: `strings` (`CONFIG.BM1` in `BMENACE1.EXE`).
- Gameplay keys: `documented` (`games20.json:772-785`).

---

## 8. Blake Stone: Aliens of Gold (`blakestone`)

### Reference Tree Summary
- **Archive:** `1bs30.zip` (1,495,394 bytes, SHA-256: `9a7e39037689cb84054bd3ddca290ab3644e327f951eb08e7ad79500e9c0dfb3`)
- **Outer media (3 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `BSSW30.SHR`
- **Extraction recipe:** `zip-sfx` over `BSSW30.SHR`
- **Extracted payload (18 files):** `AUDIOHED.BS1`, `AUDIOT.BS1`, `BSHELP.EXE`, `BSTONE.BAT`, `BS_AOG.EXE`, `IANIM.BS1`, `JAMERR.EXE`, `JM_ERROR.H`, `MAPHEAD.BS1`, `MAPTEMP.BS1`, `ORDER.FRM`, `SANIM.BS1`, `SETBLAST.EXE`, `VENDOR.DOC`, `VGADICT.BS1`, `VGAGRAPH.BS1`, `VGAHEAD.BS1`, `VSWAP.BS1`
- **Destination directory:** `build/dos-games-reference/blakestone/`

### Installer Playbook
- **Command:** `C:\MEDIA\BLAKE\INSTALL.EXE` [documented: `install20.json:996`]
- **Destination:** `C:\GAME\BLAKE` [documented: `install20.json:968`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Blake Stone v3.0 Shareware Episode`
     - Text:
       ```text
              Blake Stone v3.0 
              Shareware Episode
           (c) 1994 JAM Productions
        Distributed by Apogee Software
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Blake Stone v3.0 Shareware 
               3.6Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\BSTONE`
     - Action / Keys: Backspace default, type `C:\GAME\BLAKE`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Renames `bssw30.shr` to `bssw30.exe`, self-extracts, renames back.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about this game, 
          please visit our Official Web page at:
                http://www.apogee1.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
        Thanks for installing the shareware episode of Blake Stone! 
            To play the game, run BSTONE from the game directory.
                       Be default, this is C:\BSTONE.
         If you run into any trouble, please consult the bshelp.exe
           file that has been installed into your game directory.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\BLAKE\BS_AOG.EXE` (or `BSTONE.BAT`) [documented: `install20.json:1003`]
- **Menu path (recovered from decompressed `BS_AOG.EXE` strings via `reachability.py`):**
  1. Audio hardware detection:
     - Blake Stone detects Sound Blaster DSP parameters via the `BLASTER` environment variable (`SET BLASTER=A220 I5 D1 H5 P330 T6`).
     - (`SETBLAST.EXE` is an external diagnostic tool that edits `AUTOEXEC.BAT` on real hardware; not required in the DOS VM).
  2. In-Game Sound Menu (`SOUND SETTINGS`, via `Esc` / `F1`):
     - `SOUND EFFECTS`: Select `ADLIB/SOUND BLASTER`.
     - `DIGITIZED SOUNDS`: Select `SOUND BLASTER`.
     - `BACKGROUND MUSIC`: Select `ADLIB/SOUND BLASTER`.
  3. Return to Main Menu / Game.
- **Configuration files written:** `CONFIG.BS1` [strings: `BS_AOG.EXE`].

### Gameplay Keys
- **Enter:** `esc`, `ret`, `ret`, `ret` [documented: `games20.json:813-818`]
- **Play:** `up`, `ctrl` (walk forward, fire weapon) [documented: `games20.json:819-822`]
- **Quit:** `f10`, `y` (quick exit) [documented: `games20.json:823-826`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:968`).
- Sound setup menus: `strings` (recovered from unpacked LZEXE load-module of `BS_AOG.EXE`).
- Config file: `strings` (`CONFIG.BS1` in `BS_AOG.EXE`).
- Gameplay keys: `documented` (`games20.json:813-826`).

---

## 9. Terminal Velocity (`terminal`)

### Reference Tree Summary
- **Archive:** `3dtv12.zip` (3,795,467 bytes, SHA-256: `ff55a2a54be15f73899cc591310a226ded1f652c1a73ceeaeeb72e9881346ca4`)
- **Outer media (4 files):** `FILE_ID.DIZ`, `INSTALL.EXE`, `LICENSE.TXT`, `TV12S.SHR`
- **Extraction recipe:** `zip-sfx` over `TV12S.SHR`
- **Extracted payload (36 files):** `ARIA.COM`, `COMMIT.EXE`, `GAME.EXE`, `GF166.COM`, `GF1DIGI.ADV`, `GF1MIDI.ADV`, `LICENSE.TXT`, `NOSOUND.COM`, `ORDER.FRM`, `PAS16.COM`, `PAUDIO.COM`, `SB16.COM`, `SBCLONE.COM`, `SBLASTER.COM`, `SBPRO.COM`, `SETD.DES`, `SETD.EXE`, `SETD.SND`, `SETUP.EXE`, `SHARE.POD`, `STARTUP.POD`, `TRI1.WAV`..`TRI10.WAV`, `TV.EXE`, `TVDWANGO.ZIP`, `TVHELP.EXE`, `ULTRAMID.EXE`, `ULTRAMID.INI`
- **Destination directory:** `build/dos-games-reference/terminal/`

### Installer Playbook
- **Command:** `C:\MEDIA\TV\INSTALL.EXE` [documented: `install20.json:1052`]
- **Destination:** `C:\GAME\TV` [documented: `install20.json:1024`]
- **Screen sequence (recovered from embedded `install.cfg` inside `INSTALL.EXE`):**
  1. **Welcome / Title Screen** [strings: `INSTALL.EXE`]
     - Window Title: `Terminal Velocity v1.2 Shareware Episode`
     - Text:
       ```text
         Terminal Velocity v1.2
           Shareware Episode
        (c) 1994 Terminal Reality  
         Distributed by 3D Realms
       ```
     - Action / Keys: Press `Enter` (or any key).
  2. **Group Selection Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
       1   [X] Install Terminal Velocity v1.2 Shareware
               10.2Mb of Hard Drive Space Required
       ```
     - Action / Keys: Press `Enter`.
  3. **Destination Directory Prompt** [strings: `INSTALL.EXE`]
     - Text: `Please select a drive and directory to install this game to, and press [Enter].`
     - Default: `C:\TV`
     - Action / Keys: Backspace default, type `C:\GAME\TV`, press `Enter`.
  4. **Extraction Progress Screen** [strings: `INSTALL.EXE`]
     - Action: Renames `tv12s.shr` to `tv12s.exe`, self-extracts, renames back.
     - Pop-up message [strings: `INSTALL.EXE`]:
       ```text
        If you want to learn more about Terminal Velocity, 
              please vist our Official Web page at:
                     http://www.3drealms.com
       ```
     - Action / Keys: Press `Enter` (or wait for auto-advance).
  5. **Completion Screen** [strings: `INSTALL.EXE`]
     - Text:
       ```text
        Thanks for installing the shareware episode of Terminal Velocity! 
         Before you play the game, you must first run the setup program.
              To do this, run SETUP.EXE from the TV game directory.
                         By default, this is C:\TV.
          If you run into any trouble, please consult the tvhelp.exe
            file that has been installed into your game directory.
       ```
     - Action / Keys: Press any key to exit to DOS.

### Sound Setup Playbook
- **Program:** `C:\GAME\TV\SETUP.EXE` (which invokes `SETD.EXE` for digital audio) [documented: `install20.json:1059`]
- **Menu path (recovered from decompressed `SETUP.EXE` and `SETD.EXE` strings):**
  1. Main Menu ("TV Shareware Setup"):
     - Select `Sound Setup` ("Setup sound and music for TV"), press `Enter`.
     - `SETUP.EXE` spawns `SETD.EXE` (DIGPAK Driver Setup Program V3.3).
  2. Inside `SETD.EXE`:
     - Select sound card: choose `Creative Labs Sound Blaster 16` (`SB16.COM`).
     - Configure parameters: Port `220h`, IRQ `5`, DMA `1`, High DMA `5` (or accept BLASTER detection).
     - Test sound: plays `SETD.SND` twice ("Playing sound effect 2 times", "Did the sound play ok? (y/n)" -> press `y`).
     - Driver is copied/configured as `SOUNDRV.COM`. Exits back to `SETUP.EXE`.
  3. Inside `SETUP.EXE` Music Configuration:
     - Select `MusicDevice`: choose `Sound Blaster` / `AdLib` (OPL FM music; target in `games20.json`) or `General MIDI` on port `0x330`.
     - Return to Main Menu.
  4. Exit & Save:
     - Select `Save and launch TV` (or press `Esc` -> prompt `Save Settings before exiting?` -> select `Yes: Save current setup and exit setup program`).
- **Configuration files written:** `SETUP.CFG`, `SOUNDRV.COM` [strings: `SETUP.EXE`, `SETD.EXE`].

### Gameplay Keys
- **Enter:** `esc`, `ret`, `ret`, `ret` [documented: `games20.json:856-861`]
- **Play:** `up`, `ctrl` (pitch up/accelerate, fire primary weapon) [documented: `games20.json:862-865`]
- **Quit:** `esc`, `q`, `y` [documented: `games20.json:866-870`]

### IPX Multiplayer
- **Driver Executable:** `COMMIT.EXE` [strings: `SETUP.EXE`, `COMMIT.EXE`]
- **Launch path:** Via `SETUP.EXE` -> Main Menu -> `Play a network game of TV` -> `Network Game` menu:
  - `Number of Players`: select 2
  - `Network Setup` / `Change Socket Number` (default socket 0x869C)
  - `Launch TV`: writes `COMMIT.DAT` (`COMMTYPE 3`, `NUMPLAYERS 2`) and spawns `COMMIT.EXE` / `TV.EXE`.
- **Packet protocol:** IPX peer discovery via `_LookForNodes` broadcast and ECB send/listen packets.

### Confidence Marks
- Installer sequence & text: `strings` (recovered from `INSTALL.EXE` embedded script).
- Destination: `documented` (`install20.json:1024`).
- Sound setup menus: `strings` (recovered from unpacked `SETUP.EXE` and `SETD.EXE`).
- Config files: `strings` (`SETUP.CFG` and `SOUNDRV.COM`).
- Gameplay keys: `documented` (`games20.json:856-870`).
- IPX network driver: `strings` (`COMMIT.EXE` and `COMMIT.DAT` in `SETUP.EXE`).

---

## 10. Jazz Jackrabbit (`jazz`)

### Reference Tree Summary
- **Archive:** `jazz.zip` (1,421,049 bytes, SHA-256: `ed025415c0bc5ebc3a41e7a070551bdfdfb0b65b5314241152d8bd31f87c22da`)
- **Outer media:** None (Direct ZIP archive; no outer installer archive)
- **Extraction recipe:** `zip`
- **Extracted payload (78 files):** All 78 direct ZIP members, including `JAZZ.EXE`, `SETUP.EXE`, `HELPME.DOC`, `MANUAL.DOC`, `SETUP.INT`, `MDRVINFO.INT`, `SONG*.PSM`, etc.
- **Destination directory:** `build/dos-games-reference/jazz/`

### Installer Playbook
- **Command:** None (Direct ZIP archive; extracted directly to `C:\GAME\JAZZ` per G20-1 host extraction specification) [documented: `install20.json:1106-1111`]
- **Destination:** `C:\GAME\JAZZ` [documented: `install20.json:1080`]

### Sound Setup Playbook
- **Program:** `C:\GAME\JAZZ\SETUP.EXE` [documented: `install20.json:1114`]
- **Menu path (recovered from `SETUP.INT` and `MDRVINFO.INT` resource tables):**
  1. Main Setup Menu ("Jazz Jackrabbit Setup"):
     - Title banner: `Jazz Jackrabbit / Version 1.0 / (c) 1994 Arjan Brussee, Joshua Jensen, Cliff Bleszinski and Epic MegaGames`
     - Menu options: `Select Sound Card`, `Instructions`, `Exit and Save`.
     - Action: Select `Select Sound Card`, press `Enter`.
  2. BLASTER Auto-Detection Screen:
     - Prompt: `BLASTER settings are ok. Accept them?` [documented: `SETUP.INT`]
     - Action / Keys: Select `Yes` (or select `No` to customize).
  3. Manual Sound Card Selection (if selecting manually):
     - Card menu: choose `Creative Labs Sound Blaster 16` ("Select this only if you have a true Sound Blaster 16, Sound Blaster 16 ASP, AWE-32... Supports high-quality stereo music and sound effects") [documented: `MDRVINFO.INT`].
     - Select Port Address: `0220h`.
     - Select Main IRQ: `IRQ 5`.
     - Select DMA Channel Out: `DMA 1`.
     - Select DMA Channel In: `DMA 5` (or 1).
     - Select Playback Quality: choose `High Quality (486-33)` (20,100 Hz) or `Very High Quality (486-50)` (32,100 Hz) or `Ultra high quality (Pentium)` (44,100 Hz).
     - Select `Exit to Main Menu`.
  4. Exit & Save:
     - Select `Exit and Save` (persists settings and terminates).
- **Configuration files written:** `SOUNDCRD.INF` [strings: `SETUP.EXE`, `JAZZ.EXE`].

### Gameplay Keys
- **Enter:** `ret`, `ret`, `ret` [documented: `games20.json:905-909`]
- **Play:** `up`, `ctrl` (look up, fire blaster) [documented: `games20.json:910-913`]
- **Quit:** `esc`, `q`, `y` [documented: `games20.json:914-918`]

### IPX Multiplayer
- None (`lan: null`).

### Confidence Marks
- Direct ZIP extraction exception: `documented` (`install20.json:1106-1111`, `fx-games20.md:147`).
- Destination: `documented` (`install20.json:1080`).
- Sound setup menus & card definitions: `documented` (`SETUP.INT` and `MDRVINFO.INT` package resources).
- Config file: `strings` (`SOUNDCRD.INF` cited in `SETUP.EXE` and `JAZZ.EXE`).
- Gameplay keys: `documented` (`games20.json:905-918`).
