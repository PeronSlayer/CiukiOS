# SYSTEM account service core

This private service implements `sec-user-01`; it does not install an endpoint or
change G32 desktop logon. Build it with `crypto.c`, `codec.c`, `store.c`,
`service.c`, `migration.c`, the existing SHA-256 library and G32 `users.c` for
read-only v1 decoding. There is no vendored implementation or `.c` inclusion.

`acct_service_open(..., host_test=0)` refuses bindings unless kernel, owner and
storage adapters all attest real implementations. The host harness explicitly
uses test mode. Only sealed boot code constructs adapter tables; no request can
set their flags. Production binding is the lead's k01/k02 integration, rather
than an insecure executable temporarily registered as SYSTEM.

`adapter.h` is the sole private kernel seam. The kernel binding must acquire a
sealed SYSTEM/session-0 lease, enforce one live service, attest trusted-gate and
actual Safe Mode state, and retain endpoint subjects across waits. It maps
identity queries to frozen SYS_SECURITY 101 QUERY_SELF/QUERY_PEER. Copied peer
credentials alone are insufficient: endpoint closure/reuse/session revocation
must invalidate `peer_alive`. `active_uid` includes closing sessions. Closing a
service requires joining its worker first. Service/session admission must be
serialized so active-account checks cannot race a new logon.

The k02 binding authenticates the retained SYSTEM peer before use. PREPARE
resolves/stages an inaccessible profile and reserves capacity, COMMIT publishes
its durable owner, ABORT cancels a pending transaction while retaining retired-id
evidence, and STATUS is idempotent for the exact transaction/uid/content. The
private `binding` query fills the committed canonical root/key/generation by uid,
including mapped G32 roots; it cannot rebind an owner. Retired bindings deny reuse.
One account mutation publishes one owner transaction, avoiding assumptions about
the size of owner-generation increments. Disabled built-ins receive their owner
on first enablement; a configured disabled 500 receives one on its first
password-authenticated Safe Mode use, before any logon uid is returned.
Seeds contain the two disabled built-ins and one enabled
ordinary administrator. Multi-profile enrollment is staged by sec-user-02.

Storage callbacks operate only under `/System/Security` on the identified system
volume, after kernel policy protects that directory and all aliases. A POSIX
0600 bit is insufficient. Reads are bounded and must detect trailing bytes;
writes handle short I/O and return normal errors. Native rename returns EEXIST
without replacing a destination. `flush` must flush file data and `barrier` must
persist namespace/volume metadata. No I/O or KDF runs under the short queue lock.
The worker is polled from a service worker thread, never an input callback.

`accounts.db.new` is flushed before the checksummed intent; owner commit precedes
canonical installation. Native replacement moves the old database to `.bak`
without deleting it. After canonical flush/barrier, `.bak` and bootstrap pending
data are removed durably before the intent is removed. Only then is success
acknowledged. Without an intent, a backup is contradictory evidence, never a
password fallback. With an intent, only the named generation/size/digest may be
installed. Invalid candidates retain evidence and stop authentication. These are
host-tested protocol guarantees, not arbitrary FAT corruption repair claims.

Creation first commits a generation containing the advanced UID high-water mark.
Failure after that reservation consumes the ID. Full account capacity, malformed
requests, entropy failure and OOM before reservation consume none. An owner
failure can leave an inaccessible orphan and a retired ID. Delete retires the
owner without deleting its files. Missing/corrupt ownership prevents service
startup. A client dying before publication is checked again before owner COMMIT;
pending ownership is aborted and the cancelled transaction generation is consumed
durably as a reservation. Work already durably committed is completed without
returning a successful response to a dead/reused peer.

Bootstrap validates a CIUKISEE seed and obtains the machine SID and ready-user
salt from entropy. Its expected origin comes from the sealed install manifest;
a ready seed passed for a Setup installation fails before entropy or writes.
A flushed `provision.pending` contains the resulting database
so restart can finish with the same SID. A checksummed `provision.used` survives
cleanup and prevents seed replay after loss of the database. A torn/corrupt marker
fails closed. Setup seed verifiers are preserved; ready seeds contain none for
CiukiUser. Subsequent provisioning and repair UI belong to sec-user-02.

The G32 adapter validates v1 through the existing decoder without touching its
files/locks. It returns a draft and explicit old-id/new-uid/existing-root map,
with uid 1000 chosen explicitly and other IDs assigned in old-id order. Reserved
names require replacements; blank passwords remain disabled; every imported
verifier requires trusted enrollment before ordinary logon, because its original
password length is not encoded. The draft is not a seed or a publishable store:
user-02 must supply conforming enrollment/recovery secrets, journal collisions and
maps, protect old evidence, and commit ownership before enabling sessions.

Run the linked production harness and unchanged G32 regression serially:

```
nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_accounts.py
nice -n 19 python3 -B -m unittest tests.host.desktop.test_users
```

Both compile with ASan/UBSan, retain leak detection, and use disposable host
artifacts. Research decisions are in `docs/validation/sec-user-01.md`.
