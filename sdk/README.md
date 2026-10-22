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

The F2-47 SDK also installs `libreg.a` and `<ciuki/registry.h>`. Link with
`ciuki-cc program.c -lreg -o program`. See the [registry contract](../docs/design/registry-contract.md)
for UTF-8 text storage, generations, recovery and the one-writer-per-root rule.
Hardware is an inventory snapshot and is always read-only. Existing desktop
and audio settings remain owned by their existing persistence code.

F7-01 installs `libgl.a`, `<GL/gl.h>` and `<ciuki/gl.h>` for the TinyGL
software subset. Link with `-lgl`. CiukiOS uses TinyGL by Fabrice Bellard,
Gek (DMHSW), and C-Chads; its acknowledgment licence is installed under
`sdk/libgl/LICENSE-TinyGL.txt`. See [libgl](libgl/README.md) for ownership,
supported calls and limitations.


F6-10 adds the IPv4 TCP/UDP socket headers and libc wrappers, `poll`, `select`,
strict IPv4 conversion and bounded `getaddrinfo` A lookup. These use native
calls 78–89; f2-100 adds privileged `ciuki_net_control` on call 90, with the
versioned records in `ciuki/abi.h`. See [network settings](../docs/design/network-settings.md).
Link with
the usual `libciuki.a`. See the [F6 subset and limits](../docs/design/posix-subset.md#f6-ipv4-source-api-extension-approved-2026-10-20)
for flags, descriptor readiness, non-pollable objects and deferred features.
