CiukiOS VM session for the DOS window  (C:\VM)
==============================================

This directory runs original DOS programs inside the desktop DOS window
with a virtual VGA, keyboard, mouse, Sound Blaster 16 (DSP 4.05, IRQ 7,
DMA 1/5, port 220h) and OPL3 whose sound plays on the AC'97 card.
DOS/4GW games (the original DOOM, doom-vanille) use the same devices through
a session-bound protected-mode host.

The desktop starts the VM manager at boot and opens every DOS program in a
window of its own (Run, a double-click, or "Full screen" from the menu for
the whole screen). Nothing needs to be typed. The files:

  VMSTART.COM   loads JEMM386.EXE (V86 monitor) and CVSESS.DLL (session
                module) with the options the window profile is tested with;
                the desktop runs it at boot.
  DPMIRUN.COM   the window's launcher: binds \SBEMU\HDPMI32I.EXE to the
                window for DOS/4GW programs, runs the program from its own
                directory and unloads the host afterwards.
  VMFORK.EXE    the desktop's fork launcher, with an explicit MZ memory
                requirement and its own stack. Creates the DOS window's VM.
  VMFORK.COM    VMFORK <program> [arguments]: the same launcher for command-
                line tools and compatibility probes using the COM format.

Sound needs an AC'97 card (QEMU: -device AC97). The games' DEFAULT.CFG must
select Sound Blaster (port 220h, IRQ 7, DMA 1) for effects and OPL/Adlib or
Sound Blaster for music. The default VMSTART options place upper memory at
CD00-EBFF, which is free RAM on QEMU/SeaBIOS.

Validated on QEMU (KVM, pentium3, AC'97) only. No physical-hardware
qualification is claimed.

Licences
--------
JEMM386.EXE, JLOAD.EXE: Jemm by Japheth, see JEMM.TXT (Artistic licence) and
JLOAD.TXT; CiukiOS changes are listed in JEMMMODS.TXT.
CVSESS.DLL contains DBOPL (DOSBox OPL emulator, GPL-2.0-or-later, from
VSBHDA; see \SBEMU\VSBHDA.GPL); CiukiOS sources are in the CiukiOS repository.
