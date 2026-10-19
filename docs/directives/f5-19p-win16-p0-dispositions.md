# f5-19P — Win16 P0 handoff dispositions (document only): proposals for C01, C02, G01, G02, G04 and G07 so that f5-19b and f5-19c can start

- **Phase:** F5 (Win16). **Model:** Antigravity Gemini 4 Argon high (Codex out of credit). **Worktree:** `wt/f5-19p` from main.
- **Contract:** `docs/design/win16-subsystem-contract.md` — §13 "Ordered implementation directives and dependencies" (the rule that f5-19b/c do not start until the P0 handoffs are accepted by the lead), the Codex cross-review §1–§4, and "f5-19a inputs — 2026-10-15" §E (design inputs E1 C01 kernel 16-bit execution context, E2 C02 USER queue model per Win16 task, E3 G01 DOS/DPMI address model, E4 G02 startup and callback ABI tables, E5 G04 message conversion matrix skeleton, E6 G07 containment enforcement points) and §F (open points). Read the code that now exists for each owner: f5-03/f5-04/f5-05B/f5-06 (`src/kernel/proc/win32/`, `sdk/win32/ntdll`, `sdk/win32/kernel32`), f5-10A/B (`sdk/win32/user32`, `sdk/win32/gdi32`), F3 (`src/kernel/vm/`, DPMI) and their reports in `build/codex/`.
- **Authority:** a new section "P0 handoff dispositions — proposal (f5-19P)" appended to `docs/design/win16-subsystem-contract.md`, and nothing else. No code.

## Required

1. For each of C01, C02, G01, G02, G04, G07: the concrete interface the owner must provide (types, fields, calls, error codes, lifetime and locking rules), checked against the code that exists today on main (cite file:line), the alternatives considered, the recommended disposition and the exact changes it implies for the owner directive (f5-03/04/05/06/10, F3) and for f5-19b/c.
2. For §F's open points: the primary sources to consult (Windows 3.1 SDK vol. 4, Open Watcom `wlink` output, period files) with links, what each would settle, and a recommended interim rule so that f5-19b can proceed without inventing relocation or x87 fixup behaviour.
3. A short ordered plan for f5-19b split into parallel worktrees as §13 suggests (selector/accounting, NE instantiation/relocation, heaps/thunk tables, integration owner), with the host gates of the contract per worktree.

## Constraints

Document only; use `rg` and `sed`, never `semble`; no builds or tests needed; no commits, no diary edits. Mark every disposition "proposed" — the lead accepts or changes it before f5-19b launches. English.
