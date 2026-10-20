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

## Lead review (2026-10-20 05:45): dispositions accepted, decisions

The delivery (Claude Opus session after an Antigravity session stopped by quota; report `build/codex/f5-19p-report.md`; new section of `docs/design/win16-subsystem-contract.md` from line 1518) is accepted: C01 (per-host kernel LDT, op 10Fh, blocking run call 110h returning every fault/INT to the flat caller), C02 (one Win32 thread per Win16 task, group token released only at Windows 3.1 yield points), G01 (no companion DOS VM; per-host DOS arena at 10000h–9FFFFh), G02 (startup, callback, Catch/Throw, self-loader tables), G04 (message conversion in the Win16 host, procedure-based renumbering), G07 (kernel NE recognition at process creation, group ledger). The previous draft's errors listed in the report are superseded. Decisions:

1. **INT 21h backend of the Win16 host:** the kernel's VMM DOS file API (f3-48…f3-51, merged), not a second implementation over the Win32 file services — one DOS file implementation, the IFS Manager model of the owner's decision 2026-10-19-06. The contract's §8.1 wording is amended accordingly in f5-19b; Q65128's protected-mode failure rows stay.
2. **Versions seen by 16-bit programs:** `GetVersion` 3.95, DOS 7.10, INT 2Fh 1600h 4.10, proposed; f5-19b confirms the Windows 98 values from the Resource Kit media (`build/rskt/`) or the Windows 98 SDK before freezing them, and records the source.
3. **x87 emulator fixups:** recorded and patched to native x87 on first execution (the Wine behaviour); f5-19a's refusal is lifted in f5-19b.
4. **32-bit segments (Win386 extender, Win32s-style code):** refused in this phase. **Owner decision 2026-10-20 (dev diary 2026-10-20-03): they enter F5's scope** — USE32 segments in NE modules, Watcom Win386 programs, `WINMEM32.DLL`, Win32s Universal Thunks and Windows 95/98 flat thunks — in a dedicated step f5-19e after f5-19b–d, extending C01 to 32-bit descriptors.
5. Departures accepted as recorded (no espfix, as in F3; posted pointer messages refused). A launch from outside a group always creates a new group.
6. The 32-threads-per-process limit stated by the Win32 contract is enforced; it joins f5-19b's kernel part.
7. Operation numbers 10Fh and 110h are reserved for Win16 in the f5-03 table.
