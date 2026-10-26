# sec-k01: credential and session foundation

- **Written:** 2026-10-26 11:24 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: wt/sec-contract 3ddc9de88 (main a5e35dcf2 + the reconciled user security contract).
- **Specification:** `docs/design/user-security-kernel.md` (frozen SYS_SECURITY=101 ABI, credential lifetime, sessions, queries and IPC subjects, the inventory) and `docs/design/user-security-contract.md` section "Implementation plan", row 1 (sec-k01) with its acceptance criteria and host evidence H01. Owner decisions and lead decisions are in `docs/directives/sec-lead-decisions.md`. Implement exactly that row; do not start rows k02+ (file policy, device authority, DOS, Win32 broker, logon UI).
- A read-only cross-review of the contract runs in parallel; if it changes the k01 ABI the lead will send an amendment. Keep the ABI exactly as frozen until then.

## Scope (from the plan row; the contract is authoritative)
Immutable per-process credential record created before process/thread publication; inheritance through proc_prepare/spawn/thread creation/Win32 child spawn/exec replacement; the seven call-101 operations with exact layouts, constants and errors; gid 0 for SYSTEM, invalid-uid sentinel, monotonic session ids; public spawn can never elevate; protected service bootstrap (the boot path that creates the SYSTEM account service) and cross-credential fd sanitization (no SYSTEM descriptor survives into a user process); retained-subject primitives for channels/services (a server can learn the authenticated credential of its peer, which cannot be forged by a uid field in a message); libciuki/newlib query wrappers (getuid/geteuid/getgid/getgroups and the session/profile queries the contract names); ABI oracle rows (one field per line in abi.h), SDK header copies, API reference regeneration. Until the logon service exists (sec-user-03), boot must keep working: define and implement the bootstrap rule the contract gives for the desktop session before real logon (no regression of the current desktop, Setup or test boots).

## Tests
H01 as defined in the contract, plus: allocation/copy failure at every step exposes no child or session; peer fd reuse and forged uid fail; existing kernel process/desktop/Win32 spawn regressions (scripts/test/host_kernel_tests.sh relevant sections, tests/host/proc, tests/host/win32 test_kernel_core and test_kernel_loader), ABI section, `python3 scripts/gen_api_reference.py` and `--check`. Run them one at a time with nice -n 19.

Report to build/codex/sec-k01-report.md. Leave changes uncommitted.
