# SYSTEM account service core

This private service implements sec-user-01 through Lead amendment 3. It installs
no runtime endpoint and leaves G32 desktop logon unchanged. Link `crypto.c`,
`codec.c`, `owner.c`, `bootstrap.c`, `protocol.c`, `store.c`, `service.c`,
`migration.c`, `persist.c`, `results.c`, `auth.c`, `enrollment.c`, `manifest.c`,
`checkpoint.c`, `logon.c`, repository SHA-256 and G32's read-only decoder.
No source is vendored or included through a `.c` include.

`acct_service_open(..., host_test=0)` requires real sealed kernel, OWNER and
protected storage adapters. Only sealed boot code supplies these tables; host
tests explicitly select test mode. No fake endpoint is installed at runtime.

`adapter.h` follows the frozen call-101 operations 8..14 and 64-byte link record.
Retained peer `role_generation` identifies the remote registered role instance;
it is distinct from the connected server's `service_generation`. ACCOUNT trusts
the retained LOGON role to relay the actual UI->LOGON endpoint generation. It
never substitutes the LOGON->ACCOUNT generation. Kernel attestation supplies
roles, credentials, Safe Mode, active sessions, epochs and service generations;
request uid/name/environment fields grant no authority. `subject_get` retains a
link subject across exchanges; `peer_alive` must check its original process/link
revocation generation. Registration and all authority fds remain nontransferable.
The required private `same_link(ctx, a, b)` adapter returns 1 only when both
retained subjects denote the same live link and process/revocation generations,
0 for different links and negative errno on failure. It must consult the kernel
retained-subject identity; comparing opaque reference handles is insufficient.
Both ACCOUNT verification and LOGON submission use this check, including when
separate acquisitions produce different handles. Missing bindings refuse open.

The private codecs are little-endian, with canonical padding and no attachments:

- BEGIN_AUTH 64; challenge 64; CHALLENGE relay 128; UI submission 208;
  verification relay 272; correlated AUTH_OUTCOME 304; public progress 16 bytes.
- Management 312, including client_operation_id at 304; filtered public records
  144; successful mutation generation 8; RESERVE request/reply 48/16; STATUS
  request/reply 32/80; authorized PROFILE 272 bytes.
- OWNER batch request 160+128*N and reply 96+32*N; checkpoint requests are 160
  bytes with zero bindings. Every binding's request locator is zero.

AUTH LIST permits LOGON_UI or LOGON. All challenge, verification, enrollment and
AUTOLOGON operations require LOGON. ACCOUNT retains each resolved selector,
challenge, snapshot and original LOGON subject, consumes it once and reports
success or failure to LOGON with the complete challenge. Errors carry zero
identity/profile/name. ENROLL_STAGED/COMMITTED also carry zero identity: a fresh
LOGON challenge is required. Cancellation, timeout, changed epochs, target,
account/service generations or a different connection cannot authorize a result.
`auth_binding` must validate LOGON's cross-connection UI attestation against its
retained attempt, not infer it from ACCOUNT's endpoint. `auth_outcome` sends on
the private ACCOUNT->LOGON connection and waits for acknowledgment while the
receiver continues draining outcomes independently of verification replies.

`logon.c` supplies the private coordinator's selector/challenge/submission/outcome
correlation and one-shot autologon latch. Its outcome generation argument must
come from trusted current ACCOUNT state, never a UI assertion. The later LOGON
event loop binds transport, invalidation on cancellation/disconnect, secure
attention, and actual session creation/launch/unlock. SESSION_LOCK accepts only
a retained same-session DESKTOP and calls the current trusted lock backend; it
cannot release a gate or expose its cookie. Blank-owner guidance belongs to the
trusted UI. ACCOUNT has no session creation, process launch or SECURE_LEAVE hook.
LOGON allocates attempts as `(registered_logon_generation << 32) | counter`,
with a nonzero 32-bit counter that refuses to wrap. Each respawn must use its
new kernel-registered role generation and keep this coordinator for its lifetime.
ENROLL_COMMITTED retains its original challenge correlation but validates the
trusted current ACCOUNT generation against `resulting_accounts_generation`.
Its zero-credential/profile/name rule still requires a fresh LOGON challenge.

Both ACCOUNT and LOGON consume AUTOLOGON eligibility before issuing a challenge.
It requires the exact retained ready tuple, CONSUMED bootstrap, normal boot and
both singleton role generations=1. The coordinator clears eligibility on Log
Off/Lock; either role restart suppresses it for that boot. Persistent retirement
is monotonic. Ordinary password-first operations replace blank CiukiUser: set
its password, enable another configured administrator, change active session,
then disable/demote it. There is no atomic replacement operation or wire codec.
Last-admin and active-account checks apply after each operation.

`results.c` issues durable monotonic client IDs and records caller uid, operation
and the SHA-256 of operation||management-with-zero-id. Client recovery state
persists the id/digest/non-secret selectors before sending; it never persists
plaintext secrets. PENDING precedes mutation effects. Exact retries return their
retained result before stale-generation checks and never repeat a KDF or CREATE.
Same-uid reconnect STATUS is permitted; admin membership does not expose another
uid's receipt. IDs above the watermark return ESRCH; discarded IDs return EXPIRED
without owner/result information, and old mutations return EAGAIN. EXPIRED never
means rollback. Before COMMIT_REQUIRED a revoked request can abort; afterward
explicit SYSTEM recovery finishes it and suppresses delivery to the dead link.

