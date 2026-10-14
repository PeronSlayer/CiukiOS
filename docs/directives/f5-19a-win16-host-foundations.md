# f5-19a — Win16 host-side foundations: NE parser, relocator and resources, synthetic NE fixtures, 16-bit installer census (allowed by the cross-review before the P0 handoffs)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f5-19a` from `main`. Host-only deliverable (no kernel or runtime code).

## Input

`docs/design/win16-subsystem-contract.md` (sections on the NE loader, census procedure, acceptance, directives f5-19a..d) and its appended cross-review of 2026-10-14 (verdict: approve with listed changes; host-only f5-19a research, fixtures and parser work may proceed; P0 items C01, C02, G01, G02, G04, G07 are handoffs for the lead and f5-03/f5-04/f5-10, not for this directive). `docs/design/win32-personality-contract.md`, f5-02's fixture tooling (`scripts/win32/fixtures.py`, `tests/host/win32/`), the research `docs/design/research/win32-layer-2026-10-14.md` (Worms demo: InstallShield 5 with a 16-bit SETUP.EXE, _SETUP.DLL, _ISDEL.EXE). No Open Watcom on this workstation: fixtures are synthesised by Python from an NE model (like f5-02's synthetic PE fixtures) with manifests; if an Open Watcom 1.9 release can be pinned, record its URL/hash as `pending-download` and the recipe to build the real fixtures later.

## Task

1. **NE model and parser** (`sdk/win16/ne/`, C, i386, freestanding like f5-05's loader): NE header, segment table and flags (code/data, movable/preload/discardable, iterated, self-loading flags), resource table, resident/nonresident name tables, module reference and imported names tables, entry table bundles (fixed/movable/const), per-segment relocation records (internal ref, imported ordinal, imported name, OSFIXUP) with additive and chained fixups, 64 KiB sector/offset arithmetic, self-loading modules recognised and refused for now with a named reason; every refusal names the rule.
2. **Synthetic fixture corpus** (`tests/host/win16/fixtures/`, Python generator `scripts/win16/fixtures.py`): at least 40 NE fixtures with manifests covering each table and flag combination, each relocation type and chain, iterated segments, resources by type/name, malformed images; reproducible hashes.
3. **Census tool for NE** (`scripts/win16/census.py`): imported modules/ordinals/names of NE images, output as text; run on the fixtures; the format for the acceptance set's 16-bit setup stubs is documented (payload bytes never committed).
4. **Design inputs for the P0 handoffs** (document only, `docs/design/win16-subsystem-contract.md` dated appendix "f5-19a inputs"): the exact fields the kernel ABI needs for the 16-bit execution context (C01), the USER queue model per task (C02), the DOS/DPMI address model options with numbers (G01), the startup/callback ABI tables (G02), the message conversion matrix skeleton (G04), the containment enforcement points (G07) — so the lead can dispose them with f5-03/f5-04/f5-10.
5. Host tests: parser against every fixture (accept/refuse with reason), relocation application on a mock segment store, census of fixtures matches manifests.

## Acceptance

Host suites green; no runtime claims. Files: new `sdk/win16/`, `scripts/win16/`, `tests/host/win16/`, contract appendix. Do not touch the kernel, `sdk/win32/` (f5-05) or the desktop.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`); record links and decisions in the contract appendix. Report `build/codex/f5-19a-report.md`.
