# Run dialog (G9)

## References and decisions

The owner's Windows 98 Resource Kit, `wrkappg_4.htm` (read-only extraction),
specifies Win+R for Run. `wrkc03_4.htm` illustrates a path plus arguments in
Start > Run. Online primary sources checked before implementation:

- [Microsoft shell launching](https://learn.microsoft.com/en-us/windows/win32/shell/launch)
  describes the default association for documents and opening folders.
- [ShellExecute](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutea)
  separates the object and its arguments and reports missing objects.
- [Environment expansion](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-expandenvironmentstringsa)
  specifies case-insensitive `%name%` substitution, retaining unknown names.
- [ReactOS Run dialog implementation](https://github.com/reactos/reactos/blob/master/dll/win32/shell32/dialogs/dialogs.cpp)
  provides an independent shell implementation reference for Browse and
  HKCU `Software\Microsoft\Windows\CurrentVersion\Explorer\RunMRU`:
  `a` through `z`, `MRUList`, a `\1` suffix, case-insensitive duplicate promotion
  and reuse of the oldest slot at 26 commands. This is implementation evidence,
  not proof of every Windows 98 detail. No upstream code is copied.

Run keeps Start > Run and Win+R on their existing single window. The editable
combo offers a persistent newest-first list of 26 distinct successful commands.
Unknown environment references remain literal. Quoting preserves spaces and
arguments; malformed quotes and overlong input fail rather than truncate.
Native catalog programs keep their existing launch authority. Other objects
use G8's resolver and launch bridge, including drive paths, documents, folders,
HTTP(S), DOS COM/BAT/PIF and Win32 EXE. Bare external names search the working
directory, PATH, and the Windows directories, with EXE/COM/BAT suffix search.
Errors retain the command in Run for correction and show the ordinary error
window. Browse selects a file and inserts a quoted path without executing it;
Cancel preserves the field. Directory enumeration, resolution and registry
I/O run on the existing sleeping links worker, outside input and painting.
There are no new syscalls, devctls, desktop messages or public SDK records.

Implementation bounds follow the existing launch bridge: 1023 command bytes,
259 target-path bytes and six arguments for native catalog programs. Excess
input is refused. Browse uses the common toolkit's 512-entry directory bound.
Registry values share w32srv's ordinary delayed-flush/shutdown persistence;
G9 introduces no separate registry file. Native asynchronous requests retain
their complete command until completion, so failed spawns are not added to MRU.

Accepted completions queue up to 26 distinct pending commands in acceptance
order, so multiple native launches completing in one input turn retain their
MRU order. Shutdown drains these requests before its existing w32srv flush
barrier and before closing file descriptors.
