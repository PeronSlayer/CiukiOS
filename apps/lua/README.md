# Lua 5.4.8 for CiukiOS

This is the f2-07 application port. `build_lua.py` imports only local archives
whose SHA-256 matches `config/sdk-pins.json`, builds the unmodified sources
with `ciuki-cc`, and checks both `lua` and `luac` using the SDK's ELF and opcode
checker. There are no patches. Source, test, SDK, supplement and ELF hashes
are recorded in `build/apps/lua/manifest.json`; guest qualification is
`not_run` until the lead supplies QEMU evidence.

## Research and configuration

The [official Lua build instructions](https://www.lua.org/manual/5.4/readme.html)
and [official test procedure](https://www.lua.org/tests/) were checked against
the pinned 5.4.8 archive's `src/Makefile`, `luaconf.h`, `loslib.c` and the matching
test archive's `all.lua`. The website now describes newer releases; the pinned
archives define this port. Upstream's `generic` target selects portable C,
`LUA_COMPAT_5_3`, 64-bit `long long` integers and double numbers. It does not
enable `LUA_USE_C89`, Linux/POSIX extensions, readline or dynamic loading.
A compiled configuration probe checks these choices and that assertions remain
enabled. All compiler/linker target flags come from the SDK. The upstream
Makefile's archive commands use the SDK's pinned LLVM tools.

`_U=true` is the sole upstream test switch. `all.lua` itself selects `_soft`,
`_port`, `_nomsg` and disables the internal `T` interface. No additional skips
are supplied. Complete and internal modes are `excluded_by_contract`.
`final OK !!!` with exit zero is required in the guest; a host run is only
`host-reference`. Upstream source notices are retained, the exact `lua.h`
notice ships under `/system/licenses/lua-5.4.8.txt`, and the test archive's
notice ships unmodified in `all.lua`.

The [f2-18 files.lua audit](F2-18-REPORT.md) records the gate environment,
active file/time coverage, declared upstream exclusions and host evidence.

## Build and payloads

See [the SDK guide](../../docs/sdk.md) for the capped offline build commands.
The Lua recipe accepts `--source-archive`, `--tests-archive` and `--jobs 1|2`.
It never downloads and keeps temporary files under `build/apps/lua/`. It checks
the installed SDK and license hashes before building and verifies the untouched
upstream files afterwards. Unchanged inputs and matching output hashes reuse
the build; a digest mismatch is rejected before replacing existing outputs.

The image contains `/bin/lua`, `/bin/hello`, `/bin/libc_smoke`, the complete
`/system/tests/lua-5.4.8-tests/` directory (including `libs/P1/`), and the
separate `/system/tests/ciuki-f2.lua`. `/tmp` and `/home` are initially empty.
Kernel, boot options and license files also appear in the payload inventory.
`luac` is built and inspected for the SDK but is not an image payload in this
directive. Every file is listed by path, size and SHA-256 in
`build/f0/build-manifest.json`; T1 reads each file back from the final disk with
mtools. Both the 512 MiB image layout and the existing geometry/fsck checks
are retained.

## Supplement interface

The controller starts a separate run of `/bin/lua /system/tests/ciuki-f2.lua`
with `LC_ALL=C`, `TZ=UTC0`, `HOME=/home`, `TMPDIR=/tmp`, `PATH=/bin`. The script emits bounded
printable `case=<name> ok=1|0` lines for `file-roundtrip`, `allocation`,
`time-utc` and `console`, then exits zero only if all four succeed. It emits
no controller BEGIN/DATA/END framing or sequence numbers.

File byte indices are zero based. The final 65536-byte file has indices
8192–12287 replaced with `255 - (i % 251)`; every byte is checked after close
and reopen and again after rename. The case reports its FNV-1a-32 checksum;
the controller retains the separate script SHA-256 and console digest. The
allocation case keeps 4096 distinct 128-byte values live across full collection
in each of 100 cycles, checks every reference, releases them and collects again.
The UTC case checks exact epochs as well as both local and UTC table round trips.

The console case flushes each of these writes in order:

```text
stdout: ciuki-f2 stdout 1
stderr: ciuki-f2 stderr 2
stdout: ciuki-f2 stdout 3
stderr: ciuki-f2 stderr 4
```

The observer verifies the captured streams and exit status independently;
`ordered=1` is the script's assertion, not observer evidence.
