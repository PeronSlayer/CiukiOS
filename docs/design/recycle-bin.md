# Recycle Bin (G2)

Written 2026-10-23 21:30 CEST for directive G2
(`docs/directives/g1-g6-os-gaps-batch-1.md`). Rows closed in
`os-feature-inventory.md`: FM-13, FM-23, DT-1, DD-6, MM-2 and DT-5 (Shift+Del).
The Win32 `SHFileOperation` side is described under "Win32".

## Research (before the code)

- Windows 98 Resource Kit, ch. 10 "Disk Management", *Managing the Recycle Bin
  to Free Disk Space* (`wrkc10_5.htm`): "When you delete a file or directory,
  it is moved to the Recycle Bin, but it still takes up space on the hard
  disk"; SHIFT while deleting, or the property *Do not move files to the
  Recycle Bin*, avoids the bin; the bin's maximum size is a percentage chosen
  with a slider, "separately for each hard disk drive" with *Configure Drives
  Independently*.
- Resource Kit appendix G, *Shortcuts for Objects, Folders, and Windows
  Explorer* (`wrkappg_1.htm`): "SHIFT+DELETE: Delete file immediately without
  putting it in Recycle Bin."
- Microsoft, [SHFILEOPSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shfileopstructa):
  `FOF_ALLOWUNDO` "preserve undo information, if possible"; with
  `FO_DELETE` it sends the files to the Recycle Bin, but only when `pFrom` has
  full paths ("If `pFrom` is set to a file name without a full path, deleting
  the file with `FO_DELETE` does not move it to the Recycle Bin"). `FOF_WANTNUKEWARNING`
  asks for a warning when a file is destroyed instead of recycled.
- Removable media: files deleted from a floppy, a USB stick or a network drive
  are not recycled (period sources: the 1998 course note at
  [cs.montana.edu](https://www.cs.montana.edu/courses/spring2007/150/cs/150windows/recycle.htm),
  [Computer Hope](https://www.computerhope.com/issues/ch001009.htm),
  [O'Reilly, Windows XP Home](https://www.oreilly.com/library/view/windows-xp-home/0596002602/ch04s04.html)).
  The same sources say a bin exists per drive.
- INFO2 (Windows 95/98/ME): `C:\RECYCLED\INFO2` plus the items renamed
  `D<drive letter><record number>.<extension>` ("DC1.TXT"); a 20-byte header
  and 280-byte records: the ANSI original path (260 bytes), the record number,
  the drive number (A: = 0), the deletion time as a FILETIME and the size
  (4 bytes). Microsoft never published the format; the layout comes from the
  forensic literature and tools that parse it (the rifiuti/rifiuti2 manuals,
  the "Forensic Analysis of Microsoft Windows Recycle Bin Records" paper;
  version 4 = 280-byte records, version 5 = the 800-byte Unicode records of
  2000/XP). Only the record size and the field order are relied on.
- Confirmation strings are Windows 98's shell32 wording ("Are you sure you
  want to send '%s' to the Recycle Bin?", "... these %d items ...", "...
  remove the folder '%s' and move all its contents to the Recycle Bin?", the
  same three without the bin for a permanent delete).

## Decisions

1. **One service, in the Files model.** `apps/files/recycle.inc` is included at
   the end of `model.c` (declarations in `model.h`), so every program that links
   the Files model has the bin and no build list changes. It owns the policy,
   INFO2, the bin of one drive, the job attachments and the folder view.
2. **fx-17's engine does the work.** A delete is an ordinary `FM_DELETE`
   `fm_job` whose sources may carry a destination (`struct rb_job`, in
   `fm_job.bin`): a recycled item is one `rename()` into `RECYCLED` (a copy
   never happens: the bin is on the same volume), the others are removed as
   before. Restore is an `FM_MOVE` job with a per-source target (the original
   path), so fx-17's replace prompt, progress dialog, cancel and error texts are
   shared. Empty and Delete inside the bin are `FM_DELETE` jobs over stored items.
   fx-19's drop rule is unchanged: the bin icon and the bin window accept a
   move only (never a copy), refuse a CD and items already in a bin.
3. **Records follow what moved.** After the count pass the job reserves record
   numbers (INFO2's header carries the next number) and decides per item;
   the records are written when the job ends, for the items that completed (a
   cancelled job records the finished items only). Whatever a crash leaves
   stored without a record is adopted on the next listing as an *orphan*
   (listed under its stored name, location unknown): Delete and Empty remove it,
   Restore refuses it. A record whose stored item is gone is dropped.
4. **Per volume.** `C:\RECYCLED` (`/RECYCLED`), `D:\RECYCLED` (`/mnt/d/RECYCLED`)...
   exist only for fixed disks: the media state (`removable_mask`) of the program
   that deletes says which drives are fixed (`fm_model.fixed`, bit 0 = A:; C: is
   always fixed; before the first media read only C: counts). Everything on
   removable or optical drives deletes at once, as Windows 98 does. `RECYCLED` is
   hidden at every volume root (`fm_hidden`), like Windows' system folder.
5. **INFO2 writes** are exclusive-temporary + replacing rename (as the file
   clipboard), under `INFO2.lck` (exclusive create; a lock older than 30 s is
   abandoned; a lock held longer than 1.5 s fails the job with EBUSY and leaves the
   items where they are), because Files and the shell may both delete.
6. **The bin as a location.** `Recycle Bin` (the typed address; the virtual path
   `FM_RECYCLE`, whose ':' cannot occur in a FAT name) lists every fixed drive's
   items. Entries keep the stored name internally (unique, safe for selection and
   refresh) and show the original name through their record; `fm_entry_path`
   returns the stored path so every job works on real paths. Details columns:
   Name, Original Location, Date Deleted, Type, Size, Modified; sort keys 4 and 5
   exist only there. Cut, Copy, Paste, New, Rename and dragging out are
   disabled: items leave the bin through Restore.
7. **Restore** moves to the original path and recreates missing folders
   silently (the directive's "recreating folders"); an existing name raises the
   ordinary replace prompt; the record is removed when the item moved.
8. **Policy** in `/system/settings/recycle.cfg` (text): `confirm`, `independent`,
   `nuke`, `percent`, `drive.<l>.nuke`, `drive.<l>.percent`. Percentages are 1-100
   (Windows' slider allows 0; 0 would refuse every delete). The *Display delete
   confirmation dialog* option governs every delete prompt, permanent ones too
   (Windows 98 has no separate nuke prompt; `FOF_WANTNUKEWARNING` is NT 5).
9. **Size limit.** The maximum is `percent` of the drive. When the limit is
   exceeded the oldest items are removed for good (oldest deletion time, then
   lowest record number), never the items of the running job. An item (or the
   rest of a batch) that cannot fit even in an emptied bin is **refused**: it is
   left where it was and a report says so ("... is too large for the Recycle Bin
   and was not deleted. Increase the maximum size, or hold Shift ..."). Windows 98
   offers to delete it permanently instead; refusing never loses data. The drive
   capacity has no ABI source yet (G6, FM-22): until it exists the limit uses
   `RB_ASSUMED_CAPACITY` (512 MiB, the canonical image's size), so on a larger disk
   the bin is smaller than Windows would make it, never larger. `rb_capacity_hook`
   is the seam G6 fills.
10. **A path longer than INFO2's 260 bytes** (a CiukiOS path can be up to 1039) is
    refused with a report, like a too-large item.
11. **Migration of the old Trash.** Items in `/Documents/Trash` (the pre-G2
    desktop Trash, which only ever received Desktop items) become bin records with
    the Desktop (`C:\Documents\Desktop\<name>`) as original location and their
    modification time as deletion time; the folder is removed when empty. It runs
    at the first write to the bin, at the first listing of the bin and once in the
    desktop's folder worker; until then the desktop icon is full. The image builder
    no longer creates `/Documents/Trash`; `RECYCLED` is created on first use.
12. **Desktop.** The icon is "Recycle Bin" (its saved position keeps the key
    `/Trash`), the open or full picture is Tango's `user-trash` or `user-trash-full`
    (polled once a second beside the Desktop folder, updated at once after the
    shell's own jobs). Menu: Open, Empty Recycle Bin (disabled when empty),
    Properties (Files opens at the Recycle Bin Properties dialog). Delete, Shift+Delete
    and the drop on the icon go through one request that asks first (a Yes/No
    question in the shell's message window: Enter, Y or the Yes button; Esc, N, the
    No button or closing the window answer No). The `empty_recycle` sound event
    (`SOUND_EMPTY_RECYCLE`) plays when an Empty finishes, from the desktop and from Files.
13. **Properties** (Files, Recycle Bin Properties): one setting for all drives or
    one per drive (the Drive button walks Global, C:, D:, ...), *Do not move
    files to the Recycle Bin*, the maximum size as a percentage (with the size it
    makes) and *Display delete confirmation dialog*. The toolkit has no slider or
    tab strip that reports to its application inside a dialog, so the size is an edit
    field and the drive choice a button; the settings are the same as Windows 98's.
    Item properties (double click or Properties on an item): name, type, original
    location, deletion time, size, modified time.

## Win32

`SHFileOperationA` with `FO_DELETE | FOF_ALLOWUNDO` (`sdk/win32/shell/fileop.c`,
`rb_recycle`) no longer fails with ERROR_CALL_NOT_IMPLEMENTED: it moves the item
into `X:\RECYCLED\D<drive><n>[.ext]` and appends the same INFO2 record the native
service writes (under the same `INFO2.lck`), so Files shows and restores it.
The policy file is honoured (`nuke`, per-drive `drive.<l>.nuke`, and the size
percentage: an item above the limit fails with ERROR_DISK_FULL, never lost).
Items go to the bin only when the drive is `DRIVE_FIXED`, the path is absolute
and not already in a bin; everything else, and every delete without
`FOF_ALLOWUNDO`, is deleted at once as before. The confirmation says "Send ... to
the Recycle Bin" when the items will be recycled. Win32 deletes do not trim the
bin to its limit (the next native delete does); `FOF_WANTNUKEWARNING` and name
mappings (`0x20` = `FOF_WANTMAPPINGHANDLE`) are not provided (error 120). The
loader's shell import table gains `GetDriveTypeA`, `GetSystemTime`,
`SystemTimeToFileTime` and `Sleep` (`scripts/win32/shell.py`). Tested in
`tests/host/win32/installers/shell_test.c` (INFO2 bytes, FILETIME, numbering).

## Remaining gaps (explicit)

- Dragging items out of the bin (Windows allows it) and Cut/Copy in the bin are not
  offered; Restore is the way out. An orphan (no record) can only be deleted.
- No "Undo Delete" (Edit menu) and no Create Shortcut/Properties of the bin's own
  Rename/Size options; undo of a delete belongs to G8.
- Maximum size relative to the real drive capacity waits for G6 (see 9).
- Network drives are not a CiukiOS concept yet (NW-12); they would be removable.
- Each drive's bin settings of drives without a letter in the drive walk (the
  Drive button lists the drives that exist when the dialog opens).
