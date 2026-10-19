# f5-07A registry and service implementation

Primary-source research, 2026-10-19, before implementation:

- [Microsoft registry overview](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry)
  and [hives](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-hives):
  retain the API model, **not** NT hive filenames, binary format or profiles.
  CiukiOS owns SYSTEM.CRG and USER.CRG under the personality contract.
- [RegQueryValueExA](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexa):
  unnamed values are optional, size queries succeed, short buffers return 234
  with required bytes; stored bytes, including missing string terminators,
  must survive unchanged.
- [RegDeleteKeyA](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regdeletekeya):
  current documentation describes NT restrictions. The contract explicitly
  selects recursive 9x deletion. Its [period Microsoft reference](https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/regapi_1po9.htm)
  was requested but unavailable through the research browser; no new period
  experiment is claimed.
- [App Paths](https://learn.microsoft.com/en-us/windows/win32/shell/app-registration)
  and [Uninstall](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key):
  seed empty registration containers, with Ciuki-owned system/folder values.
- [VerQueryValueA](https://learn.microsoft.com/en-us/windows/win32/api/winver/nf-winver-verqueryvaluea):
  version blocks are aligned trees, with binary lengths in bytes and string
  lengths in characters. Bound traversal to the supplied root block.
- [LZRead](https://learn.microsoft.com/en-us/windows/win32/api/lzexpand/nf-lzexpand-lzread),
  [Wine 10.0 LZ implementation](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/kernel32/lzexpand.c)
  and [libmspack KWAJ decoder](https://raw.githubusercontent.com/kyz/libmspack/master/libmspack/mspack/kwajd.c):
  SZDD uses a 4096-byte space-filled LZSS history; KWAJ identifies its method
  and optional fields separately. Malformed or unsupported streams must fail.

Repository comparison: f5-04 already authenticates service acceptance by exact
path and supplies peer PIDs. Native libreg has different limits, types and a
different recovery format; it cannot be the Win32 hive store. The Wine audit
records version.dll's kernelbase dependencies and lz32's kernel32 forwarders;
neither module independently supplies this service's implementation. This
checkout has no production f5-06 kernel32 facade. New code therefore uses the
existing f5-04 channel and an explicit RPC protocol, with host adapters to test
the production parser, dispatcher and facade logic. Runtime qualification and
FAT power-loss durability remain separate from host evidence.

## Implementation decisions

`SYSTEM.CRG.0/.1` and `USER.CRG.0/.1` use a 40-byte little-endian header:
`CRG1`, version, hive number, u64 sequence, body byte count, body CRC-32,
header CRC-32, commit marker and a zero reserved word. The header CRC covers
the sequence and body checksum. Sections contain escaped UTF-8 full paths;
values contain escaped names, an eight-digit type and exact hexadecimal bytes.
Backslash, delimiters and control bytes use `\xHH`. Unknown types, duplicate
sections/values, malformed UTF-8, excess limits and inconsistent lengths fail
validation before a candidate replaces the other slot.

Commit writes the inactive slot with an unset marker, fsyncs it, patches the
marker and fsyncs again, then changes the in-memory active sequence. A failure
retains the dirty state and prior active slot. Recovery selects the highest
valid committed sequence; corruption of both slots fails closed instead of
silently replacing an installation with defaults. An fsync failure after the
marker may expose the new complete state after a crash; host tests allow old
or new, never a partial value. The native file fsync contract includes volume
metadata; the host adapter also syncs the directory.

Defaults are seeded only into new hives. Deleted defaults stay deleted across
restart; synthetic display keys are rebuilt from the active mode. The native
service owns serialization, the 2-second dirty deadline, explicit flush and
writer-disconnect flush. SIGTERM/SIGINT flush orderly service exit. A separate
two-slot `SERVICE.CRS` checkpoint preserves global/local atoms and the window
directory. Handle ranges are durably reserved before allocation; a new server
skips the prior range, so a stale registry handle cannot alias a new one.
Process enumeration on service startup removes positively identified dead
owners' local atoms/windows and preserves live owners' identities.

The API facade converts CP1252 A names/data and UTF-16 W names into the shared
UTF-8/canonical UTF-16LE representation. Binary, NONE, DWORD and the canonical
bytes of string values are preserved, including missing terminators. The
64-KiB storage limit is on canonical bytes; an ANSI string expanding beyond
that limit fails. Native Ciuki registry roots and types are unchanged.

`version` reads bounded PE32 resource trees and version-block children, and
keeps ANSI string results in caller-provided version buffers. The current
buffer registry tracks 64 version buffers for ANSI conversion (W queries do
not need it). Registry key class strings are accepted but returned empty;
security is the single-user compatibility model, with optional unsupported
security/service-control imports using W21 stubs. `lz32` uses the separately
pinned libmspack subset; all five KWAJ methods and SZDD have independent host
fixtures. Its full-buffer expansion cap is documented in
`sdk/win32/lz32/NOTICE.md`.

The builder compiles the native service, produces fresh default hives using
the same production parser/writer, and builds three PE facades. Each PE has a
version resource, a non-overlapping builtin base, a hash and per-census export
coverage. `scripts/build_image.py` consumes validated payloads; no image was
built for this directive. The suite case is authored with explicit pending
integration and is unrun.

Host evidence and remaining qualification are recorded in
`build/codex/f5-07a-report.md`. Resident memory at the contract's maximum hive
and client loads, FAT power-loss durability, production backend startup and
desktop restart supervision are not established by a host sanitizer pass.
