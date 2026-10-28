# File, handle and mapping enforcement (sec-k03)

This records the research, the repository comparison and the decisions of
[sec-k03](../directives/sec-wave3.md) (contract row 3A of
[user-security-contract.md](../design/user-security-contract.md#implementation-plan),
enforcement table in
[user-security-kernel.md](../design/user-security-kernel.md#enforcement-points-and-retained-handles)).
Exact commands and results are in `build/codex/sec-k03-report.md` of the
implementing worktree. No QEMU run or image build is part of this step.

## Primary sources

* Microsoft [File Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights):
  access is decided when a handle is created and the granted rights travel with
  the handle; a parent's descriptor is not used for a child unless its ACEs are
  inherited. CiukiOS decision: rights are frozen into the description at open
  (`file_description.granted`, `vfs_description.granted`) and only shrink;
  inherited labels give children their parent's ownership.
* Microsoft [CreateFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea):
  `TRUNCATE_EXISTING` requires `GENERIC_WRITE`; delete access allows delete
  and rename; `FILE_FLAG_DELETE_ON_CLOSE` deletes after the last handle.
  Decision: `O_TRUNC` needs WRITE before data is cleared; Win32 DELETE access
  needs REMOVE at open; a deferred delete acts on the authorized node
  identity at the last close, never a later path lookup.
* Microsoft [MoveFileEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa):
  rename needs delete permission on the source (or delete-child on its
  parent) and the destination directory's add permission; a moved file keeps
  its descriptor within a volume. Decision: RENAME on the source object, ADD
  on the destination parent, REMOVE on a replaced object. CiukiOS is
  deliberately stricter than Windows: a move across an ownership/policy
  boundary is administrator/SYSTEM only and returns EBUSY while anything in
  the moved subtree is open, mapped or a cwd (contract text), because labels
  are recomputed from the destination.
* Microsoft [CreateFileMapping](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga):
  `PAGE_READONLY` needs a `GENERIC_READ` handle, `PAGE_READWRITE` needs
  `GENERIC_READ|GENERIC_WRITE`. Decision: a file section needs the caller's
  current READ (plus WRITE for a shared writable section) on the source
  object; each new view repeats the check; paging and writeback then run
  under that admission only.
* The Open Group [rename()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/rename.html):
  write permission on both containing directories, search permission on every
  prefix (EACCES), no change on failure. Decision: traversal is checked by the
  resolver for every real component including `..`; a denied rename changes
  nothing (H04 checks the namespace afterwards).
* The supplied Windows 98 Resource Kit (read in place under
  `/home/peronslayer/Desktop/CiukiOS/build/rskt`) has no per-user file
  security; it remains historical context only. DOS stays a V86 guest whose
  VMM file arbitration enters this VFS authorization (k05 binds the subject).

## Repository comparison and decisions

| Area | Before | sec-k03 |
| --- | --- | --- |
| Native pathname calls (`proc/syscalls_file.c`) | `px_*_locked` without identity | `px_*_as_locked(..., p->subject)`; CHDIR adds TRAVERSE; FILE_INFO GET/SET via READ/META |
| Setup FAT devctl (`proc/syscalls_file.c`) | Setup capability only | plus META (attrib) / READ (query) on the retained description, before the Setup adapter |
| Native descriptions (`proc/fdtable.c`) | access mode only | retained subject + granted rights; every read/write/truncate/fstat/getdents rechecks the caller (live subject, granted ⊆, current label, O(1) cached) |
| Exec (`fdtable.c` image ops) | path resolution only | READ\|EXEC as the subject before any byte is read; `security_exec_open` (k01 seam) authorizes as the prepared child; every open re-authorizes, so a snapshot is never a grant |
| Creation | FAT create | ADD on the actual parent plus the destination name's own rule (reserved, sealed, hive and service names); Standard root entries run `owner_root_reserve` → create → `owner_root_finish` |
| Removal/rename | FAT remove/rename | REMOVE/RENAME on the object, ADD on the destination parent, REMOVE on a replaced object, OWNER lease, creator-root change/finish, cross-boundary EBUSY |
| Legacy tables (`fs/vfs.c`) | no identity | `struct vfs_table.subject`; every path/handle call authorized by the same resolver **before** the legacy lookup reads anything; the legacy lookup must reach the authorized identity (else retried, then EBUSY; kernel tables keep the legacy result) |
| Win32 (`proc/win32/files.c, sections.c, loader.c, personality.c`) | `/System` write ban by personality | subject on every call and find table; K-Q4: managed volume uses the common policy, builtin store `C:\System\Win32` and system fonts stay read-only; unmanaged volume keeps the old ban; section/view authorization; cached source image re-authorized |

### Unmanaged volumes

The integrated base binds no policy volume at boot yet (OWNER provisioning
and the boot binding are lead/sec-user-02 integration). A drive without a
binding is unmanaged and decided by `px_policy_check_locked` itself:
any live subject reads/traverses/executes; SYSTEM and administrators modify;
Guest never modifies; Standard users modify removable media only. A fixed
volume not yet provisioned is therefore public-readable and
administrator-writable, the contract's rule for managed data without owner
records. Missing subjects still deny everywhere; no path becomes SYSTEM.
This keeps the interim uid-1000 administrator desktop, its games, DOS and
installers working before binding, and makes Standard sessions read-only on
the system volume until it is bound.

The sec-k05 DOS host views use the same adapter, so a user VM's E: on an
unbound removable volume reads like the native namespace (the contract's
"Unmanaged removable" row) instead of the fail-closed DOS 5 sec-k05 had
assumed before this resolver existed; the read-only E: provider still
refuses every write (H06 E: view, sec-wave3 integration).

The k02 adapter `vfs_resolve_subject_locked` moved from `fs/security_path.c`
to `proc/posixpath.c` unchanged except for the unbound-volume rule above, so
that every object linking the native resolver also has the legacy-table
adapter (a harness or kernel without it would deny every subject table).

### Kernel attribution

`PX_KERNEL` is an explicit attribution for kernel-internal SYSTEM work
(probes, net settings, OWNER backend tables, legacy kernel tables from
`vfs_table_init`). The historical `px_*_locked` names equal the `_as` forms
with `PX_KERNEL`; H04 checks by source census that no user frontend calls
them. Process-attributed frontends use `px_process_subject(p)`: the process's
retained subject, or NULL (denied) when it has none. Only a standalone host
harness that links no credential implementation (`proc/security.c`, weak
symbols absent) attributes a process to `PX_KERNEL`, matching its behaviour
before this step; the kernel always links that implementation.
Kernel-internal maintenance of creator-owned root entries carries no
OWNER transaction (the H02 stale-slot fixture relies on this); a stale record
then denies.

### Creator-owned root entries

Creation reserves the name with placeholder evidence (the slot does not exist
yet), creates the entry under the caller's writer turn, then publishes the
real object proof. While pending the name is deny-labelled for everyone.
Rename inside the root uses `owner_root_change` + finish with the new slot;
leaving the root (administrators only) retires the record. An open creator
entry cannot be deleted (EBUSY): a deferred final close inside another
frontend's teardown cannot carry an OWNER transaction. Delete-on-close
(DELETE access) on the root entry itself is refused for the same reason.
Every creator operation holds the volume's writer turn from reserve/change
to finish, so an intent seen by a turn holder is stale (crash, power cut or a
lost finish). The next creator mutation, or the integration once at mount
(`px_creator_reconcile_locked`), reconciles it from the namespace: a pending
create whose name exists publishes that object's proof, otherwise it is
aborted (retired); a pending rename finishes when only the new name exists
and aborts when only the old one exists; a pending delete retires when the
old name is gone. Contradictory evidence keeps the intent deny-labelled and
returns EIO. Without this a stale intent would block every later root and
account batch with EBUSY.
Retired names remain tombstones (k02 `owner_root_occupied`); creating one
again returns EEXIST for every subject. Legacy tables cannot create, rename
or delete creator entries (DOS host views are read-only or removable).

## Evidence summary

H04 (`tests/host/security/test_file_enforcement.py`) links the production
code listed above on the real FAT/VFS. It covers profile privacy for native,
Win32 and legacy calls (including 8.3 aliases and `other/../mine`), a spy
block device proving that denied operations issue no read of the private
directory/file sectors and no write, read-only descriptions, `O_TRUNC`,
attributes, dead subjects, section admission, creator roots (create, other
users, Guest, rename transaction, open EBUSY, retirement, tombstone, slot
reuse), rename endpoints and cross-root EBUSY, unmanaged/removable volumes,
Guest, SECRET, devices, K-Q4, deferred delete, OWNER lease and missing
subjects, plus a source census of the frontends.
