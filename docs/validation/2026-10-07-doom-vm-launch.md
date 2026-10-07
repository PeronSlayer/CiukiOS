# DOOM launcher in DOS sessions

## Evidence and decision

The DOOM.COM/DOOM.EXE wrapper is used in two distinct contexts. In plain DOS it
owns a short-lived HDPMI/VSBHDA setup and probes the native Sound Blaster before
launching the game. A desktop DOS window instead starts `DPMIRUN.COM /V` in a
forked CiukiOS VM. DPMIRUN begins that VM's CVSESSION devices and installs its
session-bound HDPMI host before starting COMMAND.COM; programs launched from
that shell inherit the session's virtual SB16/OPL devices and protected-mode
host. This is the CiukiOS Jemm V86 session design, not Windows NT NTVDM.

The DPMI 1.0 specification defines INT 2Fh/1687h as the real-mode test for a
DPMI host and the entry point for a client to switch into protected mode. It
does not identify a CiukiOS DOS window, so host presence alone is insufficient
to select the session path. CVSESSION's existing INT 2Fh/1684h device API
provides that discriminator: `VM_OP_QUERY` with `CX=0` returns the active
session in `DX`. The wrapper now requires both an active CVSESSION and a
32-bit DPMI host before using the session path. These contracts are defined in
the [DPMI 1.0 specification](https://openwatcom.org/ftp/devel/docs/dpmi10.pdf),
[`src/vm/session_abi.inc`](../../src/vm/session_abi.inc), and the query handler
in [`src/vm/session_jlm.asm`](../../src/vm/session_jlm.asm).

The [DPMIRUN source](../../src/com/dpmirun.asm) documents and implements the
per-window host lifecycle: `DPMIRUN /V` binds the session scheduler, begins
virtual devices, installs HDPMI, then starts COMMAND.COM. The upstream
[HDPMI project](https://github.com/Baron-von-Riedesel/HX) supplies that host.
Upstream [VSBHDA documentation](https://github.com/Baron-von-Riedesel/VSBHDA/blob/main/vsbhda.txt)
says protected-mode games should use HDPMI and warns that running VSBHDA under
a different DPMI host is possible but not recommended.

The session path therefore runs `DOOMCORE.EXE` directly with the caller's
arguments and the DOOM directory as the working directory. It skips the
wrapper's second HDPMI install/uninstall, direct Sound Blaster DSP I/O probe,
and CR4.PVI modification. The existing transient HDPMI/VSBHDA route remains
for launches without an active CVSESSION. This follows the distinction in the
DPMI specification between a DOS virtual machine and the DPMI client running
inside it; neither the presence of a VM nor the presence of a DPMI host alone
is treated as an error.

The desktop `C:\DESKTOP\TestGames\DOOM.COM` launcher must invoke the installed
`C:\APPS\DOOM\DOOM.COM` wrapper. Launching `DOOMCORE.EXE` directly bypasses
the wrapper's session-aware audio/host selection, so the GAME_KIND=0 launcher
and full-image entry now route through the wrapper while preserving the
user's command tail. The runtime gate observes the actual DOSVM CAPP segment
through SHELL's `app_segs` slot 8 and window 11 flags. `[DOSVM] ended` marks the
guest finished but leaves the window open; the test closes it with Alt+F4 and
checks that SHELL unloads the app and closes the window before reopening it.

## Validation

The bounded full-HDD QEMU run in
`build/full/t23-vbe-fix/doom-wrapper-cirrus-return/report.json` passed on a
512 MiB Pentium III profile with Cirrus VGA. It entered the exact desktop
command `C:\DESKTOP\TestGames\DOOM.COM -warp 1 1 -nomusic` twice, confirming
the TestGames launcher reaches the installed APPS wrapper. Both runs reached
gameplay and changed the captured frame after input (109,660 pixels on the
first launch and 139,609 on the second). The harness recognized all six DOOM
menu rows from the WAD sprites, selected the quit path, observed the wrapper
return and DOSVM end, then closed the still-open DOS window with Alt+F4 and
confirmed app slot 8 unloaded before the second launch. The report also records
that the source image remained unchanged.

The same bounded wrapper-return gate also passed with standard VGA in
`build/full/t23-vbe-fix/doom-wrapper-std-return/report.json`. It launched the
same TestGames command twice at 1024x768, observed gameplay frame changes of
152,995 and 152,067 pixels, checked all six menu rows, and observed clean game
exit followed by Alt+F4 DOS-window closure for each run. The report records
`physical_tested: false` and `source_unchanged: true`.

This is QEMU evidence for the wrapper/session path. It does not qualify the
physical T23, and it does not claim a Windows runtime test. The separate
`doom-wrapper-cirrus-linked` display run applied Preview and Keep 640x480, but
the overall run failed later during setup with `KeyError('TESTGAMES')`; the
earlier `display-cirrus-first` report passed its full display check at 640x480.
These display results do not alter the DOOM wrapper pass above. The run reports
and source contracts are tied to the full-HDD profile and the primary DPMI
specification and upstream HDPMI/VSBHDA references linked above.
