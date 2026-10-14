# f5-02 (milestone A) — PE toolchain pins, fixture corpus and the provisional Win32 API census

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f5-02` from `main`. Host-only deliverable (no guest code).

## Input

`docs/design/win32-personality-contract.md`: the directive paragraph "f5-02 — PE toolchain, fixture set and API census" (line 1356) and the Codex cross-review appended at its end (section 4.2 row "f5-02A/B", section 4.4 "PE binaries and honest host evidence", section 3.1 M01-M03 on the compatibility matrix, calling conventions and loader details). The review orders f5-02A first; f5-14A (Wine dependency audit) and f5-17A depend on it. The lead's machine has clang (check `clang --print-targets`), no MinGW GCC and no Wine; network downloads by you may fail: when a pinned archive cannot be fetched, write the exact URL, size and SHA-256 you expect into `config/toolchain.json` with `status: pending-download` and build the fixtures that clang's `i686-w64-mingw32`/`i686-pc-windows-msvc` targets can produce with `lld-link` and `llvm-dlltool` alone (import libraries from `.def`, no CRT: freestanding fixtures with their own entry points).

## Task

1. **Pins** (`config/toolchain.json`): clang/lld version in use, the i386 Windows targets, `llvm-dlltool`, `llvm-rc` or `windres`, a mingw-w64 headers/CRT release (URL, hash), optional NSIS 2.x release for a period installer fixture; a `scripts/win32/toolchain.py` that verifies the pins and reports what is missing.
2. **Fixture corpus** (`tests/host/win32/fixtures/`, sources only; built artefacts under `build/host/win32/` with reproducible hashes): at least 60 PE32 fixtures covering every loader rule of the contract (imports by name/ordinal, bound imports, forwarders, relocations with and without `.reloc`, TLS callbacks, resources by language/name/ordinal, sections with `VirtualSize==0`, low alignment, packed-like self-modifying code, SEH chain and nested handlers, stdcall/cdecl/varargs/thiscall adapters, 64-bit returns, DF/nonvolatile register rules, console and GUI subsystems, DLLs with `DllMain` ordering, malformed images that must be refused with the exact reason). Each fixture carries an expected-behaviour manifest (JSON) a host test reads.
3. **Census tool** (`scripts/win32/census.py`, pure Python, pefile if available otherwise a self-contained parser): static imports (name/ordinal/delay-load/bound), exports, subsystem, linker version, packer heuristics, manifests; output as text (DLL → function list) with no payload bytes. Run it on the fixtures and on any freely distributable period binaries the lead has (list what you would need: the Half-Life Uplink demo, Quake II demo, StarCraft demo, Age of Empires trial, the NSIS/InstallShield sample installers) and commit only the text census; where binaries are absent, write the census format and a stub list to be filled by the lead.
4. **Compatibility matrix** (`docs/design/win32-compatibility-matrix.md`): one row per approved title/version and installer (exact setup entry point, engine, PE/NE/DOS, codecs, disc requirement, renderer, audio, input, expected exclusions), with sources; mark unknown cells explicitly.
5. Host tests: `tests/host/win32/test_fixtures.py` (every fixture builds twice to the same hash, manifests validated), `test_census.py` (census of the fixtures matches the manifests).

## Acceptance

All new host tests green; `config/toolchain.json` verified by the script; the census text committed; no binary payload in the repository except fixture sources. The lead fetches any pending download and reruns the script.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search` is enabled), record links and decisions in `docs/design/` (amend `win32-personality-contract.md` only by appending a dated subsection to its review area, never rewriting earlier text), then implement and test. Report `build/codex/f5-02-report.md`: what was found, what changed, which host tests prove it, what the lead must decide or verify.
