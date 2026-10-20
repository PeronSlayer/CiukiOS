# f5-20D — the reused Wine DLL build as a cached step of `make build-full`, msvcrt staged, core-DLL hashes refreshed automatically, the `w32-upper-dlls-qemu-t23` guest probe

- **Phase:** F5 (build and personality). **Model:** Claude Opus agent (Codex out of credit until 2026-10-25). **Worktree:** `wt/f5-20d` from main.
- **Context:** `docs/directives/f5-20-core-exports-for-reused-dlls.md` amendments 1 and 2 and the part C report (`build/codex/f5-20c-report.md`): the qualified `msvcrt.dll` is not staged because the canonical build would depend on a 6-minute Wine upper build; the core-DLL hashes in `config/wine-upper.json` go stale at every core change (f5-10C2 amendment item 4); `f5-14B` keeps the reproducible build recipe (`scripts/win32/build-wine-dlls.sh --upper-build`, `scripts/win32/wine_upper.py`).
- **Authority:** `Makefile` (one cached target), `scripts/win32/wine_upper.py`, `scripts/win32/build-wine-dlls.sh`, `scripts/build_image.py` (staging), `config/wine-upper.json` (automatic hash refresh in the check, never by hand), `tests/host/win32/wine/`, `tests/suites/f5-core.json` (`w32-upper-dlls-qemu-t23`), a guest probe under `src/probes/` and `src/kernel/probes/` registered in the kernel selector **and** the loader's `f5_probe_names` table (`src/boot/ciukldr/menu.inc`), `docs/build-and-run.md`, `third_party/wine/README.md` (the LGPL source offer once a reused PE ships in the image).
- **Not yours:** the Wine patches 0001–0010 (f5-14B/f5-20C), the exports (f5-20A/B).

## Required

1. **Cached upper build.** `make build-full` builds the reused DLLs once per (Wine archive hash, patch set hash, core-DLL export hash) into `build/win32/upper/` and reuses them while the key is unchanged; a rebuild is logged with its reason; the build never downloads; `make CAP= apps` stays independent of it. Host time of a warm `make build-full` unchanged within ±5 %.
2. **Staging and hashes.** `msvcrt.dll` qualified and staged in the builtin store (`QUALIFIED` restored); the other reused DLLs staged as each one's load/DllMain evidence qualifies it (the directive's qualification rule); `wine_upper.py check` refreshes the core-DLL hashes from the built core and fails only on a semantic mismatch (missing export, changed decision), never on a hash that merely moved with a rebuild.
3. **Guest probe.** `f5:w32-upper-dlls`: loads every staged reused DLL in a Win32 process through the production loader, runs DllMain attach/detach, exercises one census entry per DLL, emits trusted `CIUKI_TEST` records; the case's predicates dry-run against the host run; the name registered in the kernel selector and the loader table with a host test that compares the three sources (the f5-17B lead note).
4. **Licensing.** The LGPL source offer text and the patch list in `third_party/wine/README.md` and the release policy note (`config/release-policy.json` comment) updated once a reused PE ships in the image.
5. **Evidence.** `make build-full` twice (cold, warm) with timings and identical image payload hashes for the reused DLLs; `python3 -B -m unittest discover -s tests/host/win32 -t .`; `tests/host/test_image_payloads.py`; `wine_upper.py check`. Report `build/codex/f5-20d-report.md`.

## Constraints

rg and sed, never semble; no QEMU, no systemd-run (the lead runs `make build-full` in a scope: give the lead the exact commands and run `make CAP= apps` and the host tests yourself), no commits, no diary edits; host tests one at a time with `nice -n 19`; never disable sanitizer leak detection where the repository script does not; no new `panic()` paths; English.
