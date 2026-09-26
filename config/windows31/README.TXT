CIUKIOS WINDOWS 3.1 LOCAL MEDIA

The original floppy images are available in C:\MEDIA\WIN31.
Their merged, directly accessible setup files are in C:\WIN31SET.

To install from CiukiOS:

  CD \
  CD WIN31SET
  SETUP.EXE

Select VGA and Windows 386 Enhanced mode.  If a locally prepared installed tree
was available when the full image was built, Windows is also in C:\WINDOWS:

  CD \
  CD WINDOWS
  WIN

WIN /3 explicitly selects Enhanced mode.  WIN /S is only a diagnostic
fallback for Standard mode.

With the normal graphical launcher, click once inside the QEMU display to
capture the relative PS/2 mouse.  Ctrl+Alt+G releases it back to the host.
The packaged Windows profile disables mouse acceleration and calibrates its
sensitivity for linear 1:1 movement without changing the DOS INT 33h service.

The full build also installs the native Microsoft Sound Blaster 1.5 and AdLib
drivers from these source disks.  They are configured for the canonical QEMU
hardware: Sound Blaster A220 I7 D1 and AdLib/OPL2 port 388.

The generated default DOS profiles request all conventional, EMS, and XMS
memory available to each Windows DOS VM.  There is no per-application memory
setting: programs copied into C:\WINDOWS later inherit the same policy.

Windows can be closed and launched again without rebooting CiukiOS.  XMS,
A20, DOS file handles, and application EXEC frames are restored when a child
or the Windows session ends.

CiukiOS installs a standard INT 33h PS/2 mouse service during every boot and
provides the IBM INT 15h/C2 BIOS interface selected by the Windows PS/2 mouse
driver.  MOUSE STATUS and MOUSE INFO inspect the DOS service.  CTMOUSE.EXE is
also provided under C:\SYSTEM\DRIVERS as an optional GPL replacement for
compatibility testing; it is not loaded on top of the resident service
automatically.

These files are optional local media and are not part of the CiukiOS license.
