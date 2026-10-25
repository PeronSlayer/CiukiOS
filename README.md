![CiukiOS boot splash](misc/CiukiOS_SplashScreen.png)

# CiukiOS

**Classic computing, one woof at a time.**

An open-source operating system for playing the games of the 1990s on the
computers of the 1990s. Dedicated to **Ciuki**, the dog in the boot splash.
Released under the [GNU GPLv2](LICENSE).

<table>
<tr>
<td><a href="docs/screenshots/1.0-alpha/01-desktop.png"><img src="docs/screenshots/1.0-alpha/01-desktop.png" width="320" alt="Desktop"></a><br><sub>Desktop</sub></td>
<td><a href="docs/screenshots/1.0-alpha/02-start-menu-games.png"><img src="docs/screenshots/1.0-alpha/02-start-menu-games.png" width="320" alt="Start menu, Games"></a><br><sub>Start menu, Games</sub></td>
<td><a href="docs/screenshots/1.0-alpha/03-doom.png"><img src="docs/screenshots/1.0-alpha/03-doom.png" width="320" alt="DOOM in a window"></a><br><sub>DOOM in a window</sub></td>
</tr>
<tr>
<td><a href="docs/screenshots/1.0-alpha/04-wolfenstein-3d.png"><img src="docs/screenshots/1.0-alpha/04-wolfenstein-3d.png" width="320" alt="Wolfenstein 3-D"></a><br><sub>Wolfenstein 3-D</sub></td>
<td><a href="docs/screenshots/1.0-alpha/05-quake.png"><img src="docs/screenshots/1.0-alpha/05-quake.png" width="320" alt="Quake"></a><br><sub>Quake</sub></td>
<td><a href="docs/screenshots/1.0-alpha/06-files.png"><img src="docs/screenshots/1.0-alpha/06-files.png" width="320" alt="Files"></a><br><sub>Files</sub></td>
</tr>
<tr>
<td><a href="docs/screenshots/1.0-alpha/07-control-panel.png"><img src="docs/screenshots/1.0-alpha/07-control-panel.png" width="320" alt="Control Panel"></a><br><sub>Control Panel</sub></td>
<td><a href="docs/screenshots/1.0-alpha/08-netsurf.png"><img src="docs/screenshots/1.0-alpha/08-netsurf.png" width="320" alt="NetSurf"></a><br><sub>NetSurf browser</sub></td>
<td><a href="docs/screenshots/1.0-alpha/09-dos-box.png"><img src="docs/screenshots/1.0-alpha/09-dos-box.png" width="320" alt="DOS Box"></a><br><sub>DOS Box</sub></td>
</tr>
</table>

QEMU captures of the 1.0 alpha ([details](docs/screenshots/1.0-alpha/README.md)).

## Highlights

**Games and DOS**
- DOS games in a window or full screen, with Sound Blaster 16 and AdLib/OPL3
  sound, DPMI and XMS/EMS, on a unified DOS kernel (a FreeDOS fork).
- DOOM, Wolfenstein 3-D and Quake shareware are pre-installed (Programs > Games).
- Virtual NE2000 and IPX for DOS LAN play (host-tested).

**Desktop**
- A Windows 98-class desktop: Start menu, taskbar, tray, Control Panel, Files,
  Task Manager, Terminal, Registry Editor, CiukNote, WordPad, CiukPaint, Music
  Player, Image Viewer, Calculator, card games, CiukIDE with a C compiler.
- Tango icons, FreeType fonts, themes and wallpapers.

**Windows programs**
- A Win32 API subset (PE loader, kernel32, user32, gdi32, DirectDraw,
  DirectSound) under development; no original Windows game has run yet.

**Internet**
- TCP/IP with DHCP and DNS (lwIP), HTTPS (BearSSL) and the NetSurf browser.

**Hardware**
- One image for late-1990s and early-2000s PCs: ATA with UDMA, FAT32, PS/2 and
  USB input, USB sticks, CD-ROM, AC'97 and other audio chips, several network
  cards. Target machines: ThinkPad T23, Compaq Armada E500 and assembled
  desktops of 1998-2004. Hardware qualification is not finished.

More detail, the plan and the evidence behind each item:
[Overview](docs/overview.md) and [release notes](docs/release-notes-1.0-alpha.md).

## Status

**1.0.0 alpha** on `main`, not released. The latest image (2026-10-25) is
verified on host tests and QEMU; its trial on the ThinkPad T23 is pending
([dev diary](dev_diary/2026-10-25-01-immagine-t23-correzioni-notturne.md),
[Changelog](CHANGELOG.md)). The frozen 0.8 line lives on the branch
[`legacy-0.8`](https://github.com/PeronSlayer/CiukiOS/tree/legacy-0.8).

## Try it

```bash
make build-full        # kernel build/f0/VMM.ELF + image build/f0/ciukios.img
make test-host         # host unit tests
make qemu-run-full     # boot the image in QEMU
```

You need clang, ld.lld, nasm, mkfs.fat, mtools and qemu-system-i386. Full
instructions, including writing the image to a real machine:
[Build and run](docs/build-and-run.md).

## Links

- [Build and run](docs/build-and-run.md)
- [Desktop guide](docs/desktop.md)
- [Developer guide](docs/developer-guide.md) and [SDK](docs/sdk.md)
- [Credits and licences](docs/credits.md)
- [Changelog](CHANGELOG.md) and [Roadmap](Roadmap.md)
- [Overview and plan](docs/overview.md)
- [Design contracts](docs/README.md)
- [Development diary](dev_diary/README.md) (Italian)
- [Support the project](DONATIONS.md)

Developed by its owner with AI agents; see [AGENTS.md](AGENTS.md).
