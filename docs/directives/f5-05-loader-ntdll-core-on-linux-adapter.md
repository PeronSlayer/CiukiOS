# f5-05 (part A) — PE loader and `ntdll` core developed on a Linux i386 adapter in parallel with the kernel core

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f5-05` from `main`. Host-only until f5-03 lands; the code targets the frozen record header of f5-03a (coordinate: read `docs/design/win32-personality-contract.md` and `execution-abi.md` for the header once f5-03a appends it; until then implement behind a thin service interface `w32_services` with a Linux implementation and a CiukiOS implementation stub).

## Input

Contract sections "PE loader" (accepted images, relocations, imports/exports/binding, TLS, initialisation and loader lock, search order and the builtin store), "Memory" (heaps, sections), "Exceptions" (SEH dispatch and unwind on x86, FS:[0] chain), the directive paragraph "f5-05 — Loader and ntdll core", the cross-review's M03/M04, the research `win32-layer-2026-10-14.md` sections 3-5 (census: 596 functions for five titles, 93 common CRT-startup imports; ordinal imports in wsock32/comctl32/dplayx; `smackw32.dll`/`eacsnd.dll` based at 0x400000; a Linux i386 adapter built like `tests/host/native_samples_host.c`; a stub generator producing named stubs for every censused function so every title loads from the first image; Wine 11.18 on the workstation as a behavioural oracle), f5-02's fixtures (`tests/host/win32/fixtures`, `scripts/win32/fixtures.py`), the owner's decision: CiukiOS writes more DLLs itself, LGPL-2.1-or-later.

## Task

1. **Loader library** (`sdk/win32/ldr/`, C, i386, no libc dependency beyond our own `rtl`): PE32 parsing with every f5-02 rule, image mapping into a provided address-space service, relocations (HIGHLOW/ABSOLUTE/HIGH/LOW/HIGHADJ), imports by name/ordinal/forwarder (cycles bounded), bound IAT policy, delay-load tables, TLS directory and callbacks, resources (type/name/language lookup, DATAFILE loads), DLL reference counting, `DllMain` ordering with the loader lock, unload, search order and the builtin store manifest; every refusal names the f5-02 reason.
2. **ntdll core** (`sdk/win32/ntdll/`): SEH dispatch and unwind on x86 (FS:[0] chain, nested exceptions, collided unwind, `RtlUnwind`, `RaiseException`), heaps (`RtlAllocateHeap` family with Windows semantics and debug fill), critical sections with the kernel wait primitives, `Rtl*` string/path/unicode helpers, `Ldr*` exports, the PEB/TEB/process parameters layout fields of the era, `NtCurrentTeb` via FS.
3. **Stub generator** (`scripts/win32/stubs.py`): from the census text files, generate named exported stubs for every function not yet implemented, each recording one bounded diagnostic on first call; export tables with Windows-exact ordinals where the census needs them (wsock32, comctl32 #17, dplayx 1/2/4, winmm 2).
4. **Linux i386 adapter** (`tests/host/win32/adapter/`): maps PE images with mmap, provides FS via `modify_ldt`/`set_thread_area`, routes the service interface to Linux; runs the f5-02 fixture corpus through the real loader and ntdll, and a Wine comparison harness for selected behaviours (`wine` on this workstation) where licences allow.
5. Host tests: every fixture loads or is refused with the right reason; SEH fixtures; heap conformance; stub generation reproducible.

## Acceptance

Host suites green; the loader and ntdll build for both the Linux adapter and the CiukiOS target (`i686-pc-windows-msvc`/`i686-w64-mingw32` per `config/toolchain.json`), licence headers LGPL-2.1-or-later with notices. Files: new `sdk/win32/`, `scripts/win32/stubs.py`, `tests/host/win32/*`. Do not edit the kernel (f5-03) or `src/kernel/vm` (f3-20).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`), record links and decisions in the relevant `docs/design/` document (append dated subsections; never rewrite earlier text), then implement and test. Report `build/codex/f5-05-report.md`: findings, changes, host tests, what the lead must verify on QEMU and the T23.
