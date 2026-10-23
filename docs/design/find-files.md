# Find Files or Folders (G7)

## Reference behaviour and implementation decision

Microsoft's Windows 98 Resource Kit, Appendix G, `wrkappg_1.htm` and
`wrkappg_4.htm` (owner's extracted copy, read only), assigns F3 to Find and
Win+F to Find Files or Folders; Win+Ctrl+F is the separate Find Computer action.
Microsoft's [Q133717](https://jeffpar.github.io/kbarchive/kb/133/Q133717/)
(original Microsoft KB text preserved by PCjs) describes Start > Find > Files
or Folders, Named, Look In, Include Subfolders, Find Now, multiple name
specifications, and Date Modified/Advanced criteria. Microsoft's
[Searching for One or More Files](https://learn.microsoft.com/en-us/windows/win32/fileio/searching-for-one-or-more-files)
describes wildcard enumeration and returned size, attributes and times.

Use the existing Files window, worker and file-action controller. F3 opens
criteria for the current folder; desktop F3, Win+F and Start > Find > Files
or Folders open Files with `find:`. This is an ordinary launch argument,
requiring no syscall or protocol allocation. Keep Win+Ctrl+F separate.

The criteria have three pages (Name & Location, Date Modified, Advanced),
with Find Now, Cancel and Next Page. Named accepts case-insensitive `*` and
`?`, `*.*` also matches extensionless names, bare terms match name substrings,
and space/semicolon-separated alternatives accept quoted names. Containing
Text is a literal, case-insensitive byte search, including across read boundaries.
All criteria combine with AND. Look In accepts a folder, a drive or Computer;
Include Subfolders defaults on. Date bounds are inclusive local calendar dates
(YYYY-MM-DD). Size is at least/at most KiB (1024 bytes), with inclusive bounds.
Type selects all items, folders, files or a filename extension pattern.

Searches run on the existing cancellable browser worker. F3 while searching
stops the scan; a new navigation invalidates old results. Content I/O checks
cancellation between reads, closes each descriptor and keeps only bounded
buffers. Recursion is bounded by FM_DEPTH and results by FM_ENTRIES, with at most 65,536 visited items; limits,
unreadable subtrees and content errors are reported as incomplete results,
not as a successful exhaustive search. Virtual device/tmp/mount directories
and the Recycle Bin are excluded as in the normal browser. Computer scans
mounted volumes individually. Result entries keep stable full paths, so equal
names in different folders remain distinct under sorting, selection and file
actions. Details adds an In Folder column. Open, Cut, Copy, Delete, Rename,
Properties and Paste into a result folder reuse existing Files operations.
Creation and background Paste require a concrete destination and are disabled
in results. Open Containing Folder is an additional result action.

The current text matcher folds ASCII case and searches bytes (including binary
files); it does not decode Unicode text or apply locale-specific case mappings.
Criteria use Next Page rather than expanding the toolkit's global dialog limit.

This branch predates G4's `desk_shell_find` placeholder and G6's new metadata;
provide the entry point here and use existing `fm_meta.modified`/`size`.
Creation/access date choices and G6 hidden/system attribute filtering need
integration after G6; Find Computer, Internet and People remain separate gaps.
No guest or hardware qualification is claimed by host tests.