Result checkpoints retain at most 1024 receipts, compact proactively at 768 and
keep 256 terminal results when possible. An independent two-slot operation-id
watermark prevents reuse after a lost reservation reply. Full snapshots include
database hash/generation and allocation watermarks; candidate/previous files are
validated before forward recovery. PENDING recovery compares the unique planned
G with the recovered coherent database; no new caller authority is inferred.
One unsubmitted reservation per uid may be replaced; a pending writer blocks it.
No plaintext secret is stored in the result journal.

Ownership-neutral passwords, pictures, labels, roles and enablement use a full
ACCOUNT-only local intent and verify existing committed bindings. They allocate
no OWNER receipt. That intent includes the complete target database and retired
bootstrap flag, so interruption cannot restore an acknowledged password or blank
exception. CREATE/RETIRE/mapping changes use one full OWNER batch, including the
three bootstrap bindings or all sixteen G32 profiles plus both built-ins.
Before allocating a CREATE uid, the required OWNER `root_occupied` adapter
checks the candidate numeric root on this volume, including retained imported
and retired mappings and live reservations (1 occupied, 0 free, negative errno).
Conflicting uids are durably consumed through the allocation watermarks and
allocation retries the next uid. The pending client receipt is durably replanned
to the actual commit generation after skips. OWNER still resolves/reserves the
destination; this read-only query never supplies a key, locator or other identity.
Storage faults close queued authentication and invalidate all outstanding
attempts; close/open recovery is required before verification resumes.

ACCOUNT stages exact CIUKISTG bytes at the deterministic destination path,
validates its target uid/legacy map and independently checks canonical tree
hashes. OWNER independently reads/hashes those same bytes and resolves its own
objects. `stage`/`tree_digest` are protected destination-I/O seams; they never
return locators. RESERVE alone persists nonreused locators/keys/owner generation,
fences the objects/ancestors/aliases and manifest/tree, and reserves one receipt
slot including an early ABORT. PREPARE requires RESERVED. UNKNOWN ABORT allocates
nothing. COMMIT_REQUIRED is durable before OWNER COMMIT. Retired data and roots
remain protected and consume permanent profile capacity.

`checkpoint.c` checks OWNER's versioned CIUKIOCP snapshot codec: 160-byte header,
112-byte sorted full-map records (including retired roots), and length-prefixed
canonical request/reply pairs for the latest 256 terminal receipts. Header fields
are version/header/counts at 8/12/16/20, SID at 24, volume/floor/accounts G at
40/48/56, database digest at 64, committed/reserved owner generations at 96/104,
permanent key/locator watermarks at 112/120, and ACCOUNT G/uid watermarks at
128/136 (zero padding through 160). ACCOUNT verifies maps, results,
generations and hashes before publishing its protected marker. The exact
192-byte pending request/watermark record is first journaled as
`owner.checkpoint.pending` after the snapshot is flushed and before sending
OWNER CHECKPOINT. Recovery replays that exact request, durably publishes the
matching `owner.checkpoint` marker, completes COMPACT, and durably removes the
pending journal before admitting account changes. OWNER CHECKPOINT
retains the previous store; COMPACT requires that durable marker before installing
its floor and trimming prefixes. Recovery completes the same pair, including an
interruption after floor publication but before trim. G<=floor cannot execute,
even with changed request bytes; STATUS returns CHECKPOINTED. The real k02 adapter
must share/bind this private snapshot codec; no kernel owner store is delivered
here. `recover_orphans` aborts reserved batches lacking an ACCOUNT intent.

`enrollment.c` durably stages a protected source, confirmed plan, old-id/new-uid
map, provisional catalogue and selected verifiers. Changed plans retire prior
uids/attempts and reserve a fresh G. Imported old secrets, including short ones,
are verified only here; blank/fresh records and recovery 500 require an empty old
secret plus the confirmed gate. Every replacement must satisfy 8..63 bytes.
Only the complete batch enables accounts. `enrollment_authorized` requires the
confirmed protected workflow and no conflicting ordinary session. user-02 owns
actual source protection/removal, destination relocation and UI confirmation.

`protocol.c` accepts contiguous CSC1 fragments of at most 192 bytes, 8192 per
logical body, one exchange/link and eight globally. The channel boundary rejects
attachments. `exchange_begin` spends request ids at first server admission;
CANCEL=14 returns 1 only with proven pre-dequeue queue removal, otherwise 0
(irrevocable-or-unknown). Neither result undoes a prior same-id operation. Reply
delivery rechecks the original link/request and keeps its subject until final
drain; stale replies cannot reach a reused fd/session. All temporary secrets,
worker buffers and allocated snapshots are wiped before release.

ACCOUNT accepts no client environment/stdin/settings/fds. LOGON/k01 generates
LC_ALL=C, validated committed TZ, CIUKI_PROFILE_SETTINGS and purpose-specific
launch/descriptor labels. PROFILE uses the actual committed owner root, adds
`/Documents` for home and returns `/Programs/terminal`; it never infers a root
from uid. Ordinary/Guest callers get their own mapping, administrators and
retained LOGON/ACCOUNT roles may query others. The main contract governs policy.

H03/H10 link production code with faulting boundary adapters, independent Python
wire/PBKDF2/hash oracles, >2048-result/owner-window streams and persistence-cut
fixtures. RUS retains the unchanged G32 regression. Exact results, research and
remaining integration work are in `build/codex/sec-user01-report.md`.
