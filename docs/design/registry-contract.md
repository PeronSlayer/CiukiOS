# F2-47 system registry

Contract recorded and self-reviewed before implementation on 2026-10-13.

> **Superseded in part by user security** (sec-user-06 reconciliation, 2026-10-28). The machine roots (System, Programs) and the
> Win32 machine hive are written only by administrators through the SYSTEM registry broker; per-user
> state binds to the profile and native libreg uses the broker transport (A1-9)
> ([registry and per-profile state](user-security-contract.md#62-registry-and-per-profile-state)). Registry Checker backup,
> restore and repair are broker operations ([sec-sysvc2 amendment](user-security-kernel.md#system-jobs-sealed-boot-settings-and-registry-checker-sec-sysvc2-amendment-2026-10-28)).
> Runtime: sectest `machine-registry` (su-04-roles, acc-accounts item 14); self-test
> `security.registry_backup` (S2-6).

Research and repository comparison:

- [Microsoft registry structure](https://learn.microsoft.com/en-us/windows/win32/sysinfo/structure-of-the-registry)
  defines keys containing both values and subkeys, with case-insensitive names.
- [Microsoft registry value types](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-value-types)
  and [registry import/export](https://support.microsoft.com/en-us/topic/how-to-add-modify-or-delete-registry-subkeys-and-values-by-using-a-reg-file-9c7f37cf-a5e9-e1cd-c4fa-2a26218a1a23)
  motivate typed values and branch exports. Ciuki uses the owner's UTF-8 text
  dialect below, not Windows binary hives or a Win32 API compatibility promise.
- [POSIX rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html),
  [fsync](https://pubs.opengroup.org/onlinepubs/009695399/functions/fsync.html) and
  [durability rationale](https://pubs.opengroup.org/onlinepubs/9799919799/xrat/V4_xbd_chap01.html)
  distinguish replacement visibility from durability.
- [Unicode case folding](https://www.unicode.org/reports/tr44/#CaseFolding.txt)
  supplies locale-independent name comparison. The library pins Unicode 16.0.0
  full default folding (Python 3's Unicode database); spelling is preserved,
  normalization is not applied.

Checked code: `apps/desktop/settings_io.c` writes a temporary, flushes, fsyncs,
then renames, with an unlink fallback. `vfs_rename` in `src/kernel/fs/vfs.c`
uses `fat_rename`, which rejects replacement with EEXIST. The registry must
retain a backup and a durable intent record instead of unlinking the old root.
`ciuki/devices.h` supplies the bounded, retained inventory already used by
Control Panel; querying Hardware must never probe or configure a device.
`libdeskui` already implements tree/list views, input and file dialogs.

## Namespace and bounds

Roots: System, Desktop, Programs, Users (persistent), Hardware (volatile,
read-only). Root spelling is canonical. Public operations take a root handle
and a full `Root\Sub\Key` path; the root itself is a key. Key components and
value names are valid printable UTF-8, at most 64 Unicode scalar values (256
bytes), without control characters. Key components cannot contain `\`, `[` or
`]`, or leading/trailing spaces. Value names may contain these characters;
quoted names escape syntax punctuation. Empty names are allowed for default
values. Maximum depth is 16 components including the root. UTF-8 overlong
sequences, surrogates and out-of-range scalars are rejected.

A root is limited to 256 KiB serialized text, 1024 keys and 4096 values. Strings
are UTF-8, at most 4096 bytes; binary values at most 4096 bytes. Integers are
signed int32; booleans are exactly true/false. Limits are checked before any
persistent mutation. Names use pinned Unicode folding; data does not.

## Text storage and SDK

`/System/Registry/<Root>.reg` lives on the settings volume (the current system
volume). `reg_open_root_at` permits a caller-selected directory, including host
fixtures. Missing roots open as empty trees; reads do not create files.
Example (LF or CRLF accepted):

```ini
; ciuki-registry generation=1
; User comment
[System\Version]
name = "CiukiOS"
version = "0.9.0-pre-alpha"
build = "git hash"
[System\Identity]
Tagline = "Classic computing, one woof at a time."
[System\Example]
count = -123
active = true
bytes = hex:00ff
"name with = punctuation" = "line\nquote\"backslash\\"
```

Strings and quoted names support `\\`, `\"`, `\n`, `\r`, `\t`. Unquoted names
are trimmed; hex has an even number of digits. Blank lines and leading `;`
comments are accepted; trailing `;` comments outside strings are retained.
Comments are collected in encounter order and rewritten before the sections,
so their wording survives while association/spacing is best effort. The
reserved generation comment is emitted exactly once; absent means generation
zero for manually created files. Duplicate sections merge; duplicate values
use the last occurrence. Malformed documents fail as a whole (EINVAL).

The SDK installs `libreg.a` and `ciuki/registry.h`. Open/close, typed get/set,
create key, delete value or recursive branch, immediate-child/value enumeration,
branch export, root-scoped merge import and generation polling are synchronous.
Functions return 0 or -1 with errno; enumeration ends with ENOENT, type mismatch
is EINVAL, insufficient caller buffer is ERANGE, limits are E2BIG, Hardware
mutation is EROFS. Buffer-size queries for strings/binary are supported.
`reg_get_text` and `reg_set_text` expose the same typed literal dialect for the
editor. Paths always include the handle's root. Deleting a root is forbidden.

Import fully parses and validates one root before writing; foreign roots,
Hardware imports and mixed-root files fail without changes. Export contains the
selected key and all descendants, and uses the same durable replacement path.
Import merges, retaining unrelated keys; export is not a deletion script.
Export to registry storage/journal filenames is rejected to avoid bypassing
root generation or Hardware protection. File paths are absolute.

## Replacement, recovery and generation

Every change reloads the latest root, applies one operation to an isolated
tree, serializes, then checks the on-disk generation again before committing.
A changed generation returns EAGAIN; callers retry the operation. Stale handles
therefore apply edits to the latest root without losing unrelated values. The
committed generation increases once per mutation, including import; creating an
existing key is a no-op;
UINT64_MAX is reserved as the polling error result. Readers refresh on access.

There is no interprocess lock: **one writer per root at a time** is mandatory,
including the Registry Editor and the owning application. The generation check
is stale-state detection, not a compare-and-swap. Same-handle concurrent calls
also require caller serialization. Recovery itself is a writer: accesses to a
root with an active journal must follow the same exclusive-access rule. Manually editing a live file must advance
its generation. Root transactions are independent, with no cross-root atomicity.

Write `<path>.new`, flush and fsync; write and fsync `<path>.txn` containing the
new generation, length and checksum; sync its parent directory on POSIX before
installation. On a POSIX host rename replaces atomically.
On native EEXIST, rename the old file to `<path>.bak`, then rename the new file
into place. Fsync the installed file (native FSYNC flushes volume metadata), and
sync the parent directory on POSIX hosts. Remove intent and backup only after
installation is durable. Errors retain recoverable artifacts; success means
the installed document is durable. No old root is unlinked before replacement.

Open/read/write recovers pending transactions: a complete new/current document
must match the intent generation, size and checksum before roll-forward; otherwise
restore the valid backup or keep the old canonical file. A partial `.new`
without durable intent is discarded. Invalid recovery candidates never replace
a valid root. Unrecoverable intent returns EIO with artifacts retained. This is
a recovery journal, not a general filesystem journal. Native replacement has a
brief missing-canonical interval; it cannot promise POSIX atomic visibility or
arbitrary FAT power-loss repair. QEMU/hardware durability qualification belongs
to the lead, outside this host-only directive.

## Hardware

Build a read-only tree from the boot-generated devices API, lazily materialized
on opening the root and refreshed by inventory generation. `Hardware\PCI\bb:dd.f`
contains vendor, device, PCI class/subclass/prog-if, driver, state, name, reason
and resources. Resource entries record type/flags and 64-bit hexadecimal start
and end strings (end exclusive; unknown size remains explicit).
`Hardware\Display`, `Hardware\Audio`, `Hardware\Network` contain counts and
per-device summaries from their existing class nodes, including firmware-first,
absent and quarantined state. PCI summaries reference the full PCI branch with a
`pci` value instead of duplicating resource keys; legacy summaries retain their
resources. This keeps the maximum 64-device inventory within the tree bounds.
Hardware generation is the devices generation;
no Hardware.reg is read, created or modified. The inventory's truncation flag is
exposed. If the API is unavailable, opening Hardware reports its error.

## Editor and migration

`/Programs/regedit`, labelled Registry Editor in Programs > Accessories, has
an expandable tree at left, typed values at right, search through keys/value
names/data, refresh and generation polling. New key, new typed value, edit,
recursive delete, branch export and import use toolkit dialogs. Edits, deletes
and imports require explicit confirmation. Cancellation performs no writes.
Hardware can be browsed/exported; mutation actions are disabled and also rejected
by the library. File and parse errors are shown to the user. Large values that
cannot fit the toolkit input field cannot be edited there (export/import remains
available). The UI uses the existing theme and Tango family.

No settings migration occurs here. Existing `/system/settings/desktop.cfg`,
`/system/settings/sounds.cfg`, and the audio master's dual-record persistence
at `/System/AUDIO.CFG` in `src/kernel/audio/audio_dev.c` remain authoritative. The named follow-up
**F2 registry-settings-migration** (directive number to be assigned by the lead)
will migrate these to Desktop and System\Audio, with one-time import, backward
compatibility, and UI/driver consumer changes in its own reviewed contract.

Phase-1 self-review: the replacement limitation, inventory-only Hardware,
SDK static linkage, toolkit edit limits and unchanged settings ownership above
were checked against the named production code before Phase 2. Acceptance is
host parse/roundtrip/limits/generation/recovery tests, production editor tests
through the desktop toolkit harness, `nice -n 19 make CAP= apps`, and regenerated
developer API documentation. No QEMU, commits or diary for this directive.

## F4-18 master-volume preference

F4-18 supersedes the master-volume part of the migration deferral above.
`System\\Audio` / `Master` is one int32: left percent in bits 0..7, right
percent in bits 8..15, mute in bit 16; other bits must be zero. Both channels
must be 0..100. This single libreg mutation saves a complete stereo/mute
preference without intermediate channel states. Default is 20/20, unmuted;
missing, malformed or invalid registry data falls back to validated AUDIO.CFG,
then the default. A pending System root journal also selects AUDIO.CFG for
that boot; user-space libreg retains its normal recovery protocol. The kernel
uses the same full parser, case folding and bounds as libreg, never a second
text dialect. The desktop's delegated mixer worker saves the preference for
Control Panel, tray and Volume Control, and observes EC changes for saving.
AUDIO.CFG remains the compatibility fallback; other migrations are unchanged.
See [F4-18 research/decision](f4-18-audio-levels.md).
