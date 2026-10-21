# Add/Remove Programs (f2-87)

The existing Control Panel applet owns one sorted list and confirmation. It
combines the copied HKLM/HKCU registry adapter, bundled game provenance already
installed in `/system/licenses/*-UPSTREAM.json`, and user program directories.
System components remain visible and protected. No installer engine is added.

Research (2026-10-20):

- Microsoft, [Uninstall registry key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key):
  DisplayName, DisplayVersion, Publisher, InstallLocation, EstimatedSize and
  UninstallString are installer-owned metadata. Re-read after the process exits;
  never delete a registry entry to pretend an uninstaller succeeded.
- Microsoft, [ARPNOREMOVE](https://learn.microsoft.com/en-us/windows/win32/msi/arpnoremove)
  and [ARPNOMODIFY](https://learn.microsoft.com/en-us/windows/win32/msi/arpnomodify):
  NoRemove prohibits removal, including when UninstallString is absent.
  NoModify alone does not prohibit removal. Protected entries remain visible
  as required by the directive, rather than using older Windows' hiding behavior.
- Windows 98 Resource Kit, chapter 25, `wrkc25_2.htm`, Installing Applications
  Locally / Using Add/Remove Programs with Win32-based Applications, read in
  place from the lead checkout's `build/rskt/`: Add/Remove finds Setup/Install
  on selected drives; DOS applications normally run their own Setup.exe.
  `wrkc31_10.htm` describes stale entries after manually deleting a program.
  The licensed book is not copied or redistributed.

The bundled image is a logical FAT volume. Removal must use the existing kernel
FAT driver with exclusive access to the backing file, retaining the C: write
gate and refusing while a VM holds its read/deny-write handle. All errors,
including failed durability barriers and partial cleanup, must be reported.
Licences remain in /system/licenses. Shortcut cleanup matches path components,
not display-name substrings, and is covered by the same confirmation.
