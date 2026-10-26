# sec-contract: reconcile the two halves of the user security contract

- **Written:** 2026-10-26 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: wt/sec-contract (main a5e35dcf2 + both drafts). Documentation only, no code.

Two implementers wrote the user security contract in parallel from the same shared decisions (`docs/directives/sec-kernel.md`, `docs/directives/sec-model.md`): `docs/design/user-security-kernel.md` (kernel enforcement, syscall inventory, path policy, ABI proposal, directives sec-k01..k08) and `docs/design/user-security-contract.md` (accounts, logon, Setup, Control Panel, Win32/native/DOS, migration, non-kernel directives). The lead's answers to both sets of open questions are in `docs/directives/sec-lead-decisions.md` and are binding.

Do this:
1. Apply every lead decision in both documents (replace the open-question tables with a "Decisions" table citing the decision ids; keep only genuinely new questions).
2. Reconcile the two halves so they never contradict: identities, groups, credential record, session lifecycle, account service/SYSTEM bootstrap, sender identity, owner/account transactions, profile paths, shared /Documents, root creator-owner rule, removable media, raw disk, registry (per-user w32srv + SYSTEM machine broker, HKCU/HKU, .DEFAULT), Safe Mode CiukiAdministrator, Guest, ready images and Setup, password policy and retry backoff, recovery. Where the halves disagree, choose the one consistent with the lead decisions and say so in a short "Reconciliation" section at the top of user-security-contract.md.
3. Freeze the ABI proposal for sec-k01 (call number per the repository allocation rules in abi.h/execution-abi.md, record layouts one field per line, errors, bounds) in user-security-kernel.md so the implementer of sec-k01 needs no further design.
4. Produce one ordered, parallelizable implementation plan covering both halves: every directive with id, dependencies, files/areas, acceptance criteria and host tests, marking which ones can run concurrently after sec-k01 (the lead wants to run several Codex sessions in parallel). Put it in user-security-contract.md section "Implementation plan" and make the kernel half point to it.
5. Keep docs/design/users-and-logon.md (G32 supersession notice) and the inventory rows consistent with the result.
Report to build/codex/sec-contract-report.md (what changed, remaining questions). Leave changes uncommitted.
