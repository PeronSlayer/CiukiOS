# f5-14 (milestone A) — Wine upper-DLL reuse: pinned release, dependency closure and build audit before the core ABI freeze

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f5-14` from `main`. Host-only deliverable.

## Input

`docs/design/win32-personality-contract.md`: option C (owner decision Q2: CiukiOS writes the core DLLs, the installer-facing upper DLLs are reused from one pinned Wine release under LGPL-2.1-or-later), the directive paragraph "f5-14 — Reused upper DLLs" (line 1435), the Codex cross-review sections 2 (contradictions), 4.2 row "f5-14A/B/C" and 6 (risks: the audit must happen **before** the core ABI is frozen because it decides which core exports the reused DLLs really need). The review found that Makefile dependency reads of Wine master are not a pinned census.

## Task

1. **Pin** one Wine release (recommend the latest stable 10.x; record the alternative 9.0 LTS) with archive URL and SHA-256 in `config/wine-pin.json`; if the download fails, record the pin with `status: pending-download` and work from the git mirror's tagged sources via the web as far as possible.
2. **Dependency closure**: for the upper DLL list of the contract (shell32, comctl32, comdlg32, ole32, oleaut32, rpcrt4 subset, msvcrt, version, setupapi, advapi32 parts, winmm parts, dinput/ddraw/dsound as candidates) compute from the pinned sources the exact set of imports each needs from ntdll, kernel32, user32, gdi32, wineserver protocol usage (which requests), unixlib calls (`__wine_unix_call`), and Wine-private exports; produce `docs/design/win32-wine-dependency-audit.md` with tables: DLL → required core exports (count and list), wineserver requests used, unix-side calls, PE-only buildability with `winegcc`/`winebuild` producing pure PE (`--builtin`, `-mno-cygwin` modern PE builds), licence notices required, patches expected.
3. **Build experiment**: with the tools on this machine (clang + lld; no MinGW GCC) attempt to build `winebuild` and one small upper DLL (`version.dll` or `comctl32.dll`) as PE from the pinned sources; record exactly what is missing (mingw-w64 headers/CRT, winegcc) with pins for the lead; if nothing can be built here, deliver the build recipe as a script `scripts/win32/build-wine-dlls.sh` with the expected outputs and hashes to be verified by the lead.
4. **Decision input**: given the closure, estimate the size of the core (number of ntdll/kernel32/user32/gdi32 exports the reused DLLs need versus what the ten titles need per the f5-02 census format), the memory footprint at 128 MiB (sum of PE sizes and heaps), and whether option C still beats a full Wine port on POSIX (the review's alternative); recommend, with the numbers.
5. Append a dated subsection to the contract's review area: "f5-14A findings" (facts only).

## Acceptance

Audit document and pin files committed; host test `tests/host/win32/test_wine_audit.py` validates the audit tables' consistency (every required export named exists in the pinned sources' `.spec` files you parsed); the lead decides on the core ABI freeze with these numbers.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search` is enabled), record links and decisions in `docs/design/` (amend `win32-personality-contract.md` only by appending a dated subsection to its review area, never rewriting earlier text), then implement and test. Report `build/codex/f5-14-report.md`: what was found, what changed, which host tests prove it, what the lead must decide or verify.
