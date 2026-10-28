# sec-own: OWNER binding and system-volume publication

Directive: `docs/directives/sec-wave4.md` (first round, row "sec-own").
Contracts: `docs/design/user-security-contract.md` 3.3-3.6, 5.2, 6.1, 8.4;
`docs/design/user-security-kernel.md` rule table, "Owner metadata and
consistency" and its sec-own amendment. Inputs: the sec-k03, sec-user-02 and
sec-wire reports (OWNER never bound, volume identity not shared, six missing
owner operations, interim OWNER adapter, `vfs_table_subject` seam).

## Research

- Windows XP creates a new profile by copying the Default User profile; its
  per-user shell folders are Application Data, Desktop, Favorites, My Documents,
  Recent, SendTo, Start Menu (Programs, Startup) and Templates
  ([User Profile Structure](https://technet.microsoft.com/en-us/library/cc775560.aspx),
  [Contents of a user profile](https://technet.microsoft.com/sv-se/library/cc758618(v=ws.10)),
  [Profiles directory](https://learn.microsoft.com/en-us/windows/win32/shell/profiles-directory)).
  CiukiOS images ship no `/Windows/Profiles/Default` template, so a fresh
  profile is staged with exactly the folders the window server already
  registers as the profile's shell folders (`apps/w32srv/storage.c`
  `profile_folders`: Documents, Documents/Desktop, Start Menu, Start
  Menu/Programs, Start Menu/Programs/StartUp, Favorites, Application Data,
  Recent, SendTo, Templates) plus the protected `Settings` anchor
  (k06 `Settings/Registry`, w32srv `Settings/Win32`, k05 `Settings/DOS`).
- Repository comparison: the builder (`scripts/build_image.py`
  `volume_identity`), libsetup and `sdk/private/provision.h` already define the
  volume identity as partition LBA << 32 | FAT32 BPB serial (offset 67); the
  kernel has both values (`storage_volume.part.start`,
  `fat_volume.volume_serial`, read from BPB offset 64+3 for FAT32).

## Design (as implemented)

1. `src/kernel/fs/owner_system.c` binds the system volume from
   `supervisor_bootstrap` before the account service, registry, logon roles or
   desktop start (normal boots; not Setup mode):
   - `machine.sid` present: open the existing store (owner_open, recovery and
     record verification included), bind the OWNER service and
     `owner_security_bind`, then `policy_init`/`policy_bind_locked` for C: with
     `policy_volume.lease` = the OWNER backend;
   - no SID and no store: UNPROVISIONED, the volume stays unmanaged (k03's
     interim rule);
   - a SID with no committed store while `bootstrap.state` is still a valid
     UNPROVISIONED record: a first account batch cut before OWNER's first
     durable record; the volume stays UNPROVISIONED and the SID is reused;
   - a SID without store otherwise, or an unreadable/contradictory store:
     FAILED. Every OWNER request then answers EIO, so the account service stays
     closed and answers its clients with EIO at once.
2. The first RESERVE in UNPROVISIONED state passes a **fresh provisioning
   gate** that reads the destination's own evidence: valid UNPROVISIONED
   `bootstrap.state` for this volume (no `.new`/`.bak`/`.txn`), `machine.sid`
   and ACCOUNT's `bootstrap.sid` naming the request's SID, volume, origin and
   seed digest, `provision.seed` matching that digest for origins 1/2, no
   `accounts.db*`, no OWNER store. Then the store is created, bound, and the
   policy/lease bound; the RESERVE continues normally. STATUS/ABORT of an
   unknown key before any store answer UNKNOWN.
3. The ACCOUNT evidence callbacks of `owner_vfs_backend` read, never write:
   `accounts.db` (generation + whole-file SHA-256 after its own checksum),
   `acct.commit`/`acct.abort` markers with the `accounts.db.txn` intent of the
   exact request digest (COMMIT_REQUIRED = 1; an older finished batch's marker
   is ignored; a newer or mismatched one is contradictory), the
   `owner.checkpoint` blob (exact CHECKPOINT header, watermarks), the
   `enrollment.catalogue` map.
4. OWNER query operations 8..14 (`fs/owner_system.h`): IDENTITY, GENERATION,
   BINDING, ROOT_OCCUPIED, SNAPSHOT (image through the SYSTEM-only
   `/System/Security/Owner/snapshot`), RECOVER_ORPHANS (`owner_recover`: finish
   from ACCOUNT's durable intent), CHECKPOINT_FLOOR. `channel.c` allows ops up
   to 14 on service 3 and wakes PID 1 when an OWNER request is complete;
   `security.c` admits the new op numbers for the same roles.
5. `apps/accounts/owner_kernel.c` replaces `owner_interim.c` in the image:
   transport, queries, staging/tree digest/validate (canonical stream equal to
   the kernel's), snapshot read-back, prune of finished empty staging
   generations. `main.c` takes the volume from IDENTITY (placeholder identity,
   closed service, if OWNER cannot name it).
6. First boot: sec-user-02's `acct_firstboot_promote` now completes: entropy
   SID, three CREATE bindings (500, 501, 1000) staged and published by OWNER,
   `accounts.db` generation 1, `bootstrap.state` CONSUMED, seed removed.
7. DOS: `dos_identity_bind` uses k03's `vfs_table_bind_subject` (kernel always
   links it; standalone DOS harnesses without credentials keep kernel tables).
   The interim bootstrap session carries uid 1000's committed profile when the
   store is open at desktop start (boots after the first), so k05's per-title
   stores live in `<profile>\Settings\DOS`. After every open with published
   accounts, the service moves files of the old machine store
   `/System/DOS/SAVES` into the first administrator's `Settings/DOS`
   (restartable; an existing destination or an open file stays put).
8. Policy readers no longer treat `owner_store.busy` as unavailable metadata
   (label computation, `policy_child`, `policy_check`, `owner_label`): a batch
   or creator-root transaction in flight no longer turns every public file
   access into EIO. In-flight names stay fenced by `pending`/root intents; the
   kernel is non-preemptible, so the in-memory map changes only between yields.
9. ACCOUNT accepts CIUKIOCP v2 checkpoint images (creator-root records), which
   OWNER emits once a Standard user owns a root entry.

## Decisions for the lead

- **FAILED store leaves the volume unmanaged** (not bound without owners): a
  bound policy without owner records would deny every public root entry
  (`/Games`, `/tmp`, ...) and break the interim desktop; the account service is
  closed either way. Log: `[owner] store unavailable ... volume stays unmanaged`.
- **Contract consequences now active on the administrator desktop** (rule
  table, K-Q4): `/System/Logs` is SYSTEM-only (Control Panel Logs tab, Log
  Viewer and hwreport cannot read BOOT.LOG; NetSurf and native crash logs are
  not written there); `/System/BOOT.CFG` and `/System/Drivers` are sealed
  (Control Panel boot options and driver-pack install/uninstall are refused);
  `/System/LOGS/SFC.LOG` writes are refused. The contract routes these through
  SYSTEM services that do not exist yet; nothing was relaxed here.
- **Win32 per-user mode becomes live**: k06's w32srv asks ACCOUNTS for the
  PROFILE at start and, once accounts are published, runs in session mode
  (`<profile>/Settings/Win32`, machine hives through the registry broker). On
  the integrated base without publication w32srv cannot start at all; with it,
  per-user shell folders (CSIDL Start Menu, Personal) point into the profile
  while the interim desktop still shows `/Windows/Start Menu` and `/Documents`
  (sec-user-03/05 scope).
- **G32 upgrades**: with a working OWNER, sec-user-02's G32 preparation moves
  `users.dat` into protected evidence on the first boot; enrollment (UI in
  sec-user-03) is then required, so the interim desktop shows no G32 logon.
- **First boot cut after OWNER RESERVE (found by H15, needs a decision)**:
  if power is lost after OWNER's durable RESERVE and before ACCOUNT's intent
  write, the next boot opens the store and OWNER aborts the orphan from
  ACCOUNT's evidence, as contract 3.5 requires. That abort retires G and the
  reserved uids (watermarks never decrease), so the ready/Setup recipe, which
  needs uid 1000, can never be applied: the installation stays closed and
  needs reinstallation or the Setup repair. sec-user-02's resume block
  (`store.c`) assumed it could continue at the next generation; its H08
  fixture's fake OWNER does not enforce the uid watermark. Options: an OWNER
  bootstrap exception (a uid whose every reservation was aborted before
  PREPARE, while no batch was ever committed and no profile exists, may be
  created again) or accepting fail-closed. Not changed here; the window is one
  durable write on the first boot.
- **Strict FAT names**: binding sets `strict_names` on C: (k02): a directory
  with an orphan or malformed LFN entry fails with EILSEQ instead of the
  alias fallback. Fresh images have none; ScanDisk repairs old ones.
