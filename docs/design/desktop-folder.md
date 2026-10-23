# Desktop folder (f2-42)

## F2-80: asynchronous browser and media snapshots

Research before implementation (2026-10-17): POSIX
[thread creation](https://pubs.opengroup.org/onlinepubs/000095399/functions/pthread_create.html)
provides independently scheduled work;
[mutex trylock](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)
allows consumers to defer a busy mailbox, while
[join](https://pubs.opengroup.org/onlinepubs/009695399/functions/pthread_join.html)
waits for termination and therefore belongs only to teardown.
[Directory streams](https://pubs.opengroup.org/onlinepubs/007904875/functions/readdir_r.html)
reuse returned storage, so the worker copies names and metadata into its owned
result. [Read](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html)
permits short transfers and EINTR; on-open ELF classification handles both.

The existing Files controller calls canonicalization, listing, tree enumeration,
Games availability checks and media-label reads on its UI thread. Move these
reads to one cancellable worker, publish owned snapshots with a revision, and
apply them only on the UI thread. Cancellation is cooperative at VFS call and
entry boundaries; an in-flight device call must return before it can stop.
Browser enumeration always uses FM_LIST_NAMES_ONLY and the existing Ciuki
dirent metadata extension. Extension-less files are classified on open, on the
worker, after the browser has painted; the initial icons use name/type only.
Paste arriving in the same input batch as navigation waits for its accepted
revision; a newer navigation discards that pending action. Explicit operations
retain fm_job. Desktop media observations similarly use a
worker-owned descriptor and a mailbox; shell state and AutoPlay decisions are
updated only by the frame thread. AutoPlay confirmation waits for a fresh media
observation. Failed observations preserve the previous successful state.

The shell projects `/Documents/Desktop` after Computer, Documents and Trash.
File operations reuse the Files VFS/model and incremental copy/move engine;
Trash is `/Documents/Trash`, with unique names and no overwriting. The image
builder creates both directories. The existing Files FAT name validation is
shared, including forbidden characters and trailing spaces/periods. Microsoft
[filename conventions](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)
confirm these restrictions. CiukiOS retains its existing POSIX/FAT rules rather
than adding Windows device namespace rules.

Association selection uses the existing CiukiOS catalogue; Open With lists
matching applications and CiukNote for text/unknown files. This follows the
separation between default and supported handlers in the upstream
[MIME applications specification](https://specifications.freedesktop.org/mime-apps-spec/latest/).
No XDG file format or Windows shell API is introduced.

Positions use hex-encoded full names in desktop.cfg's preserved extension keys,
so spaces, equals signs and non-ASCII bytes do not alias. Arrange clears these
keys and restores the fixed system icons followed by the model's sorted entries.
Shared file clipboard data is atomically published via the Files VFS, separate
from the text clipboard. Drag uses a small v2 desktop notification: Files first
publishes its selection, then requests a drop using the compositor's current
pointer location. Only desktop-launched Files clients can send or receive file
transfer notifications. Ctrl copies; an ordinary drag moves. No replacement is
automatic: the shell reports a collision and leaves the source intact; Files
uses its existing replacement prompt.

The shared clipboard uses exclusive temporary creation and replacing rename,
following POSIX [open](https://pubs.opengroup.org/onlinepubs/9799919799/functions/open.html)
and [rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html).
Each writer owns its temporary file; the previous clipboard survives a failed
write, sync or publication. Move completion compares the stored selection before
clearing it, preserving a newer selection from another window. This adds no
filesystem API: it uses the existing F2 VFS operations and error conventions.

Drops onto a folder icon use that folder; free desktop space uses Desktop.
Moving an icon over free space saves its position. Source paths never travel in
the small drop message, so transfers retain the Files model's path capacity.
The broker checks the sender PID, negotiated v2 packet shape, Files identity,
window existence and power/gate state. Other programs cannot request a file drop.
Input resynchronization or focus loss cancels an unfinished Files drag. Shutdown
cancels a desktop copy/move job and removes its temporary copy before the shell
stops servicing filesystem requests.

Settings extension storage is bounded at 16 KiB, with a 32 KiB settings-file
limit; an exhausted position store reports an error. Icons and label caches are
allocated as needed, using the existing Files listing bound (4096 entries), and
allocation failure leaves a consistent prefix rather than changing file indices.
The existing LAUNCH argument bound (160 bytes) still applies when opening files.

## F4-07 merge integration (2026-10-13)

Rechecked the upstream [MIME applications specification](https://specifications.freedesktop.org/mime-apps/latest/)
against `catalog.h` and the shell: default association launches and Open With
remain catalogue-driven, including Music Player's WAV/MP3/OGG/M3U handlers.
The conditional Pictures shortcut follows the projected Desktop entries;
Computer, Documents and Trash remain indices 0..2 and model entries remain
index 3 + model index. Filesystem actions and drop targets check the model
bounds, so Pictures cannot be renamed, deleted or treated as a Desktop entry.
Its saved position uses `/Pictures`, distinct from a Desktop file named
`Pictures`. The icon capacity includes this extra shortcut. Files retains both
Desktop navigation and Play in Music Player with distinct command IDs.

Image Viewer precedes Paint in the catalogue, retaining F4-07's default image
associations. Paint still advertises BMP/PNG support so F2-42's Open With menu
can launch the editor. Regression checks exercise both the default Viewer
launch and explicit Open With > CiukPaint for the same Desktop BMP.

## fx-17: drop effect, moves across drives, removable media errors (2026-10-23)

T23 trial of `e831e685`: a song dragged from the stick (E:) to the desktop
ran as `[files] job=move` (before fx-19 Files sent "copy" only for Ctrl;
anything else moved), copied 7,139,466 bytes, then failed to delete the read-only
source and reported `errors=1` with "Cannot complete the file operation …
The original item is kept", although both copies existed.

Research: [Handling Shell Data Transfer Scenarios](https://learn.microsoft.com/en-us/windows/win32/shell/datascenarios)
(a conventional move is a copy followed by deleting the original; drop
targets support right-drag); Explorer's rule as documented by Microsoft's
community answers ([ways to drag files or folders](https://answers.microsoft.com/en-us/windows/forum/all/ways-to-drag-files-or-folders/b83a8ee6-a9d6-4e8e-8e9b-778dc6d90b8b):
Ctrl copies, Shift moves, right-drag offers Copy Here/Move Here/Create
Shortcuts Here) and [Windows XP Pocket Reference ch. 2](https://oreilly.com/library/view/windows-xp-pocket/0596004257/ch02.html)
(a drag within one drive moves, to another drive copies); Windows 98
Resource Kit ch. 10 (removable media volume tracking reports improper
removal).

Drop effect: one rule, fx-19's `fm_drop_effect()` in `apps/files/model.c`
(Ctrl copies, Shift moves, Ctrl+Shift/Alt link; otherwise the same drive
moves and another drive copies), called by Files and the desktop. fx-17
does not duplicate it; the transfer engine below performs whatever effect
that rule chose. Cut/Paste is a move (copy then delete across drives).

Transfers (`fm_job_step()`):

- A cross-volume move publishes the complete copy, then deletes the
  source. A source that cannot be deleted (read-only or write-protected
  medium, access denied, in use) keeps both copies: the item counts as
  copied, the job ends `FM_DONE` with `kept`/`kept_folders`, and
  `fm_job_report()` shows a warning "'NAME' was copied to DEST, but the
  original on E: could not be deleted: REASON. Both copies are kept." It
  never says the copy failed. A rename refused by a read-only source
  volume before its EXDEV check also falls back to copy when the
  destination is on another volume (`destination_dev`).
- A move within a read-only medium fails at once with nothing copied.
- Every failure names the item and Explorer's reason text; a removed
  medium (EIO/ENOENT whose `/mnt/<x>` root then disappears, polled up to
  2 s for the 250 ms port scan) is reported as "the disk in drive E: was
  removed", with the number of completed items and whether the partial
  copy was deleted or may remain on the removed destination.
- Copies read 64 KiB per step (progress and Cancel stay responsive) and
  write up to 1 MiB per `write()`: each FAT `write()` is one durable growth
  group closed by device cache flushes, so a stick with 8 KiB clusters
  receives 4 flushes per MiB instead of 64.

