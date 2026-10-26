# SYSTEM account service core

This private service implements sec-user-01 and its first review amendment. It
installs no runtime endpoint and leaves G32 desktop logon unchanged. Build the
core from `crypto.c`, `codec.c`, `owner.c`, `bootstrap.c`, `protocol.c`, `store.c`,
`service.c`, `migration.c`, repository SHA-256 and G32's read-only v1 decoder.
No implementation is vendored or included through a `.c` include.

`acct_service_open(..., host_test=0)` requires real sealed kernel, OWNER and
protected destination-storage adapters. Host tests explicitly select test mode.
Only sealed boot code supplies adapter tables. These are integration seams, not
client flags or an alternative kernel/public SDK ABI.

`adapter.h` binds the frozen call-101 role, link and retained-subject model.
`service_link` maps operations 12 CONNECT, 13 ACCEPT and 14 CANCEL to the exact
64-byte private link record. ACCOUNT has no session-creation/launch/unlock hook;
operations 8–11 remain registered LOGON authority. Identity, role, actual Safe
Mode, endpoint/service generations, active accounts and the exclusive live lease
come from the kernel. uid 0 alone never authorizes account management or AUTH.
Public spawns, copied names and environment strings cannot acquire these roles.

`protocol.c` supplies the 208-byte AUTH and 304-byte ACCOUNTS body codecs,
144-byte filtered public records, 8-byte mutation replies, 240-byte AUTH_OUTCOME
validation, 32-byte LOGON event/challenge codecs and bounded CSC1 framing. Unknown
operations, flags, fields, lengths, padding and unselected UPDATE data fail before
dispatch. SELF_PASSWORD cannot become an administrator reset of a different uid;
reset uses UPDATE and only targets another inactive account. No reply exposes a
salt, verifier, private path or reusable credential to the UI.

The channel adapter must reject all attachments before `acct_frame`. It accepts
only the six kernel role-bound service endpoints; AUTH accepts LOGON_UI only.
One exchange per actual link and eight globally include reassembly, queued work
and queued replies. `exchange_begin` admits/spends request ids in kernel link
state; cancellation and reconnect cannot reset them on the same link. Fragments
are contiguous, at most 192 bytes, with an 8192-byte total and 5000 ms admission
expiry. A transport is driven serially by its receiver. Cancel calls operation
14 rather than interpreting caller-provided CANCEL bytes. `deliver_reply` checks
and retains the original subject/link/request through final queue drain;
`exchange_end` drops the worker reference, not queued reply ownership. Close,
revocation, timeout and all error exits wipe buffers and release charges.

`claim_attempt` atomically consumes a LOGON-issued attempt, rechecking actual UI
endpoint, action, session, console and accounts generations. It binds the resolved
account uid at consumption; UNLOCK already names the locked owner. ACCOUNT sends
successful credential/profile/name outcomes only on its own retained connection
to registered LOGON. Failed outcomes carry status with no body. LOGON revalidates
and consumes them once before its own session/launch/console transitions. Fresh
ready autologon is LOGON-initiated; `acct_autologon_eligible` checks the complete
consumed-state tuple and Safe Mode/Log Off/Lock exclusions. No list response is a
challenge. The private direct request API is for sealed internal workers/tests;
production channel ingress always uses the frozen body decoder.

ACCOUNT never launches a process or copies SYSTEM environment, settings, stdin
or fds into a user session. At integration LOGON/k01 alone generates the allowlist:
LC_ALL=C, validated committed TZ, generated CIUKI_PROFILE_SETTINGS and the
purpose-specific session/launch/descriptor labels. AUTH accepts none of these as
input. The outcome contains only the frozen credential/profile/name records.

The v2 store uses explicit little-endian 96-byte headers and 256-byte sorted
records. `auto_logon_retired` is monotonic in both accounts and bootstrap state.
Blank CiukiUser must be the sole enabled interactive account; enabling Guest,
500 or an ordinary user while it remains blank fails before persistent writes.
Blank rename/demotion requires prior password or disablement. A disabled former
blank becomes unset and cannot be enabled without a nonempty password. The
private authorized ACCT_REPLACE operation atomically disables inactive blank
CiukiUser and enables its already configured inactive replacement administrator.
It is deliberately not assigned an unfrozen public endpoint operation. Restoring
names/types, deleting other users, service restart and reboot never rearm blank
logon. Confirmation/UI integration belongs to the later account-controls task.

