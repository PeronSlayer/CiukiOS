# f5-19 (contract) — Win16 subsystem inside F5: contract before code (owner decision 2026-10-14: full Win16, not only installer substitution)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f5-19c` from `main`. Document-only deliverable.

## Input

`docs/design/win32-personality-contract.md` (Q3 decision and the cross-review's M06 "Win16 is more than DPMI", section 4.2 row f5-19), `docs/design/research/win32-layer-2026-10-14.md` (Worms Armageddon demo: InstallShield 5 with a 16-bit SETUP.EXE; NFS II SE stub is a PKZIP self-extractor; Microsoft's installer substitution on 64-bit Windows), the F3 DOS/DPMI infrastructure (`docs/design/*vm*`, `src/kernel/vm/dpmi*.c`: DPMI 0.9 host, LDT, real-mode callbacks), the desktop protocol.

## Task

Write `docs/design/win16-subsystem-contract.md` with primary sources (Microsoft Windows 3.1 SDK/DDK documentation of the NE format and KRNL386, the Windows 95 "WOW"/thunking architecture in Pietrek's books and MSDN, Wine's `winevdm`/`krnl386.exe16`/`user.exe16` design, the DPMI 1.0 specification, the InstallShield 3/5 setup engines): NE loader (segments, relocations, resources, self-loading modules), the 16-bit kernel/user/gdi API surface the installers of the era use (census method of f5-02 applied to the 16-bit setup stubs and engines of the acceptance set), memory model (global/local heaps in LDT selectors, the 16-bit address space inside a Win32 personality process), thunking between the 16-bit USER/GDI and our 32-bit USER/GDI (flat thunks, callbacks), message loop and cooperative multitasking semantics (one 16-bit VM per process group, yields), file/registry/INI access, the relationship with the F3 DPMI host (which parts are reused untouched), containment (a crashing 16-bit program never takes the desktop), diagnostics, acceptance (the Worms Armageddon demo setup and an InstallShield 3 fixture complete; a self-authored Open Watcom 16-bit fixture suite), host-test strategy (NE parser/relocator and thunk tables on Linux; execution only in QEMU/T23), open questions for the owner, ordered implementation directives (f5-19a..d) with dependencies on f5-03..f5-10. Append a dated subsection to the Win32 contract pointing to it.

## Acceptance

Contract committed; the lead cross-reviews it (Codex second session) before any f5-19 code.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`), record links and decisions in the relevant `docs/design/` document (append dated subsections; never rewrite earlier text), then implement and test. Report `build/codex/f5-19c-report.md`: findings, changes, host tests, what the lead must verify on QEMU and the T23.
