# Provisional Win32 census, milestone A

`fixtures.txt` is a real static census of the self-authored source corpus.
`period-binaries.txt` and `workloads.json` are explicitly absent-input stubs,
preserving the ten approved media pins from the contract. No original installer
or game was supplied in the usual workload directories in this worktree. Archive
metadata requests also failed; no installed entry point, imports, disc path or
distribution terms were invented. Quake II and unidentified InstallShield
samples remain proposed additional coverage. NSIS 2.0 is supplemental.

The lead needs all ten original archives, especially Uplink `hluplink.exe`, AoE
`MSAoE.exe`, StarCraft's shareware ZIP/CD contents, plus an identified original
Quake II demo if approved, original InstallShield sample bootstrap/child pairs,
and a source-built NSIS 2.0 sample/uninstaller. Keep them local under
`build/downloads/win32-workloads/`; inspect bundled distribution terms, verify
the recorded media SHA-1/size and record SHA-256 before running this census.
For ZIP/ISO/SFX/CAB inputs first identify/extract original PE children with host
tools or after installation. The parser accepts PE32 only; NE/DOS/PE32+ are
explicit refusals and require separate tools, not fabricated PE import lists.

```sh
nice -n 19 python3 scripts/win32/census.py path/to/original/setup.exe path/to/installed/game.exe
nice -n 19 python3 scripts/win32/census.py --json path/to/original/game.dll
```

Text output is deterministic, with one `FILE` / `SHA256` block per input:
`FORMAT`, `SUBSYSTEM` / `LINKER`, `STATIC DLL -> function, #ordinal`, `DELAY`,
`BOUND` timestamp and forwarder names, exported ordinals/names/forwarders,
allowlisted manifest identifiers, TLS metadata, heuristic warnings and
`DYNAMIC not-observed`. It never emits executable/resource/manifest payload
bytes. Metadata controls are escaped to prevent forged text records. XML
sidecars named `<image>.manifest` are inspected as metadata too. JSON includes
resource identifiers/language/size and relocation types, still without bytes.
The CLI exits 1 if any input fails, and continues to report later inputs.

Bound import timestamps are descriptive only; a bound IAT without its original
names cannot supply an API census. Packer names, RWX flags and high entropy are
heuristics, never proof of a packer or a game dependency. Dynamic imports,
opaque installer children, COM and codec selections need milestone B install/
play traces. Keep Wine-conformance test imports separate from game scope.
