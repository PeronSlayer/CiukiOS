# Managed-volume policy and OWNER implementation

This records the private kernel interfaces implemented for
[sec-k02](../directives/sec-k02.md). It does not establish system-wide syscall
enforcement: sec-k03 owns that work. The account adapter and authenticated boot
publication remain lead integration gates. Exact commands, exit status and
environment limitations are in `build/codex/sec-k02-report.md`.

## Research and repository decisions

* Microsoft [MS02-064](https://learn.microsoft.com/en-us/security-updates/securitybulletins/2002/ms02-064)
  gives the XP root Creator Owner and Users permissions. The approved CiukiOS
  extension also permits ordinary root files. `POLICY_ROOT` permits ADD for
  Standard subjects, while the owner transaction must reserve the side record
  before the directory entry becomes accessible.
* Microsoft's [least-privileged gaming guidance](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/gaming-with-least-privileged-user-accounts)
  distinguishes XP's NTFS protections from FAT32 and places writable game data
  under the user's profile. CiukiOS therefore supplies its own kernel policy;
  it does not infer ownership from FAT attributes or a numeric directory suffix.
* Microsoft [file naming](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)
  describes long/short names, Unicode names and trailing-dot/space restrictions.
  The repository already resolves LFN/8.3 to one short-entry object and pins its
  Unicode comparison in `fs/path.c`. The new adapter uses those resolved names,
  preserves intermediate traversal, and applies Win32 trimming before lookup.
  Managed mounts set `fat_volume.strict_names`: malformed LFN chains fail with
  EILSEQ instead of using the historical alias fallback. Unbound legacy fixtures
  retain their previous parser behavior.
  The FAT engine selects the first drive for a mounted object. Native alias
  resolution therefore reserves both its input spelling and that canonical
  drive; a second FAT instance for the same physical partition is refused at
  policy binding to prevent independent caches from retargeting retained objects.
* Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
  associates object access with process identity. OWNER uses k01's actual
  retained endpoint subject and role checks. Neither a claimed uid nor the
  destination locator in a request can supply authority.
* Microsoft [registry key security](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights)
  separates requested object rights. Authoritative hive persistence is a
  SYSTEM-only policy class; per-profile Settings/Registry and Settings/Restart
  cannot inherit the owner's ordinary write permission. Broker enforcement is
  still sec-k06.

The Open Group access pages cited by the contract could not be retrieved
(Issue 7 returned HTTP 403; Issue 6 was inaccessible). No new POSIX conformance
claim follows from those failed requests. The contract's operation-time
authorization rule remains normative.

The supplied Windows 98 Resource Kit was read in place at
`/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc07_3.htm` (profile layout and
User.dat) and `wrkc18_2.htm` (local/network logon distinction). It supplies
historical profile context, not the new isolation policy. No book/media content
was copied into the repository. DOS architecture is unchanged.

## Private interfaces and integration order

1. Supply a destination VFS table and a protected, nonzero installation/volume
   identity. `owner_vfs_backend` supplies real FAT reads/writes, barriers,
   destination manifest/tree hashing, reservations, idempotent root installation
   and mount-time proof verification. Its account-evidence callbacks must bind
   to the actual protected ACCOUNT journal/current database/checkpoint/enrollment
   map. There is no permissive default implementation.
2. Create/open `owner_store` with explicit fresh provisioning only after the
   authenticated seed/destination gate. Missing/corrupt ordinary stores deny.
   Allocate this structure and `policy_volume` outside the small kernel heap
   (their sizes exceed its 2040-byte allocation bound).
3. Initialize and bind one `policy_volume` per actual mounted volume under VFS
   namespace synchronization. Reusing the same physical FAT mount through an
   alias requires the identical policy instance. Additional fixed volumes use
   their own owner store and the ordinary data-root policy. Foreign FAT labels
   never grant a system-volume identity. Detach expires the binding.
4. Attach the native namespace and call `owner_service_bind` for the installed
   system store. PID 1's existing supervisor maintenance turn dispatches one
   complete OWNER exchange. Request fragmentation, endpoint liveness, admission,
   reply framing and cancellation use the production k01 channel implementation.
   A disconnected requester receives no late reply; its durable operation still
   follows COMMIT_REQUIRED and can be queried through STATUS on a new link.
5. Bind the account adapter's generation/binding/root_occupied/staging/tree/
   snapshot/floor methods to these private interfaces. `owner_profile` refuses
   retired mappings; `owner_root_occupied` includes committed, retired, reserved
   and physically imported roots. Both names in a pending creator-root rename
   remain occupied across restart until finish or abort clears the intent.
   `owner_checkpoint_image` emits the shared
   snapshot; the lead wrapper supplies the current encoded account digest and
   allocation watermarks. Read `floor`/`receipt_count` only during serialized
   account/owner admission. Account metadata-only updates never call OWNER.
6. sec-k03 calls `vfs_resolve_subject_locked` or
   `px_resolve_subject_locked`/`px_resolve_volume_subject_locked`, then authorizes
   actual operation objects/parents under the mutation guard. Missing subjects
   deny. `policy_reference_get_locked` retains the subject/object and freezes the
   granted access mask; a read-only grant cannot become writable. Its check/put
   operations use the same namespace synchronization. Unlinked/reused/expired
   references deny; k03 decides operation-specific fencing and teardown.
7. Creator-root publication uses `owner_root_reserve` then the guarded namespace
   mutation and its durability barrier, then `owner_root_finish`. Rename/delete
   use `owner_root_change` (NULL destination means delete) followed by finish.
   Pending old/new names remain denied on recovery. The future mutator must
   reconcile actual namespace evidence before finishing/aborting that intent.
   Keys, locators and owner generations are never reused; retired records retain
   capacity. Anchor-name checks precede these low-level private transaction calls.
   `owner_vfs_reserved` fences namespace mutation during a profile batch,
   including SYSTEM/ACCOUNT writers; OWNER's own publish callback is the only
   bypass. sec-k03 must consult the lease before admission and after waits.
   It takes a resolved within-volume object/full-child path: staging, profile
   roots and their ancestors are protected; sibling ACCOUNT journals and
   accounts.db.new remain available to complete the transaction.

None of these private hooks is a public SDK registration operation. The existing
syscalls still require the sec-k03/sec-k04 enforcement integration; this change
alone must not enable an authenticated desktop.

## Durable OWNER store

Batch requests/replies implement the frozen 160+128*N and 96+32*N byte codecs.
All request locators are zero. CREATE keys/locators and one owner generation
are reserved by OWNER. Sorted uid/path/key conflicts, stale generations,
unflushed/mismatched staging and exhausted bounds fail before reservation.
The target account generation/digest from the last committed ownership batch
prevents admitting later work against an uninstalled or rolled-back account DB.

Each transition writes an alternate versioned snapshot, flushes it and its
directory/volume, writes its sequence/length/SHA-256 commit marker, and flushes
again. Only a marker identifies committed evidence. A corrupt marked snapshot
closes recovery; an older password/owner map is never silently restored. Any
persistence error, including allocation before the first write, closes admission
until reopen. The snapshot retains allocation highs, retired owners, exact
receipt requests/results, active resolved proofs, root intents and checkpoint
state. Its checksum detects corruption, not hostile offline editing.

RESERVE consumes exactly one receipt even if subsequently aborted before
PREPARE. ABORT of UNKNOWN consumes none. Existing retained keys match their
original digest after store generations advance. At 768 receipts, new ownership
work returns EAGAIN until ACCOUNT performs the coherent two-party checkpoint;
the newest 256 terminal receipts are retained. STATUS/RESERVE/PREPARE/ABORT at or
below the published floor return CHECKPOINTED without results; COMMIT refuses.
Tests turn this window over for 2305 early-aborted batches without consuming
profile capacity. COMMIT_REQUIRED recovery completes the entire profile vector;
orphan precommit reservations abort without lowering any watermark.

The snapshot candidate is durable before CHECKPOINT replies. COMPACT requires
the exact protected ACCOUNT marker, installs the floor durably and then retains
only the suffix. Recovery reads allocation highs from the verified candidate,
not an older in-memory/account watermark. A missing marker retains the old log;
a mismatched marker closes recovery.

## Shared checkpoint codec integration

`CIUKIOCP` version 1 retains the account adapter's existing 160-byte header,
112-byte sorted profile records, and exact request/reply receipt vectors.
When creator records exist, version 2 uses the same header with root_count u32
at offset 140 (144..159 reserved zero). After the profile records it adds
root_count 160-byte creator records, in owner-key order: the first 112 bytes use
the binding codec, the resolved proof occupies 112..143, and 144..159 are zero.
Retired state is action RETIRE; live state is CREATE. Pending root intents block
checkpoint creation. Receipt vectors follow those creator records unchanged.

The current sec-user-01 checkpoint validator accepts only version 1 and requires
140..159 zero. The lead must update that private reader to accept and verify
version 2's sorted creator records, key/locator uniqueness and allocation bounds
before enabling checkpoint traffic with creator roots. `apps/accounts` was not
edited. This extension preserves the specified full owner map and has no public
ABI impact. Batch wire messages remain exactly the frozen version-1 codec.

## Evidence and limits

H02 links the production credential/channel code, policy engine, owner codecs,
durability/recovery code and real FAT/VFS backend. It covers the three-account
and eighteen-binding migration vectors, 32 profiles, 256 creator records, exact
retries, retention/floors, all rule/subject classes, allocation and write/flush
cuts, real destination installation/reload, replaced roots, LFN/8.3 and Unicode
case aliases, mount aliases, cwd/dotdot, Win32 trimming and malformed names.
Warm loops perform one million direct checks and 100000 native cached checks
without storage reads. These are host bounds, not Pentium III timings.

H02 is wired into both filesystem and kernel runners. Direct H02 retains its
default leak detection; the filesystem runner retains its pre-existing explicit
LSan-off policy. Neither sanitizer policy was changed. No QEMU, image build,
systemd scope, commit, merge, diary edit or application account-store edit is
part of this work. No public ABI/SDK header was changed and no new panic path
or vendored library was introduced.

The full filesystem runner exits 0, including the final H02 fixtures and the
reserved-name restart/abort regression. Direct default-LSan H02 and the kernel
runner exit 1 at LeakSanitizer shutdown because ptrace is unavailable in the
implementer sandbox. Their passing assertions are not leak qualification; the
remaining kernel sections were not reached. Exact results are in the report.

The canonical i686 flags, including the unchanged 4096-byte function-frame
limit and supervisor's 1024-byte limit, compile the fourteen changed/new kernel
translation units. The target object symbol audit finds no duplicate external
definition. Type-size evidence from those target objects is:

| Private type | i686 bytes |
| --- | ---: |
| owner_store (32 profile + 256 creator + 32 pending records, receipt pointers) | 49408 |
| owner_record | 140 |
| owner_vfs (including the active request and prior-root leases) | 5600 |
| policy_volume (32 trie nodes) | 2212 |
| policy_label (inline cached-node increment) | 40 |
| policy_reference (retained subject/object/access mask) | 56 |
| px_node (total, including the new label) | 1012 |

Receipts allocate only their actual request/results. At the 768-entry pressure
threshold the absolute 32-binding case uses at most 768 page-rounded 8192-byte
receipts, approximately 6 MiB; normal one-binding receipts fit the small heap.
Serialization workspaces are transient, bounded by the codec maxima, and fail
closed on allocation failure. Checkpoint validation uses two buffers of at most
1423008 bytes. Staging tree nodes consume transient memory proportional to that
staging tree and are released on success/failure; they are not retained ACLs.
The service allocates its three-page exchange buffer only when a request is
pending. These measurements do not change any hardware memory cap.
