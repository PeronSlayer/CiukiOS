# SYSTEM account service: host implementation decisions

Scope: directive [sec-user-01](../directives/sec-user-01.md). Runtime sealing,
kernel/owner binding, image integration and hardware durability remain lead work.

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
