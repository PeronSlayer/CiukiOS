# SYSTEM account service: host implementation decisions

Scope: directive [sec-user-01](../directives/sec-user-01.md). Runtime sealing,
kernel/owner binding, image integration and hardware durability remain lead work.

## Review round 3 research and implementation rules

The amendment-3 review was checked against the frozen challenge/OWNER protocols
and [kernel retained-subject contract](../design/user-security-kernel.md).
Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
bind security context to execution, not to the numeric value of a reference
handle. Use a private `same_link` kernel adapter that checks both retained
subjects' live process/link/revocation generations; independently acquired
handles may differ. LOGON attempt IDs combine its registered role generation
with a bounded counter, so a respawn cannot reuse ACCOUNT's consumed IDs.
Enrollment keeps the exact original challenge but checks ENROLL_COMMITTED
against its resulting generation; its credential/profile/name remain zero.

The upstream [fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html)
requires separate namespace durability. Journal the exact pending checkpoint
request and flushed snapshot before sending OWNER CHECKPOINT; recovery replays
that pair, publishes ACCOUNT's marker, completes COMPACT and durably clears the
pending journal before admitting changes. CREATE queries OWNER for occupied
roots, including retained imported/retired roots and reservations, and durably
skips conflicting IDs. Its pending client receipt must track the actual planned
commit generation after skips. OWNER still resolves and reserves destinations.

