# DOOM core startup evidence

The existing `build/full/t23-vbe-fix/doom-global-canonical/` run is a bounded
QEMU startup pass for the original game core. Its report records a 512 MiB
Pentium III VM, standard VGA, `\APPS\DOOM\DOOMCORE.EXE`, and
`source_unchanged: true`. The serial log records `[DOSVM] fork`, a ready DOS
window session, `HDPMI32 now resident`, and `ST_Init: Init status bar.`. The
captured frame shows DOOM's title screen inside the DOS window.

That earlier pass proves the core can initialize and render under the session
host. The newer bounded Cirrus run,
`build/full/t23-vbe-fix/doom-wrapper-cirrus-return/report.json`, extends the
evidence to the actual desktop `C:\DESKTOP\TestGames\DOOM.COM` wrapper path.
It launched twice with `-warp 1 1 -nomusic`, changed gameplay frames after
input (109,660 and 139,609 changed pixels), recognized all six menu rows from
WAD sprites, selected the quit path, observed DOSVM end and wrapper return,
closed the DOS window with Alt+F4, and verified app slot 8 unloaded before
relaunch. Thus the wrapper, input, gameplay rendering, clean return, and second
launch are covered in QEMU Cirrus; the old core-only report remains useful as
separate startup evidence.

The same complete wrapper-return sequence also passed under standard VGA in
`build/full/t23-vbe-fix/doom-wrapper-std-return/report.json`: two launches of
the TestGames wrapper, changed gameplay frames (152,995 and 152,067 pixels),
all six menu rows recognized, and clean return plus DOS-window closure after
each game. The run used 512 MiB and 1024x768; the report says
`physical_tested: false` and `source_unchanged: true`.

The test runs a private copy of the HDD image and reports `physical_tested:
false`. It does not qualify the physical T23 or Windows runtime. The related
`doom-wrapper-cirrus-linked` display run passed Preview and Keep at 640x480 but
failed later during test setup with `KeyError('TESTGAMES')`; the earlier
`display-cirrus-first` run passed its full display check at 640x480.
