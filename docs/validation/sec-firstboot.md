# First-boot ACCOUNT transient recovery

Directive: `docs/directives/sec-firstboot.md`. Recovery authority:
`docs/design/user-security-contract.md` sections 3.5 (including first-boot uid
recovery) and 3.6; entropy authority: `docs/design/kernel-entropy.md`.

## Research and repository comparison

- [Linux v6.12 random.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/char/random.c),
  `getrandom`: an unready generator with `GRND_NONBLOCK` returns `EAGAIN`.
  Decision: retain Ciuki's nonblocking syscall; ACCOUNT waits between attempts
  in its serving loop, without substituting weak random bytes or imposing a
  first-boot deadline.
- [OpenBSD getentropy(2)](https://man.openbsd.org/getentropy.2) documents the
  256-byte ceiling and high-quality entropy. Ciuki's startup `EAGAIN` is the
  explicit local contract, not a claimed OpenBSD error convention. Both
  promotion calls (12-byte SID and 16-byte ready-account salt) remain direct
  kernel entropy requests. The SDK's separately bounded arc4random policy is
  inappropriate for this startup wait.
- Microsoft [SID Components](https://learn.microsoft.com/en-us/windows/win32/secauthz/sid-components)
  describes issuer-relative uniqueness and one-time RID assignment.
  Decision: use the existing protected `bootstrap.sid`/`machine.sid` evidence
  and account allocation watermarks. Retrying never generates another durable
  installation identity, decreases a watermark or reuses a retired generation.
- [RFC 8018 sections 4 and 5.2](https://www.rfc-editor.org/rfc/rfc8018.html)
  distinguishes salt and iteration parameters. The ready-account verifier
  retains the existing fresh salt, PBKDF2 implementation and 10,000 iterations.
  Once a full account intent is sealed, recovery uses its exact verifier bytes.
- The owner's read-only Windows 98 Resource Kit, chapter 7, "How User
  Profiles Work" (`build/rskt/wrkc07_3.htm` in the main checkout), describes
  per-user directories under Windows/Profiles and profile initialization at
  logon. This is historical profile context; it does not specify Ciuki's
  security transaction or prove the directive's XP first-boot comparison.
  The exact resume authority remains the local security contract, not an
  inferred Windows implementation.

`owner_vfs_tree_digest` checks the volume mutation sequence and mount generation
after walking a tree. Concurrent unrelated writes can therefore refuse a valid
tree with `EAGAIN`. Previously OWNER's PREPARE/COMMIT revalidation latched
`ready=false` on that refusal. Subsequent STATUS/generation/orphan requests then
returned `EIO` even though the same kernel OWNER was still attached. The orphan
recovery path also latched this transient validation failure. A service reopen
alone cannot clear that kernel latch.

The lead's failing serial capture shows initial entropy refusal, readiness at
13466 ms, fresh OWNER opening and then ACCOUNT open `EIO`. The old diagnostic
suppression does not identify the second refusal. Host injection proves the
post-RESERVE tree-refusal route; the precise occurrence in that captured boot
remains an inference until the lead repeats boots with the new diagnostics.

## Recovery decision

Round 2 review (2026-10-28; primary sources above rechecked before editing):
the local 3.5/3.6 contract also applies while an intent write is refused and
while OWNER is recovering at mount. Keep the exact sealed in-memory account
intent on persistence `EAGAIN`; retry it before orphan recovery so no extra
reservation, salt or generation is allocated. A real process restart without
that memory still follows the durable-evidence orphan rule below.

OWNER publication may return retryable `EAGAIN` from either read-only proof
lookup around a successful rename. Earlier completed renames remain fenced;
the identical COMMIT revalidates their proofs and completes the same vector.
Rename errors and all durability errors keep admission closed. Mount recovery retains a loaded,
ready store on reversible validation `EAGAIN`, retries on ACCOUNT operations,
and withholds policy/service binding until recovery and record verification
complete. IDENTITY continues to name the volume while recovery waits.
Non-transient validation errors and uncertain persistence remain closed.
This preserves the SID/RID uniqueness described by Microsoft and the exact
salt/verifier output described by RFC 8018; no entropy or KDF policy changes.
The v6.12 raw-source fetch was unavailable during this recheck; upstream's
[current getrandom implementation](https://github.com/torvalds/linux/blob/master/drivers/char/random.c)
and the primary [Linux getrandom(2) manual](https://man7.org/linux/man-pages/man2/getrandom.2.html)
confirm the same nonblocking readiness refusal.

ACCOUNT retains its unpublished service and lease after promotion `EAGAIN`.
Each retry runs the same recovery routine used by service open. The server
admits clients only after recovery and seed promotion complete. OWNER remains
attached throughout these retries.

| Evidence at retry | Action |
| --- | --- |
| No SID write | Retry entropy under the same lease. |
| SID intent, no account intent | Reuse SID; recover any orphan; bootstrap at the next available account generation. |
| OWNER RESERVE, sealed account intent retained in memory | Persist the identical intent before recovery can consider the reservation orphaned. |
| OWNER RESERVE, account intent absent and no retained sealed intent | Validate and abort the orphan; preserve OWNER/account/key allocation highs; use the contract's prepublication uid exception. |
| Full immutable account intent | Resume that exact batch, SID, account generation, digests and verifier through STATUS/PREPARE/COMMIT. |
| CONSUMED and installed database | Open results/enrollment state; remove a residual seed best effort; report successful promotion. |

OWNER preserves readiness only for `EAGAIN` during reversible namespace/tree
revalidation. Corrupt evidence and uncertain persistence/publication errors keep
their existing fail-closed behavior. Existing CHECKPOINT/COMPACT recovery runs
when opening a published store; no new candidate or checkpoint is invented for
an unpublished first batch. Generation/candidate guards remain enforced.
Specifically, `acct_store_commit(old=NULL)` skips checkpoint creation, and a
fresh OWNER snapshot has no terminal floor and refuses image creation. An
active checkpoint candidate belongs to published-store recovery, which uses
the retained exact pending CHECKPOINT/COMPACT request.

ACCOUNT logs each distinct `(step, errno)` refusal once per server instance and
prints the transient retry count on promotion. The count includes transient
open/recovery refusals as well as promotion attempts.

Committed-binding validation passes through `EAGAIN` only. A missing binding
remains `EIO`; returning `ENOENT` there would collide with service open's
unprovisioned-store sentinel. The H15 binding case injects both a transient
committed-binding query refusal during promotion and a missing binding on
reopen, checks the latter keeps the service closed, then restores the query
and reopens against the same attached OWNER.

## Host evidence

`tests/host/security/test_system_owner.py` links the production ACCOUNT server,
adapters and kernel OWNER on the existing real FAT/VFS harness. Backend-seam
fault injection covers both entropy call sites, all six RESERVE tree checks
(three roots, two passes), all three PREPARE and COMMIT tree checks, a real
account-generation guard refusal, committed-binding refusal, repeated refusals,
and in-session orphan recovery that itself encounters repeated tree refusals.
Every case checks one
SID, unchanged sealed request where present, retired generations, all profiles,
seed consumption and clean simulated reboot. `fsck.fat -n` checks each result.

Round 2 extends this to 38 first-boot scenarios plus existing H15. Publication
faults enter the real `entry_proof`/`vfs_stat` lookup; repeated read refusals
retain the same COMMIT digest and pending vector at all six proof lookups
(before/after each profile rename), including after partial publication.
Rename, publication barrier and permanent lookup errors explicitly check closed
admission. Intent persistence is refused 776 times (beyond `OWNER_COMPACT_AT`)
without allocating a second receipt/generation; a partial-write case verifies
identical sealed bytes on eight retries. Teardown wipes retained work, and a
following reboot retires its orphan generation.

The backend hook now installs before `owner_open`. Reboot tests cover both an
orphan RESERVE and a COMMIT_REQUIRED batch, with five recovery tree refusals;
the loaded receipt, pending records, sequence and fences remain intact, while
policy and ACCOUNT admission remain withheld. A recovery durability `EAGAIN`
still fails closed. Allocation-fault assertions cover every OWNER operation
from both promote/recover diagnostic contexts and check the step before the
allocator refuses and after `ENOMEM` returns. All scenarios reach their PASS
markers and zero tracked resources; sanitizer exit restrictions are recorded
separately in the report.

Sanitizer settings are unchanged; ACCOUNT/OWNER retains default leak detection.
Exact commands, results and sandbox-only LeakSanitizer failures are recorded in
`build/codex/sec-firstboot-report.md`. Repeated `qemu-t23` first boots are the
lead's remaining integration evidence; no guest or OS image build is run by
this worktree task.
