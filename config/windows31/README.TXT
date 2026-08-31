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

Windows can be closed and launched again without rebooting CiukiOS.  XMS,
A20, and DOS file handles are restored when the Windows session ends.

CiukiOS installs a standard INT 33h PS/2 mouse service during every boot and
provides the IBM INT 15h/C2 BIOS interface selected by the Windows PS/2 mouse
driver.  MOUSE STATUS and MOUSE INFO inspect the DOS service.  CTMOUSE.EXE is
also provided under C:\SYSTEM\DRIVERS as an optional GPL replacement for
compatibility testing; it is not loaded on top of the resident service
automatically.

These files are optional local media and are not part of the CiukiOS license.
