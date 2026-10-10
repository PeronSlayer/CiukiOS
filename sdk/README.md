# CiukiOS F2 SDK

This is the offline, static i686 C SDK specified by directive f2-06. The build
uses full newlib 4.5.0.20241231 and the exact public `ciuki/abi.h`; it does not
need a running kernel. See [the build and program guide](../docs/sdk.md).

`build_sdk.py` verifies the archive, immutable release commit pin, every patch,
and the full version and executable digest of the pinned tools before removing
or creating output. It accepts `--archive PATH` and `--jobs 1|2`, never fetches
sources, and builds from a clean `build/tools/ciuki-sdk/`. Its temporary files
stay inside that directory. Lua pins are retained for the later application
directive; this build neither imports nor builds Lua.

The equivalent newlib system port is `sdk/newlib/recipe.json` plus the hashed
`configure.host` patch, target header overlay and separate `libciuki.a` hooks.
It deliberately keeps `i686-unknown-elf`, selects no upstream `libc/sys` board
port, and configures only `newlib/`, excluding libgloss and libnosys. All hooks
translate only the ABI's unsigned error interval into the calling `_reent`.
Public syscall constants and generated raw stubs are derived from `abi.h`.

Library entry points and static linking can be checked on the host. The
`libc_smoke` ELF must subsequently be run through the integrated F2 controller;
a successful SDK build is not evidence of runtime qualification.

SDK-authored runtime code is MIT licensed; see [LICENSE](LICENSE). Imported
newlib retains its individual notices and `COPYING.NEWLIB`, with a compiled-file
inventory in the installed manifest. The copied ABI header retains its exact
GPL-2.0-only notice: the lead must resolve the public-header licensing boundary
before claiming that the entire installed header set is permissively licensed.