Every mutation uses one complete OWNER batch: request 160+128*N, reply 96+32*N,
key (SID, destination volume, G). All accounts, including disabled built-ins,
have bindings; account-only operations use KEEP, deletion RETIRE preserves data.
`stage` resolves destination-issued stable locators, proves any protected G32
mapping/collision moves, stages under deny-first labels and flushes destination
files/directories/volume metadata. `validate_stage` rechecks that immutable
manifest and its objects during recovery; missing/corrupt staging closes the
gate. Neither callback may mutate source media/session data or reinterpret a
client LBA/directory slot as a stable locator. Real OWNER enforces aliases,
32-profile capacity, nonreused keys/generations and lifetime STATUS receipts
bounded at 1024; the fault backend models that contract, not live enforcement.

Durability files are all SYSTEM-only below `/System/Security`:

- `accounts.alloc.0`/`.1`: two checksummed allocation slots, each 96 bytes, sealing
  SID/volume and highest reserved G/next_uid. Never overwrite the newest slot;
  aborted or cancelled reservations survive old database restoration.
- `accounts.db.txn`: immutable checksummed reservation with the full encoded
  request, target database bytes/digest and prior database digest. It seals all
  intended enabled/disabled records and the complete output before publication.
- `acct.prepared`, `acct.commit`, `acct.owner`, `acct.done`, `acct.abort`: keyed,
  checksummed phase records. `acct.commit` is COMMIT_REQUIRED and irrevocable.
  Cancellation before it aborts the entire vector; afterwards only reply delivery
  is cancelled and recovery finishes publication.
- `accounts.db.new`/`.bak`: flushed staging and prior recovery evidence. OWNER
  commits before database installation. Durable DONE, bootstrap consumption and
  retirement precede acknowledgement. Remove backups durably before the intent;
  a leftover backup without an intent is contradictory, never password fallback.

`bootstrap.c` exports the exact 160-byte CIUKIBST state codec for user-02.
Only matching, flushed UNPROVISIONED seed/state/origin/volume evidence with no
prior reservations/owners/database may begin fresh provisioning. ACCOUNT never
creates this initial state. `bootstrap.sid` seals the entropy-backed SID to that
recipe; `machine.sid` is flushed before deriving the ready empty verifier. A
matching pre-SID_READY retry reuses the SID. The full reservation then seals G,
manifest and output; SID_READY never generates a new identity/verifier. Promotion
advances OWNERS_READY, ACCOUNTS_READY and CONSUMED using `.new`/`.txn`/`.bak`, with
checksums on every candidate and intent. Both flags and generations never regress.
Missing or contradictory evidence closes recovery. CONSUMED never replays a seed,
even if accounts.db disappears. The seed may remain as harmless consumed evidence.

Ready CIUKISEE has generation 0, exactly three records, uid 1000 flags=9/KDF=0,
unset 500/501 and zero SID. Publication produces flags=13 and a fresh salted
empty verifier. Setup retains both independent entered verifiers, disabled 500,
unset Guest and enabled custom uid 1000. No ready recipe is accepted on a Setup
destination. Protected enrollment uses origin 3 and its own manifest, not a seed.

The G32 adapter only decodes v1 and returns a disabled enrollment draft plus
explicit old-id/new-uid/existing-root maps. It preserves verifiers and all sixteen
profiles; unknown old password length never enables an imported account.
Trusted enrollment verifies the old secret and commits a conforming replacement
without creating a session or silently enabling it. user-02 owns actual source
protection, collision relocation, enrollment approval and destination promotion.
Shared /Documents and desktop behavior remain unchanged.

Run host gates serially, keeping ASan/UBSan and default leak detection:

```
nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_accounts.py
nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_logon.py
nice -n 19 python3 -B -m unittest tests.host.desktop.test_users
```

H10 here covers amendment service invariants; later user-03 owns ordinary logon
UI tests. Research and evidence are in `docs/validation/sec-user-01.md`; exact
commands, limitations and lead handoff are in `build/codex/sec-user01-report.md`.