Microsoft [cancellation considerations](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)
do not establish rollback after cancellation. A storage error closes all queued
verification and clears outstanding attempts until store recovery completes;
it cannot authorize an old in-memory verifier. Bound a local intent to
`64 + ACCT_FILE_MAX` before copying its payload. [RFC 8018 section 5.2](https://www.rfc-editor.org/rfc/rfc8018.html#section-5.2)
was rechecked; these fixes leave the KDF parameters unchanged. The supplied
read-only Resource Kit profile and local/network-logon ranges remain historical
context; its default-profile fallback does not override the closed gate.

## Review round 2 research and implementation rule

Research checked before changing code, after `date` reported Mon Oct 26
13:15:28 CET 2026. Microsoft [cancellation considerations](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)
distinguish an attempted cancellation from completion. Consequently transport
cancellation never asserts storage rollback: caller-owned durable operation IDs
and STATUS resolve uncertain management results. Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
attach identity to execution; the retained LOGON role, not UI-supplied fields,
attests the relayed UI connection. Microsoft [desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)
separates trusted input/presentation; outcomes must retain both console and
authentication epochs, and a DESKTOP request can only enter the lock gate.
[RFC 8018 section 5.2](https://www.rfc-editor.org/rfc/rfc8018.html#section-5.2)
continues to govern the existing KDF; this amendment changes no KDF parameters.
The supplied read-only Resource Kit profile/logon ranges were checked again;
its default-profile fallback is superseded by this contract's closed gate.

Repository comparison: the round-1 core still authenticated direct UI peers,
used 240-byte success-only outcomes, allocated locators during ACCOUNT staging,
sent metadata changes to OWNER, and exposed a private atomic replacement. Round
2 replaces those interfaces with the frozen challenge/relay/outcome codecs,
OWNER-only reservations, ACCOUNT-only metadata commits and password-first
ordinary operations. Real kernel/owner storage qualification remains separate
from the production-linked host fakes.

Primary sources inspected before implementation:

- [RFC 8018 sections 4 and 5.2](https://www.rfc-editor.org/rfc/rfc8018.html#section-5.2):
  use an explicit salt, iteration count and HMAC-SHA-256 PRF. Reuse the repository
  SHA-256 implementation; retain exactly 10,000 iterations and a 32-byte key.
- [Microsoft well-known SIDs](https://learn.microsoft.com/en-us/windows/win32/secauthz/well-known-sids):
  retain the directed 500/501 and 544/545/546 identities. Native IDs remain
  distinct from complete Windows SIDs; generate the machine components from entropy.
- Microsoft-authored XP Resource Kit chapters, hosted by a mirror:
  [Authentication Policy Options](https://flylib.com/books/en/2.922.1.130/1/) and
  [User Accounts and Security Groups](https://flylib.com/books/en/2.922.1.135/1/),
  plus the Microsoft Press [User Accounts lesson](https://flylib.com/books/en/2.732.1.17/1/).
  These confirm the historical local lockout threshold 0, separate built-in
  identities, and visibility distinct from enablement. CiukiOS's stricter password
  policy, fixed built-in names and normally disabled recovery account remain the
  lead's explicit choices, not claims of identical XP defaults.
- [Historical NIST SP 800-63B 5.1.1.2](https://pages.nist.gov/800-63-3/sp800-63b.html#memsecretver):
  eight characters and printable ASCII/space inform the approved policy. Its
  supersession notice and recommendation to accept at least 64 characters mean
  the directed 63-byte ceiling is a CiukiOS choice, not current NIST compliance.
- [Upstream Linux man-pages fsync](https://man7.org/linux/man-pages/man2/fsync.2.html):
  file flush alone does not guarantee directory-entry durability. Separate file
  flushes and namespace/volume barriers. The Open Group fsync and rename pages
  returned HTTP 403; the repository's documented FAT EEXIST behavior governs
  the private no-replacement rename adapter.
- Owner's Windows 98 Resource Kit, read-only `build/rskt/wrkc07_3.htm:10-53`
  and `wrkc18_2.htm:10-23,65-75`: profiles and local/network logon are the
  superseded G32 baseline. No Resource Kit text is copied into the implementation.

Repository comparison: G32 `apps/desktop/users.c` uses a v1 struct codec and
shared settings adapter; the new service uses an explicitly encoded v2 file,
retained kernel peers, a service lease, protected recovery files and private
owner transactions. G32 remains a read-only conversion source, with desktop
behavior unchanged. Host tests link the production entry points, substituting
only storage, entropy, memory, retained-peer and owner-service boundaries.

The implementation and binding contract are documented in
[apps/accounts/README.md](../../apps/accounts/README.md). Exact verification
commands and remaining integration items are in `build/codex/sec-user01-report.md`.

Host evidence: H03 links the production code with ASan/UBSan and completes its
ten assertion groups, including the independent full-file seed/PBKDF2 oracles,
16 concurrent clients, all 18 account slots, UID/generation exhaustion, all
bootstrap/creation allocation sites, partial ENOSPC writes, corrupt recovery
files and every observed persistence boundary (15 password, 21 bootstrap,
32 creation, 17 deletion, 41 cancellation). RUS completes its unchanged G32
model and I/O assertions. Both commands then exit 1 because LeakSanitizer's
ptrace-based leak scan is denied in the implementer's sandbox. Leak detection
was retained; these are not green host-suite results. The lead must rerun both
commands in an environment that permits the scan before integration.

An optimized host-object inspection of `acct_equal_secret` shows a complete
XOR/OR comparison with branches depending on the public length only, with no
memcmp replacement or mismatch early exit. This is compiler/host inspection,
not a target-hardware timing measurement. No runtime or disk-image evidence is
claimed.

## Review round 1

The amended main sections 3.1–3.6/4.1 and kernel A1-2/A1-3 now govern the account
core. They supersede the earlier single-binding transaction and provision.used
implementation described by the initial evidence above.

Primary research was rechecked before the code changes: [RFC 8018](https://www.rfc-editor.org/rfc/rfc8018.html#section-5.2)
retains explicit salt/count/PRF parameters; [Microsoft access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
bind identity/groups to execution; [well-known SIDs](https://learn.microsoft.com/en-us/windows/win32/secauthz/well-known-sids)
distinguish built-in account/group authorities. Microsoft-authored XP material,
[account policy](https://flylib.com/books/en/2.922.1.130/1/),
[accounts/groups](https://flylib.com/books/en/2.922.1.135/1/) and
[account visibility/enablement](https://flylib.com/books/en/2.732.1.17/1/), remains
historical context. The [historical NIST reference](https://pages.nist.gov/800-63-3/sp800-63b.html#memsecretver)
is superseded; the approved 63-byte maximum remains a project decision. The
[upstream fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html)
supports retaining separate data and namespace barriers. The supplied read-only
Resource Kit profile/local-logon ranges were re-read without copying book text.

Resulting decisions follow the reviewed CiukiOS contract rather than an assertion
of exact XP defaults: permanently retire the sole enabled blank exception; bind
consumed attempts to actual roles/endpoints/console/account generations; send
outcomes only to LOGON; reserve one G and one complete destination owner vector;
retain durable allocation watermarks independently of the database; cross
COMMIT_REQUIRED before owner COMMIT and never roll it back; admit authentication
only after CIUKIBST CONSUMED. The 96+256*N CIUKISEE and 160-byte bootstrap codecs
are shared entry points for user-02. All three bootstrap bindings and all eighteen
migration bindings are exercised through the production batch/store code.

H03 adds independent Python full-byte seed, bootstrap-state and owner request/
reply oracles, all receipt states and capacity, whole-vector cancellation/retry,
retirement across restart/reboot, endpoint framing/cancellation and consumed
attempt generations. H10 repeats the retirement/protocol assertions through the
same production core. The fault backend retains owner receipts across simulated
account-service restart and validates staged manifest identities. It is boundary
fault injection, not evidence of a real k01/k02 binding. Runtime endpoints stay
unpublished until real adapters are integrated.

Exact verification results, persistence-cut counts and the unchanged sanitizer
restriction are recorded in the Review round 1 section of the worktree report.

## Review round 2 implementation and host evidence

This section supersedes round 1's direct-UI verification, success-only outcomes,
caller-resolved locators, lifetime owner receipts, metadata KEEP transactions and
private atomic replacement. The amended implementation uses the full LOGON
challenge/relay/outcome exchange and retained selector, durable ENROLL_PLAN and
staged/final enrollment, LOGON-only one-shot autologon, and the same-session
DESKTOP lock adapter. The private LOGON core supplies correlation without an
alternative session creation or secure-leave path. Peer role generations and
connected-server generations remain separate.

ACCOUNTS RESERVE/STATUS uses permanent operation-id watermarks and bounded
caller-owned result checkpoints. Exact retries never repeat a KDF/CREATE;
EXPIRED never implies rollback. Metadata changes use ACCOUNT-only local intents,
including permanent bootstrap retirement. OWNER alone reserves root identities;
request locators are zero. Both sides validate the same CIUKISTG bytes and tree
hashes. Reserved ABORTED batches count toward the receipt window; coherent
snapshot/ACCOUNT marker/OWNER floor publication reclaims the log. The private
CIUKIOCP versioned snapshot is documented in apps/accounts/README.md for the
real k02 binding; it includes every allocation watermark and retained owner map.

Sequential nice-19 final commands:

- `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_accounts.py`
  runs 25 tests. All C assertion groups and independent Python full-byte/hash
  oracles complete; 25 shutdowns fail with the sandbox's LeakSanitizer ptrace
  diagnostic. Exit 1, not a green H03 gate.
- `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_logon.py`
  runs five tests. All assertion groups complete; five identical LeakSanitizer
  shutdown failures. Exit 1, not a green H10 gate.
- `nice -n 19 python3 -B -m unittest tests.host.desktop.test_users` runs one test.
  Unchanged G32 model/I/O assertions complete, then the same shutdown error.
  Exit 1, not a green RUS gate.

No C assertion, ASan memory-access or UBSan diagnostic remains. No sanitizer
setting or leak suppression was changed. New evidence includes 2055 successful
metadata updates with interleaved passwords and aborted results without OWNER
log growth, 2055 RESERVED-to-ABORTED CREATE batches with nonreused keys/locators,
and complete recovery at 15 OWNER checkpoint and 70 ACCOUNT result checkpoint
boundaries. Existing fault enumerations cover 125 ready bootstrap, 68 password,
94 CREATE, 51 cancellation and 116 eighteen-binding migration boundaries.
Unrecoverable incomplete bootstrap evidence stays closed; no partial vector or
acknowledged-password rollback is admitted.

Actual kernel/owner endpoints, resolver/tree fences, native durability, event
loops/UI confirmation, target KDF timing and runtime qualification remain the
lead/follow-on tasks. Live publication still refuses unbound/test adapters.
The exact commands, logs and integration checklist are recorded in
`build/codex/sec-user01-report.md`.


## Review round 3 verification

Continued and reviewed the existing amendment-3 edits against all seven findings.
H03 runs all seven production-linked regressions: faulted queued authentication,
checkpoint replay before later mutations, imported/retired root skipping with
caller STATUS recovery, final enrollment correlation, LOGON respawn attempts,
distinct references to the same kernel link, and oversized local-intent payloads.
H10 repeats the four authentication/LOGON regressions.

Final commands ran serially, with leak detection unchanged:

| Exact command | Observed result |
| --- | --- |
| `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_accounts.py` | Exit 1: all 31 assertion groups and independent Python oracles complete, then 31 LeakSanitizer ptrace-denial shutdown failures. |
| `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_logon.py` | Exit 1: all nine assertion groups complete, then nine identical LeakSanitizer failures. |
| `nice -n 19 python3 -B -m unittest tests.host.desktop.test_users` | Exit 1: unchanged G32 model/I/O assertions complete, then one identical LeakSanitizer failure. |
| `git diff --check` | Exit 0. |

No C assertion, ASan memory-access or UBSan failure was observed. These are not
green host gates: the lead must rerun them where leak scanning is permitted.
The new OWNER checkpoint journal is exercised at 24 write/flush/namespace/OWNER
boundaries, with a password change before retrying compaction and a later CREATE.
The imported-root CREATE regression covers 94 boundaries with restart, STATUS and
nonreuse checks, plus retained retired-root occupancy. Existing 70-boundary
ACCOUNT result/local-intent recovery and other persistence suites remain covered.

The explicit `same_link` private adapter is the chosen resolution for finding 6.
Real kernel/OWNER binding must supply it and the private `root_occupied` query;
missing bindings refuse open. No public ABI or new OWNER wire operation was added.
Detailed per-finding changes, primary-source links, exact redirected commands and
logs are in `build/codex/sec-user01-report.md`, section “Review round 3”.
