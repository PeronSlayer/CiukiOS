# External-data and host-limit audit

Research and implementation record for `docs/directives/fx-classaudit.md`.
The detailed case inventory and command results are delivered in
`build/codex/fx-classaudit-report.md`.

## Decisions supported by primary sources

* Microsoft [FAT specification, sections 3–5](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
  describes FSInfo as cached free-space information, with signatures that
  must be checked before using it. Linux
  [fat_read_bpb/fat_fill_super](https://raw.githubusercontent.com/torvalds/linux/master/fs/fat/inode.c)
  warns on bad FSInfo signatures and ignores those hints; it does not
  validate executable jump instructions. Accept nonbootable data volumes.
  Disable hint writes when the FSInfo location or signatures are unusable;
  never repair an unrecognized reserved sector opportunistically.
* Linux [isofs inode parsing](https://raw.githubusercontent.com/torvalds/linux/master/fs/isofs/inode.c)
  consumes directory/extent information without rejecting reserved flag bits
  or file-structure-version metadata. These fields become diagnostics;
  extent, record, interleave and buffer bounds remain enforced.
* Microsoft [AutoRun entries](https://learn.microsoft.com/en-us/windows/win32/shell/autorun-cmds)
  and Wine's [profile parser](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/kernel32/profile.c)
  distinguish recognized settings from unrelated keys. An unknown key's
  value must not invalidate recognized AutoRun settings.
* The local Windows 98 Resource Kit, `wrkc27_3.htm`, "Troubleshooting
  Windows 98 Startup", describes Safe Mode as a minimal runtime driver
  configuration. It permits deliberate configuration edits for recovery;
  suppressing all user preference writes in Safe Mode would be incorrect.
  Runtime fallbacks and explicit user edits must remain separate.
* Wine 10.0 `server/mapping.c`, `get_image_params`, reads the PE header at
  the DOS `e_lfanew` byte offset without an alignment refusal
  ([upstream source](https://github.com/wine-mirror/wine/blob/wine-10.0/server/mapping.c)).
  Ciuki's decoder already reads integers bytewise and checks file/image
  bounds. Turn only the extra offset-alignment refusal into a diagnostic;
  do not change the NE loader assigned to `fx-is5boot`.
  The host census records the same warning, and the malformed-offset
  fixture still refuses because its PE signature is missing at that offset.
* Wine's [MIDI_mciReadMThd](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mciseq/mcimidi.c)
  normalizes a format-0 file's declared track count to one. Apply the same
  normalization with a diagnostic before the track-capacity check. The
  actual track chunk, event, time-division and payload bounds still apply.
* Wine's [icon DIB decoder](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/user32/cursoricon.c)
  treats width and height independently. Accept bounded rectangular
  uncompressed ICO/CUR images, checking both XOR and AND extents before
  resampling to USER's existing 32-by-32 storage.

## Host constraints

The production PMM's LOW/DMA/NORMAL boundaries, 32 KiB kernel stack and
fixed kernel tables are the authority. Host PFN fixtures must exclude LOW,
prefer NORMAL, fall back to DMA only for unconstrained requests, and never
assemble contiguous runs across a zone boundary. Keep target stack tests
and user-copy permission/fault tests distinct from host libc execution.
No hardware-memory limit is changed by this audit.

## Findings requiring coordination with the display-preference task

Control Panel's `app.c` initializes a merge snapshot to defaults and ignores
read failure before saving it. The desktop's startup `settings_load` also
treats every read failure as first boot; its asynchronous reload correctly
distinguishes ENOENT from other errors. A transient read error must not let
a later unrelated preference write replace normal preferences with defaults.
These shared files are owned by running display-preference work. The report
records the required error/ENOENT distinction and fault-injection regression
for the lead, without making a competing multi-line edit.
