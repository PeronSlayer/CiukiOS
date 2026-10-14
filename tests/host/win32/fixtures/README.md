# f5-02A self-authored PE32 fixtures

`corpus.json` is the source manifest for every case. `census` is a handwritten
static oracle, `rules` names the contract obligations, `runtime` is the expected
future guest behavior, and `evidence` explicitly says that execution is pending.
Malformed cases carry an exact first-check diagnostic and future Windows error
193. Source recipes are independent of the production parser.

The Python layout writer lives in `scripts/win32/fixtures.py`. It deliberately
constructs layouts that a linker would normalize or refuse. These are structural
fixtures, not execution tests. Compiler-produced cases have actual i386 entry
points: `abi.c`, the FS-linked `seh.s` chain, `packed.s`, the `tls.c` template and
callbacks, `provider.c` / `consumer.c` DllMain dependencies, and `resources.rc`.
The source DEF creates real import-by-name and NONAME ordinal import libraries
with llvm-dlltool. No CRT, SDK, Wine or original game bytes are required.
`kernel32.def` additionally supplies a minimal builtin import library; `-k`
retains stdcall COFF symbol decoration while removing the `@N` suffix from PE
import names. `builtin-import.c` is a future reference-count/DATAFILE probe.

Run from the repository root, one suite at a time:

```sh
nice -n 19 python3 scripts/win32/toolchain.py --fixtures-only
nice -n 19 python3 scripts/win32/fixtures.py --census docs/design/win32-census/fixtures.txt
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_fixtures.py -v
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_census.py -v
```

Only sources belong here. The current PE files, object files, resource objects,
import libraries and `hashes.json` live in `build/host/win32/current`. Test builds
use separate temporary directories there and remove them on success or failure.
The text census is version-controlled evidence for those reproducible hashes.
LLVM tool executable hashes pin this Linux installation; replacing a tool needs
lead requalification. LLVM's PE linker-version field is **14.0**, independently
of the executable tool version **23.1.1**. Synthetic images declare 23.1 by
construction; this field alone cannot identify the actual producing toolchain.

The fixture tests compare headers for all structurally valid cases with
llvm-readobj, imports/exports/resources/TLS on compiler fixtures, and i386
calling-convention and SEH instructions with llvm-objdump. ABSOLUTE padding in
relocations depends on the number of HIGHLOW sites, hence some compiler cases
have only relocation type 3. A repeated hash is forbidden: cases must differ
in the bytes that exercise their rule.

For subsequent guest harnesses: `provider.dll` supplies name FixtureValue and
ordinal 11; `consumer.exe` and `consumer-dll.dll` import both. Substitute
`provider-fail.dll` as `provider.dll` only in the failed-attach scenario. Use the
module mapping recorded in each `scenario` for cycle/depth tests. Do not execute
arbitrary synthetic fixtures whose entry does not implement their runtime
scenario; drive the loader/resolver with a mock module store first. Missing
DLL/export and relocation collision expectations require that store. Tests for
static TLS per thread, DllMain reference counts/detach/rollback, loader locking,
builtin search precedence, DATAFILE behavior and forwarder-depth resolution
still require the later loader/runtime; parsing alone cannot prove them.
