# File, folder and drive properties (G6)

Directive: `docs/directives/g1-g6-os-gaps-batch-1.md`, section G6.

## Reference behaviour and implementation decisions

Research: Microsoft's [property sheets](https://learn.microsoft.com/en-us/windows/win32/controls/property-sheets)
use tabbed pages and OK, Cancel, Apply: Cancel discards pending edits, Apply
persists without closing, and a failed Apply leaves the sheet open.
The owner's Windows 98 Resource Kit, Appendix G (`wrkappg_1.htm`, keyboard
shortcuts), specifies Alt+Enter for Properties and Ctrl+Tab for page changes.
Chapter 10 (`wrkc10_7.htm`, Long File Names; `wrkc10_8.htm`, Technical Notes)
puts the existing 8.3 alias in General, never a freshly generated alias.
Chapter 10 (`wrkc10_5.htm`, Disk Management) puts disk utilities on Tools.
These local references are read-only in the lead checkout's `build/rskt`.

[File attributes](https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants)
and [SetFileAttributes](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileattributesa)
make Read-only, Hidden, System and Archive independent bits. Directory is
structural and cannot be changed. Directory Read-only does not deny child
writes. Browser visibility is separate from recursive operations; copies,
deletes and folder totals include hidden files. Hiding a known extension
changes only display text, never the pathname used by an operation.

[File Times](https://learn.microsoft.com/en-us/windows/win32/sysinfo/file-times)
specifies FAT creation resolution 10 ms, last write 2 s and access one day,
all stored as local calendar time (including custom rules with seconds in
the UTC offset). Preserve G1's local-to-UTC rule by date
and add its inverse for metadata writes. Missing stamps show a dash, not a
manufactured epoch. Read-only media expose metadata but refuse changes.

[GetDiskFreeSpaceEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespaceexa)
requires 64-bit byte arithmetic. Reuse fx-17b's MEDIA_GET_SPACE; COUNTING
means unknown free space and must never become a misleading pie chart.
[SetVolumeLabel](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setvolumelabela)
and Microsoft's [FAT 1.03 specification](https://www.microsoft.com/en-us/download/details.aspx?id=53426)
require at most 11 FAT label bytes, uppercase and validated punctuation.
Write the root volume entry, primary BPB label and FAT32 backup BPB through
the existing FAT publication/flush machinery. Empty input removes a label.

The implementation is additive: syscall 98 follows G1 TIME=97 and
Win32=96; MEDIA_GET_LABEL/SET_LABEL use 0x4d18/0x4d19, after the existing
CD_POSITION=0x4d17 (the inherited 0x4d07/08 proposals are replaced). The 808-byte dirent
keeps its size and all non-reserved field offsets, using one reserved byte for
attributes. A separate versioned 88-byte file-info record avoids changing
stat. ABI oracle coverage and regenerated API reference are mandatory.

## Scope boundaries

General sheets, attributes, metadata, byte totals, drive label and pie,
listing columns, status free space and folder visibility options are G6.
The Tools page must describe unavailable ScanDisk, Backup and Defragmenter
honestly: their engines are inventory G29 and do not exist in this tree.
Sharing requires the absent file-sharing service. PE32 program version resources are parsed as described below; NE/Win16
resources follow the separately deferred Win16 subsystem.

The program Version page reads PE32 RT_VERSION resources without loading or
executing the image. Microsoft's [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[VS_VERSIONINFO](https://learn.microsoft.com/en-us/windows/win32/menurc/vs-versioninfo)
and [VS_FIXEDFILEINFO](https://learn.microsoft.com/en-us/windows/win32/api/verrsrc/ns-verrsrc-vs_fixedfileinfo)
define the section/RVA mapping, 32-bit alignment and version components.
Bound every read by the file and section sizes, cap sections at 96 and resource
directories at 256 entries, cap the version blob at 65535 bytes and nesting at
four. Display fixed file/product versions plus available string properties;
malformed or absent resources do not produce invented version information.
This is a native read-only parser, independent of the Win32 DLL entry points.

## Delivered behaviour and limitations

File/folder General shows the FAT alias, available creation/access/write
stamps in G1 local time, logical bytes, allocated file bytes and recursive
contents. The existing transfer worker reads property metadata and PE resources, then
counts folders including hidden entries;
drive sheets query space directly, without traversing the drive. R/H/A/S edits
support mixed selections: an indeterminate box is a dash and changing one bit
preserves each item's other bits. Individual directory entries are durable;
a multi-selection can partially complete if a later item fails. Failed Apply
keeps pending edits and the error visible. A volume or file marked read-only
cannot be changed through its disabled controls; the kernel remains authoritative.

The View menu offers the optional Attributes column and Folder Options.
Visibility applies to the listing and tree, including attribute-only refreshes.
Known-extension hiding changes labels only. Options and the optional Attributes column are saved in the bounded, versioned
`/system/settings/files.view` through exclusive creation, sync and replacing
rename. Later Files windows read them on their browser worker during startup. Existing windows keep their
current view until changed; there is no live settings broadcast.
Read-only ISO/NTFS space queries remain unavailable rather than reporting a
fabricated FAT cluster count. Tools has no functioning utility actions until
the G29 engines exist; Sharing needs the file-sharing service. Desktop real
objects route context Properties and Alt+Enter to the same Files sheet using
`--properties=<path>`; virtual system objects and arbitrary shortcuts keep their
existing shell pages. No boot image or runtime hardware evidence is claimed.

## G2 integration

The merge rechecks Microsoft's [property-sheet contract](https://learn.microsoft.com/en-us/windows/win32/controls/property-sheets)
and [SHFILEOPSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shfileopstructa)
against the two existing controllers. Normal filesystem objects retain the G6
transactional sheet; Recycle Bin items retain G2's information dialog because
their original location and deletion date belong to the INFO2 record, whereas
the generic sheet describes the stored filesystem path. Bin policy Properties
remain separate. The normal Attributes column and the bin's Original Location
sort key both use model key 4, interpreted according to the current location;
only the bin accepts deletion-date key 5. Crossing the bin boundary resets
location-specific sort keys to Name. Browser visibility options never
filter the bin's records or recursive operations.

The lead-approved command allocation preserves G2 Recycle Bin commands
305–309 and assigns G6 Folder Options 310 and Attributes column 311.
Microsoft's [WM_COMMAND reference](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-command)
identifies menu commands by their command identifier; the Files controller
likewise dispatches activation events by ID. Distinct IDs therefore preserve
both command routes. Bin dialogs remain 207–210; Insert Disk is 211.

## fx-28 drive presentation

Drive General now shows grouped exact bytes and binary MB together. A blue/purple
elliptical disc with a darker front wall replaces the flat circle; legend squares
use exactly the same two top colours. Counting omits the disc and swatches, keeps
capacity visible and reports calculating for used/free; unknown space remains
unavailable. Free clusters are clamped to total for both numbers and the disc.
See [fx-28's researched contract](screenshot-review-fx-28.md).
