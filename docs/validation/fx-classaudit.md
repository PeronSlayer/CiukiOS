# External-data and host-limit audit

Research and implementation record for `docs/directives/fx-classaudit.md`.
The detailed case inventory and command results are delivered in
`build/codex/fx-classaudit-report.md`.

## Round 2 decisions and sources

Lead amendment 1 permits read-only NTFS access when only the dirty bit is
set and the journal proves clean. Linux v6.12
[ntfs3/super.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/super.c)
separates volume dirtiness from log replay and gates the dirty-bit refusal
on writable mounts. Ciuki retains its stricter no-replay requirement and
active-hibernation refusal. The existing restart-page clean test follows
[ntfs-3g logfile.c](https://raw.githubusercontent.com/tuxera/ntfs-3g/2022.10.3/libntfs-3g/logfile.c):
no active log client or the clean-shutdown flag. No journal code is imported.
See the amended NTFS section of `f4-audio-media-contract.md`.

Preference loading must publish only a complete read, preserve the previous
snapshot on error, and distinguish missing-at-first-boot from failed I/O.
Microsoft's [fread documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fread)
requires distinguishing short EOF from errors with `ferror`; its
[fopen documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen)
requires checking the failure and `errno`. The local Windows 98 Resource Kit
`wrkc27_3.htm`, "Troubleshooting Windows 98 Startup", describes Safe Mode's
minimal VGA/driver state and temporary diagnostic edits; it does not justify
replacing normal preferences after an unsuccessful read. Merge/save paths
must stop on a failed snapshot read; ENOENT authorizes defaults only before
a complete snapshot has been established. Runtime UI defaults after a failed
initial read are usable but cannot be saved until a complete load succeeds.

The six deferred format/interchange findings from Round 1 are recorded as
`gap` rows in `os-feature-inventory.md`; no decoder expansion is authorized.

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

## Round 1 findings superseded by lead amendment 1

Control Panel's `app.c` initializes a merge snapshot to defaults and ignores
read failure before saving it. The desktop's startup `settings_load` also
treats every read failure as first boot; its asynchronous reload correctly
distinguishes ENOENT from other errors. A transient read error must not let
a later unrelated preference write replace normal preferences with defaults.
Lead amendment 1 explicitly assigns both fixes to Round 2 of this audit.
The shared complete-read helper now rejects partial/error/oversize snapshots;
Control Panel's ordinary, Display, Power and Regional merge saves stop on a
read error. Desktop startup/reload preserves the last complete snapshot and
blocks saves until a complete read succeeds; later ENOENT is not first boot.
The display-mode/lease policy itself remains owned by `fx-dispmode`.
