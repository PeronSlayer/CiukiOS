# Jemm monitor research dependency

This directory preserves the unmodified upstream sources of
[Jemm](https://github.com/Baron-von-Riedesel/Jemm) at commit
`e96bb6bb80cbdc3e6585544db79a6b133a5566dd`. `UPSTREAM.json` records the
archive URL, SHA-256 and pinned JWasm/JWlink toolchain inputs.

Jemm is a native x86 virtual-8086 monitor and EMS/VCPI/VDS memory manager.
JLOAD loads protected-mode extension modules (JLMs). These components are
being evaluated for running original DOS programs in CiukiOS windows.
They are **not installed by the normal CiukiOS build**, and compiling them
does not prove a working windowed DOS session, DPMI integration or audio.

## License and provenance

The upstream manual describes JEMM386/JEMMEX as partly Artistic License.
`Artistic.txt` is copied verbatim; retain the original per-file notices in
the complete source archive. JLOAD's separate MIT notice is copied verbatim
to `JLOAD-LICENSE.txt`. Do not describe the whole package as Public Domain.

`source-e96bb6bb.tar.gz` is the pinned GitHub source archive, preserved without
modification. No executable binary is committed here. Each successful build
also places that source archive and both licenses beside its outputs.

## Rebuild

On Linux, install Python 3.12 or newer, GCC, GNU make, binutils and curl, then:

```sh
bash scripts/build_jemm_monitor.sh
```

The script verifies the Jemm source archive, downloads the exact toolchain
source archives, verifies their hashes and rebuilds both tools. It invokes
the unmodified upstream `Linux.mak` files to produce `JEMM386.EXE` and
`JLOAD.EXE`. Case-only symlinks resolve uppercase include filenames and the
upstream `JLoad32.bin`/`JLOAD32.bin` spelling difference on Linux. No source
or makefile bytes are patched.

Every invocation uses a fresh working directory under
`build/external/jemm-monitor/`. `CURRENT` contains the relative path of the
most recent successful output directory. That directory contains binaries,
licenses, the source archive and `manifest.json` with input/tool/output hashes,
host compiler identification and the complete build-log path. Existing outputs
are retained, including failed-build logs. No machine disk or OS image is written.

For another isolated output location and an offline repeat:

```sh
bash scripts/build_jemm_monitor.sh --output build/full/jemm-check
bash scripts/build_jemm_monitor.sh --output build/full/jemm-check --offline
```

`--output` must stay below this checkout's ignored `build/` directory.
An existing cached archive with a mismatching hash is rejected, without
silently replacing it. Offline mode requires both verified tool archives
already in that output directory's `downloads/` subdirectory. Build jobs
default to two and are limited to eight.

## Runtime requirements and unresolved integration

These are requirements read from upstream source/manuals, not claims that the
CiukiOS implementation satisfies them:

- JEMM386 needs a working external XMS host. Its initializer queries XMS free
  memory, allocates a block and locks it to obtain a physical address. Query
  success requires `BL=0`, including XMS 3 function `88h`. Allocation, lock,
  free and A20 behavior must remain correct while CiukiOS owns other buffers.
- Dynamic allocation expects the optional XMS handle-array interface. When
  unavailable, upstream disables that path; `NODYN` explicitly selects a
  preallocated pool. JEMMEX includes its own XMS provider and cannot simply
  replace CiukiOS's active XMS owner without a separate ownership design.
- Command-line `LOAD` is supported. Upstream warns that DOS does not adopt
  UMBs installed this way. A bounded diagnostic configuration can exclude
  the upper-memory area with `X=A000-FFFF`, use `NOEMS NODYN`, and explicitly
  limit the pool. These options alone do not establish hardware safety or
  successful runtime integration.
- JLOAD and Jemm versions must match. JLMs execute in ring 0, have no direct
  DOS/DPMI API, and must yield during lengthy work because interrupts are
  initially disabled.
- Upstream `Tools/JLOAD/JLOAD.txt`, section 3, explicitly states that Jemm
  does not provide multiple VMs or an integrated DPMI host. DPMI clients
  execute outside Jemm's context through an external VCPI client host.
  Consequently, installing JLM port traps alone does **not** virtualize the
  protected-mode VGA/audio access of Doom or other DOS extenders.

Window lifecycle, private video memory, VGA port/state emulation, focused
keyboard/mouse delivery, a shared DPMI context, interrupt scheduling and audio
device ownership all remain separate implementation and validation work.
Keep the existing full-screen launch paths unchanged until that work is proven.

## Explicit CiukiOS device-query adapter

The default build above remains unmodified. The qualified CiukiOS experiment
uses an explicit source patch:

```sh
bash scripts/build_jemm_monitor.sh --ciukios-device-query
bash scripts/build_vm_session.sh
```

[`jemm-ciukios-device-query.patch`](../../patches/jemm-ciukios-device-query.patch)
and [`jemm_device_query.inc`](../../src/vm/jemm_device_query.inc) adapt only the
Jemm/JLOAD identification query. They first try the ordinary DOS device open.
If DOS reports file-not-found, they traverse the actual registered DOS device
headers and issue the driver's standard request directly. CiukiOS currently
lacks a complete character-device open/IOCTL dispatcher; this narrow adapter
does not pretend to implement one. Requests validate device attributes,
request completion, transfer size and a bounded chain. The private handle
never enters the kernel's file-handle table.

The same explicit CiukiOS profile also applies
[`jemm-preserve-a20-input-registers.patch`](../../patches/jemm-preserve-a20-input-registers.patch).
The upstream emulated A20-port read used AH as scratch and returned the changed
full EAX to a guest `IN AL`. That violated the instruction's partial-register
semantics and changed CiukiOS's XMS function number during its A20 check. The
patch preserves bits 8–31 while replacing only AL. A real guest probe checks
seeded EAX, XMS queries, allocation/lock/unlock/free and restored accounting.

The manifest records both patch hashes and the helper hash. Only patched upstream
translation units have their CRLF line endings normalized before applying the
patches. The source archive and original licenses remain byte-for-byte intact;
the `modifications` field in `UPSTREAM.json` describes that archived input,
not the optional patched build. Patched and unpatched outputs have different
hashes and must not be mixed. The module also depends on the pinned Jemm page
table layout; rebuilding against another Jemm revision requires a new audit.

See the [implementation and qualification record](../../docs/vm-session-foundation-2026-09-26.md)
for the implemented scope, commands, evidence and remaining work.
