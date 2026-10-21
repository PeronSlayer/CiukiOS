# Unified CiukiDOS: FreeDOS 2043 kernel contract

Status: proposed for lead cross-review, 2026-10-21, directive f3-54. This is
an implementation contract, not an implemented guest or runtime qualification.
The [CiukiDOS inventory](unified-dos-inventory-ciukidos.md) and
[FreeDOS inventory](unified-dos-inventory-freedos.md) are part of it.

The owner's [2026-10-21 decision](../../dev_diary/2026-10-21-03-decisione-unificazione-ciukidos-freedos.md)
supersedes the old prohibition on adopting FreeDOS. The product remains
**CiukiDOS**, with a source-built fork of the FreeDOS 1.3 kernel 2043 as its
base. FreeDOS is credited as upstream. This contract supersedes only the
guest implementation choice in DF1–DF6 of [dos-dpmi-contract.md](dos-dpmi-contract.md#dos-file-api-in-the-vmm).
Their filesystem semantics, native ownership, deadlines and performance
acceptance remain binding. The directive's `resource-management-win98.md`
is a stale filename; the resource contract is
[win98-class-resource-management.md](win98-class-resource-management.md).
That baseline contains D1 but no separate D2 heading. The historical D2
file-service objective is concretely specified by DF1–DF6 and the owner's
decision; no missing D2 text is assumed to contain additional requirements.
No NTVDM architecture is introduced.

## Source register and reproducible coordinates

All `K:path:line` citations below refer to the **literal bytes** in
`kernel.zip::SOURCE/KERNEL/SOURCES.ZIP`, not current upstream master.
`F:` denotes `freecom.zip::SOURCE/FREECOM/SOURCES.ZIP`. Line numbers count
CRLF as one line. Research extraction is under
`build/codex/f3-54-sources/{kernel,shell,share}/`; no source import occurs here.

| Source | Primary reference and use |
| --- | --- |
| K2043 | [Official 1.3 kernel package](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/kernel.zip), [upstream release](https://github.com/FDOS/kernel/releases/tag/ke2043). Package SHA-256 `1efdba830a473891bf4c5c81e1858d86c247a0c2a03f00d69b5f40969c442905`; inner source ZIP `a977497b7d1b2990dc50542cf1a8da2965079f74c37508d5894ecc02a4c8a9d5`. Authority for actual hook reachability and layouts. |
| F085a | [Official FreeCOM package](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/freecom.zip). Package `25fef0309e94e87b88b634452b780bd2821fb6d94b0d0e149ad97633ea7d6be7`; inner ZIP `cdd1702c137037393f14adad64d4c3fe5e87d7381d1260c1be12b74f65d8e087`. Batch and shell source evidence. |
| DOS reference | [Microsoft MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/), chapters 1, 2, 3.3, 4 and 5: process/device/FCB/NLS conventions. Its version predates 6Ch; use the later RBIL entry for extended open. |
| Redirector | RBIL release 61: [1100 installation](https://fd.lod.bz/rbil/interrup/network/2f1100.html), [1108 read](https://fd.lod.bz/rbil/interrup/network/2f1108.html), [1116 open](https://fd.lod.bz/rbil/interrup/network/2f1116.html), [1121 conditional EOF seek](https://fd.lod.bz/rbil/interrup/network/2f1121.html), [112E extended open](https://fd.lod.bz/rbil/interrup/network/2f112e.html). These are DOS callouts, not native VMM syscalls. |
| DOS internals | RBIL [SFT/List-of-Lists](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html), [SDA](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html), [sharing](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html), [extended errors](https://fd.lod.bz/rbil/interrup/dos_kernel/2159_bx0000.html), [6C00](https://fd.lod.bz/rbil/interrup/dos_kernel/216c00.html). |
| Windows 98 | Owner-provided Resource Kit, chapter 28, *File System Architecture*, `build/rskt/wrkc28_5.htm:18–85`; chapter 29 `wrkc29_2.htm:24`. It establishes IFSMgr arbitration and ring-0 filesystem drivers, not a claim that Windows used FreeDOS's 11xx wire protocol. The supplied book is research-only and is not copied into the repository or image. Public related [Microsoft Win95 DDK IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM) supplies interface context. |
| Compilers | [OW 1.9 release and Linux installer](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9), [OW 1.9 documentation](https://open-watcom.github.io/open-watcom-1.9/c_readme.html), [OW2 upstream](https://open-watcom.github.io/), [gcc-ia16 upstream](https://github.com/tkchia/gcc-ia16). Compare with K `makefile`, `config.m`, `mkfiles/{owlinux,watcom,gcc}.mak`, not compiler marketing claims alone. |

The existing unmodified reference `BIN/KERNL386.SYS` remains pinned at
`932c0c155701eddb7b902f7269a1b2ce31f5c82a6dc195172f2336d18a74e1fb`.
F3-55 Amendment 1 additionally requires a byte comparison to this pinned member
outside its located compile-date string. The candidate is **unmodified except
patch 0001 (reproducible date)**; two clean builds with pinned inputs must also
match one another. The vendor member is packed (UPX marker at offset 85) and
has no visible compile-date banner, whereas our configured output is raw.
The strict masked comparison therefore remains blocked pending a lead decision
on equivalent representations; no decompression, alternate pin or wider mask
is authorised. Two identical local builds alone cannot qualify the guest.

## Architecture and activation

One guest kernel binary, its DOS drivers and its shell run in the existing
private V86/DPMI address space. The CiukiOS VMM remains a separately linked
protected-mode binary. Communication is through the documented checked DFVM
ABI and emulated devices. No FreeDOS kernel object or internal header is
linked into the VMM, and no native address is passed to the guest.

Use the FreeDOS redirector path as the principal file-data channel, with
explicit kernel hooks for operations that never call it. A redirector alone
cannot satisfy DF2; the omissions below are observed in 2043 source. A
second early INT21 filesystem emulator is not the proposed solution.

Boot has two phases. Initially FreeDOS uses its local FAT/block driver to
read CONFIG.SYS and the Ciuki-specific binding driver. The driver installs
through CONFIG.SYS as a character/control driver; it is not a second block
filesystem. Before AUTOEXEC, the shell, or any application is loaded, a
kernel activation barrier must:

1. Discover and validate DFVM, the fork identity and the complete registered
   control areas. Refuse unsupported versions without exposing host volumes.
2. Finish/flush bootstrap local handles and buffers, retain only known device
   handles, and quiesce local C: filesystem access. Mount C: once through the
   existing f3-47 provider/cache/overlay and native FAT/VFS adapter.
3. Register/admit guest SFT slots, the root PSP/JFT, live SDA fields, CDS array,
   device headers and process hooks atomically. Convert the C: drive view to
   the redirected/IFS route; publish D: read-only and E: according to policy.
4. Publish the active binding only after both owners can unwind. After activation,
   COM/MZ/overlay loads, CONFIG shell loads and all FCB/handle I/O on managed
   drives use the VMM. No transparent local-FAT fallback is permitted on a
   bound-drive failure. Failure before publication leaves an isolated local
   guest; a required unified profile instead reports activation failure.

Bootstrap sector I/O is allowed and counted separately. Runtime raw guest
sector access still uses the same provider/generation and serialization;
it must invalidate mapped metadata. The fork's local FAT cache cannot remain
an independently active writer after handoff. Activation and rollback are
part of f3-57 acceptance, not assumed to work because a redirector TSR loads.

## INT21 to redirector coverage

The full source-backed table is in the FreeDOS inventory. These are actual
2043 paths, conditional on network/IFS CDS or `SFT_FSHARED`, not every drive.

| INT21 caller | 11xx callout and K2043 hook |
| --- | --- |
| 39/3A/3B | 1103/1101/1105; `kernel/dosfns.c:943,1251` |
| 3C/3D/5B/6C | 1117/1116/112E; `DosOpenSft`, `kernel/dosfns.c:500–574`; 5B sets the create-new attribute high byte |
| 3E/68/6A | 1106/1107 through `DosCloseSft`, `kernel/dosfns.c:687–701` |
| 3F/40, internal EXEC and FCB transfers | 1108/1109 through `DosRWSft`, `kernel/dosfns.c:231–245`; transfer buffer is the current SDA DTA during callout |
| 41/43/56 | 1113/110F/110E/1111; `kernel/dosfns.c:1120–1258` |
| 4E/4F | 111B/111C; `kernel/dosfns.c:1022,1082`; remote search drive marker controls continuation |
| 1B/1C/36 | 110C for redirected CDS; `kernel/dosfns.c:770,885` |
| 5C | 110A for **both** lock and unlock, parameter block carries unlock; `kernel/dosfns.c:1276,1399–1413`, `kernel/int2f.asm:359` |
| 42 | 1121 **only** selected share modes and origin=EOF; `kernel/dosfns.c:310–350`; other seeks update guest SFT directly |
| Exit | 1122 and, for non-TSR child exit, 111D; `kernel/task.c:556–588`; insufficient by themselves for all PSP transitions |
| Path qualification | 1123 wrapper `kernel/int2f.asm:462`; not a replacement for shared CDS/alias policy |

`kernel/int2f.asm:369–421,452–475` marshals registers, the stack word,
negative C errors and result CX. A 1108 completion changes CX, not the
original application's AX directly. Validate this exact stack boundary,
including SS=kernel DS expectations. Do not call the v1 direct-INT21 ROM
gate with a redirector frame: its original FLAGS offset is different.

The following require kernel-side participation in addition to the redirector:

| Gap | Required hook/behavior |
| --- | --- |
| 42 SET/CUR and non-shared EOF | Synchronize authoritative VFS description position and signed error semantics for `SftSeek`; FCB seeks use it too. No private cached position owner. |
| 45/46, internal `CloneHandle`, EXEC inheritance | Native reference reservation/commit/rollback before guest JFT/SFT publication; `dosfns.c:632–685`, `task.c:215–265`. |
| 5700/01 | Read/set timestamps via common native description; upstream only reads/marks SFT, `dosfns.c:1087–1117`. Immediate query and reopen must agree. |
| 0D, 0E/19, 1A/2F, 47, 59, 60, 67 | Flush mapped drives; synchronize default drive, DTA, CDS/cwd, persistent error, qualification/aliases and expanded JFT. `inthndlr.c:582–589,648–656,750,1036,1241,1364,1490`; `dosfns.c:916,1183`. |
| 44 file/drive queries | `kernel/ioctl.c:81–216` mostly answers from SFT/CDS or calls devices; use mapped-drive policy and real description type. Do not expose an emulated host drive as a raw writable device. |
| 26/55/50/51/62, 4B/4C/31, INT20/27, failed EXEC, VM stop | Kernel lifecycle hooks preserve immutable ancestry and notify exactly once. 1122 alone is too late/incomplete. |
| 34/52/5D06 and 2F/1203,1216,1220,1226,122D | Keep real FreeDOS internals visible and synchronize bound descriptions; never return the old compact SDA as FreeDOS's SDA. Internal 1226 reaches `DosOpen`, `inthndlr.c:2018`. |
| FCB 0F–17/21–24/27–29 and 38/65/66 NLS | Preserve f3-50 behavior using the upstream FCB/SFT engine plus checked adapters and the same CP437 tables. No duplicate private FCB FAT owner. |

5A's uniqueness loop calls `DosOpen` and so eventually reaches create, but
admission, failure rollback and writable-name bounds must satisfy DF5. The
existence of that callout alone does not prove atomic temp creation.

## Ownership and the DFVM adaptation

The native VMM registry remains authoritative for VFS identity, position,
sharing, locks, searches, pending requests and resource lifetime. FreeDOS
owns DOS process/memory execution and maintains genuine guest PSP/JFT/SFT,
SDA and CDS structures as the checked guest side of that registry. They
are neither trusted inputs to native pointers nor a disposable fake mirror.

| State | FreeDOS structure and agreement |
| --- | --- |
| JFT | `K:hdr/process.h`, `task.c:249–264`: initial 20 bytes; pointer/count support expansion. Preserve DF2's 232 handle ceiling and native admission bound. Only first 20 eligible handles inherited. Register guest allocation; don't substitute the old fixed JFT arena over FreeDOS-owned MCBs. |
| SFT | `K:hdr/sft.h:42–118`: DOS-compatible 3Bh entries, six-byte chain header. Register the actual admitted block(s), including device entries and FreeDOS's first SFT. Map each validated slot to one native description/generation. Native resources admitted before create/truncate or guest publication. Never sever the SFT chain by writing an all-FF next pointer. |
| Refcounts | Native authority plus transactional guest updates. 1116/1117/112E leave count management to DOS; `DosOpenSft` supplies an initial count only if zero. 1106's upstream caller expects the redirector to decrement count. Dup/clone and exit hooks must not count those changes a second time. |
| SDA | `K:kernel/kernel.asm:584–600`: critical=+00, InDOS=+01, error locus/code/action/class=+03/+04/+06/+07, DTA=+0C, selected PSP=+10, default drive=+16. Register fields individually and update only owned fields at declared safe points. FreeDOS uses the following bytes for its own state. |
| CDS | `K:hdr/cds.h:32–94`, `kernel/kernel.asm:388`: real VM-global cwd per drive and one default drive, shared across EXEC. `CDSNETWDRV` enables remote dispatch; its combination with `CDSPHYSDRV` denotes IFS. Pin returned 4409 flags/drive behavior to the agreed profile, not an invented network service. |
| Errors and waits | DF5 persistent 59h, total 2000 ms wall deadline, cancellation/generation checks and INT24 continuation apply through every new route. A redirector has no right to wait while holding storage locks. |

**v1 cannot be bound unchanged.** Today `src/kernel/vm/dos_file_hook.inc:46–48`
requires SDA, legacy entry, result and stack in the caller's single segment.
`dos_file.c:132–167` replaces a single SFT chain, rewrites a compact SDA and
stores Ciuki's owning PSP at SDA+1Ch. Those assumptions are unsafe for FreeDOS.
Keep v1 byte-for-byte for the selectable old guest. Negotiate an additional
layout/redirector capability under a lead-reserved version and feature bits.
No new numeric ABI is allocated in this contract.

The new descriptor/bind record must describe: personality/build identity;
actual kernel entry and data segments; explicit checked SDA fields and extent;
SFT block chain/entry format/admitted indices; CDS base/count/stride; live
PSP/DTA/default drive/InDOS/critical fields; driver-owned result and internal
stack; device headers; CP437 tables; supported direct and redirector entry
roles. Length/version tags, disjoint checked ranges, rollback, immutable
generation and unknown-field handling are mandatory. Own process ancestry
in native state or a separately registered side record, never SDA padding.

Preserve the existing discovery signature AX=C148h/BX=4644h/CX=1/DX=564Dh
and v1 behavior for its exact version. The lead must reserve the new version,
feature bits, redirector gate role/ROM slot, any additional event selectors
and runtime guest selector before f3-55/f3-56 implementation. New gates must
be allocated against the complete BIOS/DPMI/mouse/DFVM reservations; existing
F000:1800/1804/1808 and table region through 1B53 remain occupied.

For a managed-drive call the fork's `call_int2f`/remote-wrapper seam invokes
the registered local backend using the existing 11xx argument convention,
then the GPL guest driver translates it into the new checked ROM-gate role.
Unowned calls continue to the original INT2F chain. The VMM accepts INT FE only at that role's
immutable published address for a live binding. It checks the registered
caller/frame, guest memory, PSP, SFT token and generation. CHAIN applies only
to unowned requests; owned drive failures return a DOS error. DONE patches
the appropriate saved FLAGS; PENDING suspends in the monitor and resumes
once. Publish separate stack diagrams for direct INT21, 11xx and lifecycle
calls in f3-56; the far-CALL's four bytes cannot erase the intervening INT2F
and FreeDOS wrapper frames. Unknown multiplex requests chain once untouched.

**Research reconciliation requiring lead confirmation:** a globally installed
network redirector conflicts with the unchanged battery. The `multiplex`
probe in `tests/host/dos/cases.py:719–729` requires public 1100 to return AL=0;
RBIL requires FF for an installed redirector. Both cannot be true in the
same profile. The nearest compliant choice is a **fork-local IFS backend at
the redirector callout seam**, not a globally installed 11xx TSR. Keep public
1100 absent and public 11xx chaining/error behavior unchanged; advertise the
local backend only through its negotiated DFVM capability. Do not intercept
arbitrary application's INT2F as a privileged request. This preserves the
preferred upstream marshalling and the unchanged test while requiring a small
kernel hook before the original INT2F instruction. If the lead instead wants
a public redirector, that requires an explicit change to the binding battery
requirement before f3-57, not a hidden profile exception or fake absence.
Unsupported printer/5E/5F calls never report blanket success. Future F6
redirectors coexist through the original unowned chain.

## Features and shell decision

| Family | Disposition |
| --- | --- |
| DOS process/MCB/console/vectors, COM/MZ/overlay loader | Upstream-equivalent baseline; fork lifecycle hooks and regression tests retain known behavior. |
| Native file bridge, FCB edge semantics, CP437 NLS, DFVM | Port into fork and Ciuki binding driver as above. Preserve behavior, not the old sector parser. |
| `VER`, boot banner, shell name and prompt | CiukiDOS identity in fork + shell, with prominent FreeDOS/FreeCOM credits and retained license notices. Binary DOS version queries remain compatibility data. |
| Builtins, command lookup, history/completion, VM exit, launch and records | Port to the shell/driver; full mapping in CiukiDOS inventory. |
| HLT/INT28/2F1680 idle, virtual XMS/EMS/DPMI/mouse/video | Reuse upstream idle where equivalent and existing VMM services; retain missing yield behavior. Do not install competing memory/device owners. |
| Old loader ABI, CLFN sector entrypoints and old embedded desktop | Retire only from the unified runtime where there is a proved replacement; retain their old-guest/future-DOS sources. No restoration or build of the floppy profile. |

Choose a **source-built FreeCOM 0.85a fork** with the CiukiDOS identity and
ported Ciuki commands. `F:README.md:65–74`, `shell/cmdtable.c`, `cmd/{if,for,call,goto}.c`
and `shell/batch.c` provide an actual batch engine, environment, pipes and
redirection. The current canonical shell is not the embedded Stage1 UI:
`scripts/build_dos_image.py:238–244` installs the 113-line
`src/com/dosvm_shell.asm` wrapper and `shell.asm` with `COMMAND_COMPAT=1`.
That interpreter dispatches the small builtin list; the wrapper executes
AUTOEXEC lines through `/C` but supplies no general IF/FOR/CALL/GOTO engine.

This is source evidence for choosing a batch-capable foundation, not evidence
that all library installers already pass. f3-58 must run synthetic batch
matrices and the approved public DOS corpus; f3-59 closes every title and
installer with lead-owned runtime evidence. No private title list is read,
named or copied in this contract. Failed compatibility is repaired within the
contract, never by patching game binaries or weakening reference assertions.

Install the user shell as `SHELL.COM`; provide `COMMAND.COM` as a deterministic
copy/entry alias of the same shell for COMSPEC and DOS programs. Set COMSPEC
and PATH consistently. Preserve the Ciuki `/C`, `/K` and `/W` entry behavior
where the existing window runtime uses it; resolve any FreeCOM switch conflict
explicitly in the shell adapter. Keep the known prompt shape and command spellings;
replace its product label with `CiukiDOS SHELL` as the owner requires. Preserve English
messages and add the project tagline `Classic computing, one woof at a time.`
where the shell displays product identity. Credits must remain available
from VER and HELP; brand changes must not falsify AH30/3306 compatibility.
FreeCOM's own internal version/date and credits remain recorded in VER's
detailed output. Replace build date/time nondeterminism through documented
source changes in f3-58, not binary hex patches.

Preserve Ciuki PATH as a real environment variable initialized to the known
search directories, rather than the old read-only display. Preserve WHERE,
RUN, PWD, CLEAR, MOVE/RENAME aliases, queued REBOOT/SHUTDOWN/status/cancel,
and EXIT/QUIT's clean VM-window close semantics. FreeCOM PATH/SET behavior
must support installers. Do not transplant bare-metal reset/APM writes into
the native desktop: power requests use the existing scoped host interface
or fail explicitly until that interface is available. Preserve any legacy-only
behavior only in the old personality and identify it as such in tests.

## Memory and device boundaries

Do not raise any VM memory cap to make the new DOS fit. Existing VMM ownership
of low memory through 0010FFEF, A20/HMA, XMS, EMS, DPMI and pageable pool is
unchanged. Boot FreeDOS with DOS=LOW initially, matching the pinned reference;
do not add HIMEM, EMM386 or CWSDPMI. No HMA/UMB optimization is authorized
before conventional/upper/reserved memory is measured on the supported
profile and reviewed. BIOS VGA/ROM/EBDA reservations stay in force.

K `mkfiles/generic.mak:37` uses LOADSEG=0060; FreeDOS relocates its resident
data/code and allocates MCB-backed drivers/FreeCOM. It is **not** the old
CIUKIDOS header loaded at 0900 with runtime segment 0300 and F000-byte extent.
f3-55 must use the existing pinned FAT16 FreeDOS boot path, not prefix the
FreeDOS binary with the old header or pretend its relocation is unchanged.

| Region/owner | Required measurement and constraints |
| --- | --- |
| IVT/BDA, ROM and VGA/EBDA | Existing VMM mappings and protected synthetic ROM; validate all new gate reservations without consuming game RAM. |
| Conventional DOS | Report kernel resident paragraphs, buffers, SFT/CDS, driver+stack, shell resident/transient/environment, largest MCB and total free paragraphs before/after binding and EXEC. Compare old, pristine reference, unmodified rebuild and unified guest on the same profile. |
| HMA/UMB | Existing virtual host owns backing; DOS=LOW for initial acceptance. No duplicated XMS/EMS driver or unmeasured relocation. |
| Extended memory/DPMI | Same host services and admitted limits; no replacement extender. Quake go32/DJGPP and DOOM DOS/4GW are distinct regression paths. |
| File cache/overlay | Reuse f3-47 and global cache accounting; no second local runtime FAT writer or duplicate resident data cache. |

No physical-memory measurements are claimed by f3-54. Record these before
any later memory-policy proposal. Desktop/window rendering and virtual BIOS
belong to CiukiOS, not the FreeDOS fork. Preserve the exact approved Ciuki
portrait and original splash; no asset is regenerated by this transition.

## Toolchain decision

Choose **Open Watcom C/C++ 1.9 Linux host tools**, the fixed `ow1.9` release,
plus pinned NASM 3.02 already in the SDK, for the initial 386/FAT32-capable
kernel. `K:makefile:74–122`, `config.m:1–71`, `mkfiles/owlinux.mak:1–19`
and `watcom.mak:5–38,67–69` already implement this path with wmake, wcc,
wlink and wlib, packed structures and DOS target conventions. Host GCC builds
exeflat and utilities. Record/pin the host compiler and libc environment too.
The workstation exposes `/lib/ld-linux.so.2`; actual installer/tool execution
is an f3-55 host gate, not inferred from that path's existence.

Lead must acquire and SHA-256 pin exactly:

- [open-watcom-c-linux-1.9](https://github.com/open-watcom/open-watcom-1.9/releases/download/ow1.9/open-watcom-c-linux-1.9),
  83,959,748 bytes; the release lists MD5 `960fe6b5cf88769a42949f5fedf62827`,
  useful as a cross-check, **not** our SHA-256 pin.
- [open-watcom-1.9.0-src.tar.bz2](https://github.com/open-watcom/open-watcom-1.9/releases/download/ow1.9/open-watcom-1.9.0-src.tar.bz2)
  if redistributing its tools or runtime portions: preserve source and
  license provenance. Verify asset availability/content before recording pins.

### Build evidence still required

No toolchain download, installation or kernel build was performed in f3-54;
no SHA-256 is invented. f3-55 cannot accept a mutable latest/daily installer
or a locally installed unpinned compiler. OW2 supports Linux x64, but changing
code generation is unnecessary for this baseline. gcc-ia16 is a real supported
alternative (`K:mkfiles/gcc.mak`, not ordinary i686 GCC/DJGPP); it adds a
compiler/binutils/runtime pin chain and different object/link/flatten paths.
Turbo C variants are listed in `K:config.b:35–47`, but require DOS-host tools,
additional proprietary-license analysis and an emulation build dependency.
Existing JWasm/JWlink and DJGPP are not substitutes for this kernel's 16-bit
C calling conventions and NASM sources.

OW's license is **Sybase Open Watcom Public License 1.0 (Watcom-1.0)**, not
GPL. Compiler use is distinct from linking its runtime into the GPL guest.
For the kernel, the Linux top-level makefile deliberately creates an empty
`lib/libm.lib`; `K:kernel/kernel.asm:887–897` provides unsigned multiply/divide
helpers. f3-55 must verify the link map contains only the source-built kernel
and device library and no Watcom CRT/startup/math objects. Do not run the
DOS `lib/makefile` recipe that extracts `i4m` from Watcom `clibm.lib`.

FreeCOM uses a C runtime, so f3-58 must audit its actual link map and the
selected toolchain runtime terms. The upstream [runtime-license discussion](https://github.com/open-watcom/open-watcom-v2/discussions/382)
does not grant an exception; no generic GPL-compatible runtime exception is
assumed. If linked runtime rights cannot be established, use a separately
SHA-pinned gcc-ia16 build for FreeCOM (supported by `F:docs/compile.txt:8–20`)
with its applicable GCC runtime exception and libc terms recorded. Escalate
that concrete link result to the lead; it does not justify silently shipping
an unreviewed mixed-license binary. Kernel compiler selection remains OW1.9.

f3-55 baseline build configuration: `BUILDENV=linux`, `XCPU=386`, `XFAT=32`,
`LOADSEG=0x60`, pinned XNASM, `XUPX` empty/unset, no debug flags. From each
fresh source extraction apply the pinned patch below, write `config.mak` as
an external build input and invoke `make all` with WATCOM and PATH pointing to
the pinned tools. No production image build in implementer sandboxes.
Uncompressed output is a standard supported configuration. Amendment 2 pins
UPX solely to unpack a copy of the vendor kernel for comparison; the rebuild
stays uncompressed. It does not reproduce the compressed release bytes. Pin environment,
locale/timezone, file mtimes, host compiler, tool versions and configuration;
compare two clean build outputs and source manifests. The lead's OW1.9 probe
rejected forced-include redefinition of `__DATE__` with E1100. F3-55 Amendment 1
authorises **unmodified except patch 0001 (reproducible date)**, replacing both
uses (`K:hdr/version.h:48`, `sys/sys.c:989`) with `CIUKI_BUILD_DATE` and a
fallback to upstream `__DATE__`. No `__TIME__` or `__TIMESTAMP__` use occurs
in the pinned kernel archive. FreeCOM changes remain in f3-58.

The [patch rationale and SHA-256](../../third_party/freedos/unified/ciuki/patches/README.md)
and `third_party/freedos/unified/ciuki/build.json` pin the patch, changed source
hashes, `Oct 21 2026`/`00:00:00` and UTC epoch 1792540800 (the existing f3-55
build-input date). The compiler receives `CIUKI_BUILD_DATE`/`CIUKI_BUILD_TIME`
through the documented [OW1.9 preprocessor `-dNAME="text"` option](https://open-watcom.github.io/open-watcom-1.9/cguide.html#Preprocessor).
A native compiler probe checks those values before make. Preserve the pristine
archive/ledger and all source hashes before/after patching and compilation.
SYS uses `CFLAGST` instead of `ALLCFLAGS`, so the external `MAKE` command
supplies the same macros using the documented [wmake command-line macro
precedence](https://open-watcom.github.io/open-watcom-1.9/tools.html#Open_Watcom_Make_Command_Line_Format),
retaining SYS's original flags and Linux object suffix.
The checker names patch 0001 and the masked pristine comparison explicitly;
it compares the raw rebuild with the separately pinned UPX-unpacked vendor
representation, accepting only one banner in each binary, identical date offsets/lengths and
identical bytes everywhere outside its 11-byte date. Missing/ambiguous banners
or any other difference block qualification. The two-date commands use distinct
roots and source mtimes 946684800 and 1609459200, as Amendment 1 specifies;
actual wall-clock dates remain informational and can be the same. This checks
input-date independence, not execution under two actual host calendar dates.

## License and source delivery

The companion [per-file ledger](unified-dos-licenses.tsv) has 1,017 rows: all
983 files in the three pinned nested source archives, eight package binary/notice
members and 26 Ciuki input files selected for port/reuse. Each row lists origin,
raw SHA-256, license basis and use. It is a source
inventory, not an automatic license grant. Package-level assignments are
explicitly distinguished from file notices and unresolved imported binaries.
No FreeDOS/FreeCOM source is copied into tracked code by f3-54.

| Component | Terms and disposition |
| --- | --- |
| FreeDOS 2043 | GPL-2.0-or-later, as its notices state, including Pat Villani/DOS-C and subsequent contributors. Preserve `COPYING`, authors, file notices and modified-file dates. Keep Public Domain attributions in `sys/fdkrncfg.c` and credited portions; do not relabel those authors' work. |
| Ciuki kernel/driver additions | New fork-specific implementation must be GPL-2.0-or-later to retain the requested unified-kernel license. Existing Ciuki files marked GPL-2.0-only cannot simply be copied and relabeled. Lead must secure the relevant rights holder's dual/relicense grant for copied code, or implement the ABI/behavior anew under compatible terms. Until then port behavior, not GPL-only source text. |
| FreeCOM 0.85a | Pinned package says GPL-2.0; distribute the shell conservatively under GPL v2 and preserve actual file notices. Bundled SUPPL contains LGPL-2.0-or-later notices and separate texts (`F:suppl/src/copying{,.lb}`). Build the supplied mini-SUPPL from source, not an opaque .LIB. Include its source, modifications and relinking materials as required for the chosen distribution route. |
| SHARE 08/2006a | Separate reference-only COM, package LSM says GPL without a precise version grant. Preserve original binary/source/LSM rather than invent a later-version permission. Unified native sharing/locks belong to VMM; do not load a competing SHARE over them. |
| SYS FAT16 boot template | GPL-2.0-or-later; exact template `boot16.bin`, authors and `K:boot/boot.asm` source accompany images using it. Wrapper MBR/build glue is separately attributed. |
| Ciuki VMM/native FS/probes | Existing repository licenses, separately linked native binary/test executables; no FreeDOS implementation transplant. Shared ABI specifications are documented; native and guest code do not import one another's private structs. |
| OW1.9 / NASM / host compiler | Build-time tools retain their own terms and pins; audit linked runtime separately as above. NASM is BSD-2-Clause; existing SDK provenance remains authoritative. |
| UPX 5.2.1 | Build-time comparison tool only: GPL-2.0-or-later with the UPX exception, preserving upstream COPYING/LICENSE. `config/upx.json` pins the Linux archive, each extracted file and both vendor representations separately; no UPX code enters the raw rebuild. Direct unpack currently fails on the flattened vendor kernel; see the f3-55 validation record. |
| Portrait/Tango/book/manuals | Portrait is separately owned, Tango is Public Domain, book/manuals are research only. No relicense or redistribution of supplied Microsoft media. |

The separate-binary/address-space design avoids combining FreeDOS and native
CiukiOS kernel implementation code. It does not license copied code or waive
source obligations. Choose the GPLv2-compatible option for components when
required; do not assert that a GPL-only imported file gained “or later”.

For each distributable image f3-55 onward must install
`system/licenses/CIUKIDOS.TXT` (origin, build/fork identity, notices, modifications,
source path and hashes), `FDK-GPL2.TXT`, `FREECOM.TXT`, `SUPPL.TXT` and any
other shipped component's notice. Place the complete corresponding source
archive beside the released image (kernel fork, shell, driver, exact upstream
sources, patches and scripts/configuration needed to rebuild); publish its
SHA-256 and immutable downloadable URL in CIUKIDOS.TXT and release metadata.
The archive must actually be available at distribution time. A link to
upstream master alone is insufficient for modified code. Source for any
shipped boot code and linked support objects is included. Follow the existing
Tango practice of preserving source archive, exact notice and credits, without
claiming Tango's license applies to CiukiDOS. Lead selects and records the
actual source publication URL; f3-54 creates no release offer or upload.

## Transition and gates

Keep three identities separate: current old CiukiDOS guest; immutable pristine
FreeDOS reference with bridge disabled and SHARE enabled; new source-built
unified CiukiDOS guest. The new guest is opt-in until acceptance. Each gets
kernel/base/content-specific COW namespaces. Existing per-game choices and
base SHA/hash-cache behavior remain stable; never reuse an old overlay after
changing its filesystem/kernel identity. No third public kernel value or
wire selector is assigned without lead reservation.

| Gate | Required closure |
| --- | --- |
| f3-55 | Pinned source+toolchain, complete licenses, two identical clean kernel builds unmodified except patch 0001 (reproducible date), strict masked pristine comparison; host boot/layout checks; lead boots new guest beside old and pristine reference on one canonical image. |
| f3-56 | New negotiated binding and lifecycle; spoof/old/invalid/rollback tests; FCB/NLS retain H50 behavior; no unsafe v1 aliasing or new memory policy. |
| f3-57 | Every coverage/gap row implemented through one native owner, activation barrier, H49/H50 host matrices; unchanged f3-48 base cases pass at 100% against pristine reference with candidate bridge on. |
| f3-58 | Source-built CiukiDOS FreeCOM shell, all commands, synthetic installer/batch tests, launch/records/idle regression, credits and reproducible shell bytes. |
| f3-59 | Every approved DOS-library title and original installer completes the lead's full validation; 100% base battery plus appended new cases; DPMI/audio/input/video/exit, resource and T23 performance closure; only then default change and diary-recorded old-guest retirement. |

The f3-48 battery's existing programs, tags, expected assertions and reference
profile are not changed to accommodate the fork. Append new cases for new
behavior. Test the local IFS capability through DFVM and guarded callout
fixtures; public 1100 remains absent as above. Do not normalize away a baseline
failure. Any incompatible original probe is a lead decision to resolve before
implementation, not permission to edit that probe.

Acceptance rejects missing cases, identical unsupported errors, trace drops,
timeouts, stale generations and nonzero ledgers. Lead runs runtime comparisons;
Codex runs host suites sequentially at `nice -n 19`. Preserve script-defined
sanitizer settings; never add an environment override to turn off leak
detection in scripts which do not do so. No new `panic()` path is permitted.
All unexpected guest failures remain local DOS errors, rollback or contained
VM stops.

Retirement requires both 100% conformance and **every** library title, not
just the three public shareware anchors. Host-only evidence cannot close
either runtime gate. The lead records the source/image hashes, serial/disk
logs, all titles' private acceptance outside tracked files, the default
switch and the retirement diary entry. Preserve the future DOS-personality
source until explicitly retired for that purpose too. No intermediate hardware
image is requested by this contract; existing owner limits on QEMU remain.
