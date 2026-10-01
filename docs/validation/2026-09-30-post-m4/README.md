# Post-M4 additions, QEMU validation (30 September 2026)

The image was rebuilt with `CIUKIOS_VM_WINDOW=1 bash scripts/build_full.sh`.
The final M4, guest I/O and PE status gates ran on
`build/full/ciukios-full.img` after the PE detector and PS/2 isolation changes.
Registry, desktop apps and desktop polish gates ran on an earlier integration
build; their modules did not change in the final rebuild. These are emulator
results only.
The earlier complete 26/26 M4 profile is documented in
`docs/validation/2026-09-30-m4/README.md`.

| Gate | Result | Evidence |
| --- | --- | --- |
| M4 with `\DESKTOP\TestGames\DOOM.COM` | Pass: DOOM with Files, two text VMs, focus, bounded close and independent close after the PS/2 isolation change. | [`m4-testgames.json`](m4-testgames.json), `build/tests/post-m4-three-games-m4/` |
| Settings Registry | Pass: save, disk checksum, reboot, read, delete. | [`registry.json`](registry.json), `build/tests/registry-final-20260930/` |
| Free Win32 fixture handling | Pass: two PE32 files recognized and closed without forking. **This is not Win32 execution.** | [`win32-status.json`](win32-status.json), `build/tests/win32-status-three-games/` |
| Desktop apps regression | Pass: Files operations, CiukNote, Task Manager and keyboard shortcuts. | [`desktop-apps.json`](desktop-apps.json), `build/tests/desktop-apps-post-m4-20260930/` |
| Desktop polish regression | Pass: context menus, Recycle Bin, Control Panel, fonts, device installation and reboot state. | [`desktop-polish.json`](desktop-polish.json), `build/tests/desktop-polish-post-m4-20260930/` |
| M4 guest input and audio | Pass: virtual mouse movement/clicks, SB16/OPL output and clean guest exit after the PS/2 isolation change. | [`guest-io.json`](guest-io.json), `build/tests/guestio-ps2-isolation/` |

`src/probes/win32/probe.c` supplies both free probes. The 32-bit PE binaries
were built with Windows 4.0 subsystem headers. Host Wine ran `HELLO.EXE` and
`SETUP.EXE` successfully to establish that the fixtures themselves work; the
CiukiOS QEMU gate deliberately reports them as unsupported. No Windows 95/98
installer or game is claimed to run yet.

The `TestGames` Files double-click path is also shown by
`docs/screenshots/0.8.0/doom-from-testgames.png` (source:
`build/tests/testgames-final-folder/`). The image now has three launchers in
`C:\DESKTOP\TestGames`; game data stays in the existing app tree.
The old `DOOMWIN` and `WOLFWIN` prototype launchers were removed from this
folder after their legacy adapters failed in M4. Wolf4GW draws its
[sign-on screen](wolf-sign-on.png) but currently loses desktop mouse input and
cannot be closed normally in QEMU (`build/tests/wolf-close-ps2-isolation/`).
Disabling its guest mouse did not resolve the issue. Containing unknown guest
8042 commands in the virtual controller was retained as an isolation fix;
the M4 and guest I/O gates above passed afterward. Wolf remains open.

`fsck.fat -n build/full/ciukios-full.img` reported a clean FAT image (871
files, 14,877 of 32,722 clusters). The rebuilt kernel is 42,511 bytes under
its 43,264-byte cap; `SHELL.COM` is 57,712 bytes under its 60,928-byte cap.
`FILES.APP` uses 65,472 bytes of its 65,536-byte DGROUP allowance, leaving
little room for further near-data growth.
