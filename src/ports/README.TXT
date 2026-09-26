CiukiOS windowed source ports
============================

Programs > Games contains Windowed Doom and Windowed Wolf3D.
The original full-screen games remain available with their existing audio.

These are separate GPL source ports, using a cooperative CiukiOS graphics
interface. They are not a general DOS graphics virtual machine. Use Run >
Window with DWIN or WWIN, or select the games in the application library.
The engines draw 320x200 indexed pixels. The desktop presents an exact 2x
image in a movable/minimizable window, using software rendering.

LIMITS: Windowed ports currently have no audio, mouse capture or joystick.
Keyboard controls and cooperative title-bar close are supported. A native
LFB desktop at 800x600 or higher and 15/16/24/32-bit color is required.
No hardware acceleration is claimed. QEMU frame rates do not certify a
physical Pentium III, T23 or Armada E500.

Doom uses the separately installed APPS\DOOM\DOOM.WAD. Wolf uses the
separately installed APPS\WOLF3D data and its own WINCFG directory, so its
settings and saved games do not replace the full-screen version's files.
Game data is not covered by the source-code licenses below.

Doomgeneric: https://github.com/ozkl/doomgeneric
commit dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
Wolf4SDL: https://github.com/fabiangreffrath/wolf4sdl
commit a51c229ed89cab1904d68abb8938def3cf456725
Both source ports are distributed under GPL-2.0-or-later, preserving their
upstream notices. See DOOMGPL.TXT and WOLFGPL.TXT.

GAMESRC.TGZ contains pinned upstream sources, CiukiOS backend modifications,
the public interface, launchers and build scripts. Extract on the build host,
install Open Watcom v2 (default /opt/watcom), Python3 and patch, then run:
  python3 scripts/build_doom_window.py --output build/doom-window
  python3 scripts/build_wolf_window.py --output build/wolf-window
The WATCOM environment variable selects another compiler installation.
