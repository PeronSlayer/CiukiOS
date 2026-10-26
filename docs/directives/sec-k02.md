# sec-k02: managed-volume path policy and owner service

- **Written:** 2026-10-26 15:46 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: wt/sec-k01 274bb7517 (sec-k01 complete: credentials, call 101 operations 1-14, roles/endpoints, retained subjects, same_link; it merges into main after today's T23 image).
- **Specification:** `docs/design/user-security-kernel.md` (filesystem path policy: identity and evaluation, rule table, owner metadata and consistency, enforcement points; bounds) and `docs/design/user-security-contract.md` "Implementation plan" row 2A (sec-k02) with H02/RFS, sections 3.5 (batch account/owner transactions: OWNER resolves and reserves CREATE roots itself, PREPARE/COMMIT/ABORT/STATUS, receipts, checkpoint/compaction), 6.1 (permissions matrix) and 6.3 (G32 conversion). Lead decisions: `docs/directives/sec-lead-decisions.md` (root creator-owner rule with up to 256 creator-owned root entries per volume, 32 rules, 32 profiles; additional fixed volumes are data volumes; u01-D2: the OWNER service answers root_occupied for imported, retired and reserved roots).
- The account service (sec-user-01, branch wt/sec-user01, apps/accounts) already talks to OWNER through a private adapter and a fault-injecting backend; build the real OWNER side so that adapter can be bound to it (the lead integrates). Do not edit apps/accounts.

## Scope (row 2A; the contracts are authoritative)
The shared explicit-subject VFS policy API, mount binding, the bounded policy trie, resolved-object labels, owner provisioning and recovery, denied-by-default for missing subjects or owner metadata, the OWNER service with its batch transaction protocol (PREPARE/COMMIT/ABORT/STATUS, one generation per batch, non-reuse after abort, lifetime receipts with the checkpoint/compaction protocol, destination staging, COMMIT_REQUIRED recovery at every write/flush), root_occupied, the creator-owner side records for the volume root, the posixpath adapter. Leave the broad syscall enforcement to sec-k03 (do not change every file syscall here); provide the API it will call and the tests that prove the decisions.

## Tests
H02 and RFS as the contract defines them: every rule with SYSTEM/administrator/standard/other user/Guest subjects; LFN and 8.3 aliases, ASCII and non-ASCII case, /mnt aliases, relative cwd, dot and dot-dot, prefix siblings, Win32 trailing dots/spaces, malformed UTF-8/LFN; missing policy/owner map; injected allocation and I/O failure; power cut at every write/flush of owner transactions (three-account bootstrap, sixteen-profile migration, creator-owned root entries up to the 256 bound); bounded warm lookup cost. Existing filesystem suites (scripts/test/host_fs_tests.sh) and kernel suites stay green. Run one at a time with nice -n 19.

Report to build/codex/sec-k02-report.md. Leave changes uncommitted.

## Lead amendment 1 (2026-10-26 21:46): review round 1

Committed as a73186023. A Codex xhigh review found two P1 and four P2, confirmed with probes (full text build/codex/sec-k02-review1.md); fix all with regressions:
1. P1 registry backing files: security_policy.c:71 protects /Windows/USER.CRG but storage uses USER.CRG.0/1 and SYSTEM.CRG.0/1 (apps/w32srv/storage.c:115); protect every actual hive slot and legacy migration input as SYSTEM-only (per-profile hive slots too, as the contract assigns them).
2. P1 owner identity broken by permitted mutations: owner_vfs.c:183 hashes the first cluster and creation time into the object proof; truncation and metadata changes alter them, then proof checks return EIO and mount verification can close the owner store. Use an identity that survives permitted mutations (as the contract specifies for owner records); never close the store on a stale proof of an object the policy allowed to change.
3. P2 DOS/Win32 paths escape their selected volume (posixpath.c:633): walk from the selected mount root, clamp .. there, exclude native synthetic mount transitions.
4. P2 /Documents/Desktop descendants classified POLICY_PROGRAM (security_policy.c:75): keep the shared legacy tree shared (contract 6.1) and protect only the designated launch anchors.
5. P2 absolute paths require access to cwd (security_path.c:18): resolve/check cwd only for drive-relative input.
6. P2 conversion drops terminal separators (posixpath.c:637): preserve the trailing-directory requirement (ENOTDIR for D:\file\).
Report as "Review round 1". Leave changes uncommitted.
