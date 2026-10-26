# sec-user-01: SYSTEM account service and store

- **Written:** 2026-10-26 11:24 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: wt/sec-contract 3ddc9de88.
- **Specification:** `docs/design/user-security-contract.md` sections 3 (accounts, password policy, v2 format, serialization/recovery, account/owner commit and bootstrap handshake), 4.1 (service interfaces), 6.3 (G32 conversion inputs) and the "Implementation plan" row 2B (sec-user-01) with acceptance H03/RUS; kernel mechanics in `docs/design/user-security-kernel.md`. Lead/owner decisions: `docs/directives/sec-lead-decisions.md`.
- **Parallel work:** sec-k01 (kernel credentials, SYS_SECURITY=101, abi.h, libc wrappers) is being implemented at the same time in another worktree. **Do not edit** src/kernel/include/ciuki/abi.h, src/kernel/proc/*, common syscall tables, the SDK ABI copies or shared test scripts. Code the service against the frozen call-101 interface through one small private adapter (a header with the calls you need and a host fake), so the lead can bind it to the real k01 implementation at integration. Same for the k02 owner service: use the specified owner-service adapter and a fault-injecting backend; the service must refuse live publication until that backend is real.

## Scope
The sealed SYSTEM account service: private account IPC, the v2 store codec, PBKDF2-HMAC-SHA-256 verifiers (10,000 iterations, per-record count stored), password policy (8-63 printable bytes, the single ready-image CiukiUser blank exception), entropy-backed machine SID bootstrap, one KDF and one pending request at a time with the 1000 ms retry backoff, identity rules (ids never reused, 500/501/1000+, last-administrator and active-account EBUSY rules), durable reservation, stale-generation and crash-lock recovery, no rollback of an acknowledged password change, and the migration adapter from G32 `users.dat` (apps/desktop/users*: read-only import into the enrollment flow; do not change the current desktop logon behaviour, which sec-user-03 replaces later). No logon UI here.

## Tests
H03 and RUS as defined in the contract, with fault injection (entropy failure, OOM, disk full, power cut at every write/flush, corrupt/truncated store, concurrent clients), constant-time verifier comparison, buffer wiping, and the G32 import cases. Run one at a time with nice -n 19.

Report to build/codex/sec-user-01-report.md. Leave changes uncommitted.
