# Desktop folder (f2-42)

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
