# File operations and launching (G8)

## Reference behaviour and implementation decision

The Windows 98 Resource Kit, `wrkc25_3.htm` (Associating File Types with
Applications; Customizing the New Shortcut Menu), describes extension →
ProgID associations in HKCR, editable verb commands and registry ShellNew
templates. `wrkappg_1.htm` specifies Ctrl+Z Undo and Ctrl+Shift drag to create
a shortcut. `wrkc19_10.htm` describes extending Send To by adding an object
to the user's SendTo folder. `wrkc10_5.htm` describes restoring deleted
objects from the Recycle Bin. These pages were consulted in the owner's
read-only Resource Kit extraction; their contents are not redistributed.

Primary online references consulted before implementation:

- [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
  specifies Create Shortcut, folder/document targets and link activation.
- [MS-SHLLINK structures](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/747629b3-b5be-452a-8101-b9a2ec49978c)
  defines the header, optional ID list, LinkInfo and counted StringData.
- [Wine upstream PIF structures and reader](https://github.com/wine-mirror/wine/blob/master/programs/winevdm/winevdm.c)
  documents the standard PIF fields and the WINDOWS 386 argument override.
  Only its file-layout definitions inform this parser; DOS sessions continue
  to use CiukiOS V86, not Windows NT NTVDM. No Wine code is copied.
- [File Types](https://learn.microsoft.com/en-us/windows/win32/shell/fa-file-types)
  defines extension keys, ProgIDs, descriptions and their common registry.
- [File Types and File Associations](https://learn.microsoft.com/en-us/windows/win32/shell/fa-intro)
  and [Context Menu Handlers](https://learn.microsoft.com/en-us/windows/win32/shell/context-menu-handlers)
  describe verbs and the default action.
- [FILETYPEATTRIBUTEFLAGS](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/ne-shlwapi-filetypeattributeflags)
  defines EditFlags and FTA_OpenIsSafe (0x10000); the download confirmation
  checkbox preserves every other flag and edits this bit in shared HKCR.
- [Managing the File System](https://learn.microsoft.com/en-us/windows/win32/shell/manage)
  and [SHFILEOPSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shfileopstructa)
  describe copy/move/rename/delete and FOF_ALLOWUNDO.
- [SHBrowseForFolder](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shbrowseforfoldera)
  describes choosing a destination folder and cancellation without an operation.

CiukiOS already writes CIUKLNK3 through IShellLink and already has a per-volume
Recycle Bin. G8 reuses these formats and the existing transfer worker. No
kernel syscall, SDK record or desktop message number is needed. Native file
associations use the existing w32srv HKCR RPC, not a second native registry.
The catalogue remains a fallback for built-in accessories when no extension
registration exists. A registered but invalid command is an error, not a
reason to silently choose another application.

Expected behaviour:

- One shared, persistent last-operation Undo covers rename, completed copy,
  move and recycled delete, including multiple selected items and operations
  started from either Files or the desktop. Permanent deletion is not undoable.
  A conflicting destination or changed copied object is refused. Replacement
  and merged-directory transfers must never make Undo remove pre-existing data.
- Copy To and Move To choose a directory, then use the ordinary progress,
  replacement, cancellation and error handling. Send To copies to folders,
  creates a desktop shortcut, or opens a selected user SendTo application with
  the source as an argument. Unavailable mail/Briefcase providers are reported.
- Create Shortcut, Paste Shortcut, New Shortcut and link drops write the same
  bounded CIUKLNK3 records that the existing Win32 IShellLink writer uses.
  New Bitmap writes a valid white 24-bit BMP. Native links can target programs,
  documents and folders. Bounded MS-SHLLINK parsing accepts local LinkInfo and
  relative-path StringData; unsupported network/PIDL-only targets are errors.
- Folder Options / File Types edits the shared HKCR description, icon, verbs,
  default verb and download-confirmation attribute. Open With can be used once
  or persist the extension association with Always use this program. ShellNew
  registered templates appear alongside Folder, Text, Bitmap and Shortcut.
- One resolver handles native ELF, PE, DOS EXE/COM/BAT, PIF targets, links,
  folders and associated documents. Existing DOS VM and native launcher
  authority stay with the desktop. G9 can call the same shell entry point.
  Link recursion, command expansion, file sizes and arguments are bounded;
  malformed data produces a normal error, never a panic.

Host evidence and exact implementation limits are recorded in
`build/codex/g8-report.md`. Rendering events are listed in the fx-15 table.

## Integration boundaries

The G6/G8 merge retains the separate View / Folder Options preferences and
File Types editor, and selects the Properties worker callback without
overwriting the Undo worker callback. Both operate on the existing transfer
worker. The merge rechecked the Microsoft
[file-management reference](https://learn.microsoft.com/en-us/windows/win32/shell/manage)
and [file-type registration reference](https://learn.microsoft.com/en-us/windows/win32/shell/fa-file-types):
G8 keeps its existing transfer inverses and shared HKCR associations alongside
G6 metadata and display preferences; no additional syscall or message is needed.

The shared resolver is `fm_launch_resolve(fs, path, verb, record)` in
`apps/files/model.c` (`operations.inc`). The desktop's `open_path` callback
queues it on the existing shortcut worker and also accepts HTTP(S) URLs.
G9 can use this entry point; Run UI/history/Browse remain G9 work. Files
uses the existing DESK_LAUNCH envelope with the internal name `shell-open`.
An unassociated document opens the same Files Open With dialog; a shortcut
uses its resolved document target. Only a desktop-authenticated Files client
may send this request, and shutdown
rejects it. No opcode or syscall is added. Per-launch records with arguments
use a private, exclusive CIUKLNK3 hand-off, consumed by the same worker.

Copy/Move/Send To from multiple desktop icons use exclusive CIUKSEL1 selection
files. They do not overwrite either clipboard or drag data, and Files removes
the manifest after reading it. The selected names are applied only after its
initial directory scan publishes. A failed process launch can leave a small
handoff file under `/system/settings`; automatic orphan cleanup is not supplied.

Fresh Win32 hives register native text, image and audio types through the same
HKCR entries Windows applications read. Existing hives keep their application
changes. The old exact `txtfile` default `ciuknote.exe "%1"` resolves to the
native CiukNote launcher; arbitrary relative or malformed commands are errors.
The download checkbox stores the shared flag; browser download enforcement,
extracted .ico icons and shortcut properties remain their subsystem's work.

The user SendTo folder is `/Documents/SendTo`. Native folder, application and
shortcut destinations work, as do Desktop (shortcut), My Documents and A:.
Mail Recipient and Briefcase report their absent provider; this repository has
no mail attachment or Briefcase synchronization service. G18 still owns the
Win32 SendTo/Recent CSIDL wiring. These two built-in destinations remain gaps.

Undo is one shared last-operation journal. Replacements, directory merges,
permanent deletion, and operations with skipped/kept or partial trees invalidate
it rather than fabricate an inverse. Undo Copy checks descendants and contents
on the transfer worker before removing anything; changed copies are refused.
Rename, move and restore check identity, allowing edits to travel with the item.
The shared last-operation journal does not add multi-process operation locking;
concurrent transfers/Undo still need qualification and serialization.
The existing native launcher retains its six-argument and working-directory
restrictions. DOS autorun retains its 96-character printable command bound;
DOS cwd differing from the executable's folder is rejected. PIF memory/video
policy and non-ASCII ANSI-link code pages are not implemented by G8.

## G7/G8 results-view integration

The integration rechecked Microsoft's [Windows 95/98 Find instructions
(Q133717)](https://jeffpar.github.io/kbarchive/kb/133/Q133717/),
[Managing the File System](https://learn.microsoft.com/en-us/windows/win32/shell/manage)
and [File Types](https://learn.microsoft.com/en-us/windows/win32/shell/fa-file-types).
Find results identify objects in their original directories; association and
file-operation services remain the shared G8 services. Tools > Find, F3 and
In Folder coexist with Undo, Copy To / Move To, Send To and Open With.
Rename Undo records the result's actual parent. Create Shortcut beside a
selected result uses that result's parent, including selections spanning
folders. New and background Paste Shortcut are disabled in results because
there is no destination directory. Registered type descriptions are populated
before sorting search results. The combined `files_search_test.c` exercises
these boundaries, including duplicate basenames, without changing public ABI.
G7 retains its shell action number and G8's Files action follows it.
