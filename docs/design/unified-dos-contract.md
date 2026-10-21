# Unified CiukiDOS: FreeDOS 2043 kernel contract

Status: cross-review approved with amendments; Amendment 1 applied, 2026-10-21,
directive f3-54. The lead decisions below are binding; the proposed DFV2 ROM
reservation still needs the lead's launch-time reservation. This is
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
A new reproducible build need not match this vendor binary: its compiler,
options and compression history are not fully recorded. Two clean builds
with our pinned inputs must match one another. Keep that distinction in evidence.

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

Boot has two phases, with **no live binding at device INIT**. Initially
FreeDOS uses its local FAT/block driver to read CONFIG.SYS and the Ciuki
character/control driver. At INIT the driver discovers DFV2, validates ROM
identity and registers only its own resident allocation and a deferred callback.
Registration is fork-local, not a native bind/event. It must not register
provisional kernel/SFT/CDS pointers, admit file requests,
mark drives redirected or acquire a native mount at this stage.

K `kernel/main.c:337` loads devices in `DoConfig(2)`, but `PostConfig()` then
extends the final SFT chain (`kernel/config.c:459–463`), replaces the CDS array
(`:465`), and `FsConfig()` reinitializes filesystem state (`main.c:348`).
`configDone()` can relocate kernel code for DOS=LOW (`config.c:483–506`);
`InitializeAllBPBs()` still runs afterward (`main.c:352`). Therefore the fork's
late callback is **after `init_kernel()` returns (`main.c:121`) and before
`DoInstall()` (`:128`)**, not merely before AUTOEXEC or the shell. INSTALL
programs are application code and must see the active filesystem too.

At that late boundary f3-56 resolves the final relocated entry/code ranges,
data/SDA, complete SFT chain, replacement CDS, root PSP/JFT, devices and driver
callback anew. No INIT-time kernel pointer or return CS is authority. f3-57
performs filesystem activation at this same boundary, in one transaction:

1. Revalidate DFV2, the fork identity and the complete final registered
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
   bound-drive failure. An explicitly isolated fixture can stay local after
   rollback; `kernel=unified` reports a contained activation failure before
   INSTALL/shell execution. It never silently boots an unbound fallback.

H56-02 and H57-01 must execute the real initialization transitions with final
FILES/SFT allocation, LASTDRIVE/CDS replacement and DOS=LOW code relocation,
poison the provisional addresses, then prove only final addresses are admitted.
Fault each late registration/publication step; no stale callback, half-bound
drive or native resource survives failure.

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
| 1B/1C/36 | 110C through `DosGetFree`, `kernel/dosfns.c:755–790`; legacy allocation/space results. |
| 7303 | Separately, `DosGetExtFree` reaches 110C at `kernel/dosfns.c:885–887`; extended-space structure and bounds are not the 1B/1C/36 interface. |
| 5C | 110A for **both** lock and unlock, parameter block carries unlock; `kernel/dosfns.c:1276,1399–1413`, `kernel/int2f.asm:359` |
| 42 | 1121 **only** selected share modes and origin=EOF; `kernel/dosfns.c:310–350`; other seeks update guest SFT directly |
| Exit | 1122 and, for non-TSR child exit, 111D; `kernel/task.c:556–588`; insufficient by themselves for all PSP transitions |
| Path qualification | 1123 wrapper `kernel/int2f.asm:462`; not a replacement for shared CDS/alias policy |

### Exact interception sites

Choose **interception at each already-marshalled callout site**, immediately
before its original INT 2Fh. `call_int2f` is a dispatcher label, not a single
interrupt seam. The fork shim selects a registered local backend only for an
owned request; otherwise it restores the complete input and executes that
site's original interrupt once. The original wrapper still converts the local
backend's CF/register result into its C return. No additional INT2F frame is
invented for a handled call. All coordinates below are pristine K2043.

| `kernel/int2f.asm` site | Input/frame variant and original completion |
| --- | --- |
| :355 `remote_getfattr` / `remote_lseek` | No pushed argument word. 110F returns attributes in AX; 1121 loads signed offset CX:DX from the near argument, ES:DI is SFT, returns DX:AX. CF error negates AX. |
| :364 `remote_lock_unlock` | No pushed argument word; DS:DX parameter block, BL=unlock byte from block+8, CX=1, ES:DI=SFT. Success returns 0; failure zeroes AH then negates AX. |
| :407 `int2f_call` | Extra pushed argument WORD (mode/attributes for open/create/extended open), ES:DI points to the call-specific object; CX=0 and CF=0 on entry. Pop argument into BX; success AX=CX, error AX=-AX. |
| :432 `remote_print_doredir` | Extra argument WORD; registers loaded from `lregs`, DS can differ from SS. Pop word and restore DS=SS; return CF as 0/-1 or negative AX. Printer calls stay unowned and chain once. |
| :443 `remote_getfree` | Extra WORD is a near output pointer, ES:DI=CDS. Pop pointer into DI and copy AX/BX/CX/DX to four checked words on success; C return 0/-1. |
| :454 `remote_rw` | No extra argument WORD; ES:DI=SFT, CX=requested byte count, SDA DTA=buffer. Success DX:AX=zero-extended returned CX; error sign-extends -AX into DX:AX. |
| :471 `int2f_restore_ds` | No extra argument WORD. 1122 supplies DS=current PSP; 1123 supplies DS:SI from the far qualification source. Restore DS=SS after completion and return 0/-1 from CF. |

Every variant retains saved BP/SI/DI and the near return address below its
optional argument word; the far gate adds a four-byte return address, not an
application interrupt frame. Preserve live wrapper CF on return, and patch
application FLAGS only through the registered kernel return path. H56-03 must
publish exact stack diagrams for all seven sites, direct INT21, lifecycle and
critical continuation, and execute success/error/unowned-chain/pending fixtures
for each applicable variant, including both 355 and 471 branches. Do not call
the v1 direct-INT21 gate with any of these frames.

The following require kernel-side participation in addition to the redirector:

| Gap | Required hook/behavior |
| --- | --- |
| 42 SET/CUR and non-shared EOF | Synchronize authoritative VFS description position and signed error semantics for `SftSeek`; FCB seeks use it too. No private cached position owner. |
| 45/46, internal `CloneHandle`, EXEC inheritance | Native reference reservation/commit/rollback before guest JFT/SFT publication; `dosfns.c:632–685`, `task.c:215–265`. |
| 5700/01 | Read/set timestamps via common native description; upstream only reads/marks SFT, `dosfns.c:1087–1117`. Immediate query and reopen must agree. |
| FCB close 10h | Checked close-metadata hook before `fcbfns.c:609–613`; caller FCB size/date/time are explicit inputs, distinct from handle close and untrusted SFT edits. See below. |
| 0D, 0E/19, 1A/2F, 47, 59, 60, 67 | Flush mapped drives; synchronize default drive, DTA, CDS/cwd, persistent error, qualification/aliases and expanded JFT. `inthndlr.c:582–589,648–656,750,1036,1241,1364,1490`; `dosfns.c:916,1183`. |
| 44 file/drive queries | `kernel/ioctl.c:81–216` mostly answers from SFT/CDS or calls devices; use mapped-drive policy and real description type. Do not expose an emulated host drive as a raw writable device. |
| 26/55/50/51/62, 4B/4C/31, INT20/27, failed EXEC, VM stop | Kernel lifecycle hooks preserve immutable ancestry and notify exactly once. 1122 alone is too late/incomplete. |
| 34/52/5D06 and 2F/1203,1216,1220,1226,122D | Keep real FreeDOS internals visible and synchronize bound descriptions; never return the old compact SDA as FreeDOS's SDA. Internal 1226 reaches `DosOpen`, `inthndlr.c:2018`. |
| FCB 0F–17/21–24/27–29 and 38/65/66 NLS | Preserve f3-50 behavior using the upstream FCB/SFT engine plus checked adapters and the same CP437 tables. No duplicate private FCB FAT owner. |

5A's uniqueness loop calls `DosOpen` and so eventually reaches create, but
admission, failure rollback and writable-name bounds must satisfy DF5. The
existence of that callout alone does not prove atomic temp creation.

### Checked FCB-close metadata

K `kernel/fcbfns.c:609–613` copies `fcb_fsize` to `sft_size`, omits
`dos_merge_file_changes` for `SFT_FSHARED`, then sets the caller's packed
date/time before close. 1106 alone cannot express that metadata intent.
Introduce an authenticated kernel-origin **INT21/10h close-metadata hook**
before those assignments for managed FCBs. Pass checked standard/extended FCB
fields, validated SFT token/generation, owning PSP and explicit proposed u32
size/u16 date/u16 time. Never infer intent by comparing an application's
writes to exposed SFT memory. Handle 3Eh/commit retain their own semantics;
`FcbCloseAll` (`:621–629`) is cleanup, not an arbitrary caller-FCB proposal.

The semantic oracle is the pristine reference with SHARE enabled. For a valid
single writable FCB close, the proposal is the caller's byte size and packed
date/time, not rounded record count, current wall-clock time or a guessed
maximum of sizes. Reproduce the reference's ordered metadata/SHARE merging
(`fatfs.c:330–398`, `:214–232`, `fatdir.c:232–268`), including a second live
description changing size/date before close. Freeze appended reference cases
for shrink, grow, unchanged/stale FCB, concurrent handle/FCB writers in both
close orders, and attribute-read-only files. Record exact AL/59h, visible
metadata, reopened bytes/size/date and other open descriptions' results; source
inspection alone does not claim their runtime results. Reproduce any reference
metadata-only behavior on attribute-read-only files without granting data-write
authority. A read-only mounted D:/E: view remains non-writable (access denied 5).

Reserve/check all output ranges and native mutation resources before commit.
Serialize the metadata operation against other descriptions, apply its defined
merge once, close/decrement once, then publish checked guest state. A successful
close marks the FCB slot FF and returns AL=00; an already closed FF slot succeeds
without a second operation. Failure returns AL=FF and persistent 59h error;
failed precommit publication leaves the FCB open and authoritative metadata,
guest outputs and reference counts unchanged. No fallible guest copy may remain
after native commit. Provider faults/cancel/stop must not expose half a size/date
change or close twice; retry/reopen and ledgers are acceptance evidence.

FCB arithmetic hardening explicitly includes `kernel/fcbfns.c:252–271`: stop
on a negative `DosRWSft` result before unsigned count/record arithmetic, and
validate record size before the modulus at :268. Preserve f3-50 status/padding,
segment-wrap and checked multiplication/addition rules; test zero record size
and injected negative/partial transfers, with no DTA or FCB scribble.

### Redirected-drive classification matrix

Managed C: (private writable FAT16 view), D: (read-only system view), and E:
(present removable view, policy-controlled read/write) use `CDSNETWDRV` with
the IFS `CDSPHYSDRV` bit. They do not expose a writable DOS DPB. In pristine
K2043 that network bit makes `get_dpb()` NULL (`fatfs.c:86–93`), rejects
`GetDriveDPB` (`dosfns.c:129–151`) and most block IOCTLs (`ioctl.c:187–190`).
The following freezes **post-activation** behavior, with valid parameters and
a present drive; bootstrap local drives retain upstream behavior.

| API | Managed C: | Managed D: | Managed E: present | Route/result |
| --- | --- | --- | --- | --- |
| 32h (also 1Fh default) | Fail | Fail | Fail | No DPB: AL=FF, CF preserved, DS:BX unchanged, persistent error 000F. `inthndlr.c:809–844`. |
| 4400h handle information | Native | Native | Native | CF=0, AX=DX=validated SFT flags: regular file has bit15 set, bit7 clear, low drive index C=2/D=3/E=4, actual clean/date flags; device handles keep their genuine device flags. Invalid handle CF=1 AX=0006. |
| 4408h removable query | Fail | Fail | Fail | CF=1 AX=000F, preserving K2043's redirected-drive rejection, rather than fabricating a block driver. |
| 4409h remote drive | Native | Native | Native | CF=0 AX=0300 DX=1000 for the unsubstituted IFS drive, as `ioctl.c:195–206`; remote classification is a DOS routing fact, not a public redirector advertisement. |
| 440Ah remote handle | Native | Native | Native | CF=0 AX=0000 DX=validated SFT flags (bit15 set for managed regular files); actual device flags for devices; invalid handle CF=1 AX=0006. |
| 440Eh logical block map | Fail | Fail | Fail | CF=1 AX=000F; no synthetic block-device mapping. |
| 7302h extended DPB | Fail | Fail | Fail | CF=1 AX=000F; no local flush/BPB refresh or exposed mutable DPB. |
| 7303h extended free space | Native | Native | Native | Separate checked extended-space adapter, CF=0 AX=7303 (K2043 preserves input AX on success); version/size/reserved fields and capacity/free counts describe the logical view. `dosfns.c:885–887` reaches 110Ch, separately from 1B/1C/36. |
| 7304h set/format DPB | Fail | Fail | Fail | CF=1 AX=000F; no formatting/DPB mutation. |
| 7305h absolute transfer | Fail | Fail | Fail | Valid packet: CF=1 AX=020C, matching `dskxfer` rejection mapped by `inthndlr.c:389–397`; no transfer or buffer change. |
| INT25h / INT26h | Fail | Fail | Fail | CF=1 AX=0201 from NULL DPB (`blockio.c:376–379`); preserve the special extra FLAGS word on the caller's stack, no transfer/buffer change. |

All native results come from registered native identity/state, never caller
SFT tampering. For 7303 preserve the full extended structure and checked
products; do not reuse 36h's narrowing or assume a larger structure fits the
110C four-word result. 1Bh/1Ch/36h retain their separate legacy layouts and
110C adapters. Allocation/free queries remain useful on a read-only drive.
Drive-query invalid/unmapped/absent E: yields CF=1 AX=000F (32h uses AL=FF
and preserved CF instead); 7305 invalid DL=0/out-of-range yields 0207,
and INT25/26 invalid drive yields 0201. An existing open handle invalidated
by media removal follows DF5 generation errors, not a newly invented 4400
drive error. Preserve K2043 validation precedence: 7302/03/04 undersized CX
returns CF=1 AX=0018, 7305 invalid CX/SI returns 0057; checked bad pointers
use DF5 without memory access. Drive-zero/default numbering is API-specific.
These deliberate errors are not substitutes for success in any original
conformance case; a baseline conflict must be reported, never normalized.

Sources: K `kernel/inthndlr.c:203–399,1655–1734`, `kernel/ioctl.c:81–216`;
RBIL [32h](https://fd.lod.bz/rbil/interrup/dos_kernel/2132.html),
[4408h](https://fd.lod.bz/rbil/interrup/dos_kernel/214408.html),
[4409h](https://fd.lod.bz/rbil/interrup/dos_kernel/214409.html),
[440Ah](https://fd.lod.bz/rbil/interrup/dos_kernel/21440a.html),
[440Eh](https://fd.lod.bz/rbil/interrup/dos_kernel/21440e.html),
[7303h](https://fd.lod.bz/rbil/interrup/dos_kernel/217303.html).
H57-08 executes every cell, including E: absent/read-only/writable, devices,
invalid handles/drives/packets, both input CF states, guarded buffers and stack.

**BIOS raw access is distinct.** INT13 CHS/EDD still accesses the virtual
provider under existing policy, generation, serialization and metadata/cache
invalidation rules. It grants no access to native physical disks and cannot
reactivate local FAT. DOS absolute APIs above deliberately reject managed
drives even though the BIOS provider exists; warm file reads use neither path.

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
Keep v1 byte-for-byte for the selectable old guest. The lead selected a new
negotiated layout identity **DFV2**, with its own descriptor signature and
offsets; never parse it as the v1 compact record. Its gate/descriptor proposal
is specified in [f3-56](../directives/f3-56-unified-dos-binding-fcb-nls.md#dfv2-rom-and-discovery-proposal).
Only the lead reserves that ROM space and the proposed discovery/version and
capability assignments when launching f3-56; proposals are not live ABI.

The new descriptor/bind record must describe: personality/build identity;
actual kernel entry and data segments; explicit checked SDA fields and extent;
SFT block chain/entry format/admitted indices; CDS base/count/stride; live
PSP/DTA/default drive/InDOS/critical fields; driver-owned result and internal
stack; device headers; CP437 tables; supported direct and redirector entry
roles. Length/version tags, disjoint checked ranges, rollback, immutable
generation and unknown-field handling are mandatory. Own process ancestry
in native state or a separately registered side record, never SDA padding.

Preserve discovery AX=C148h/BX=4644h/CX=1/DX=564Dh and the v1 descriptor,
gates and tables byte-identically. Proposed DFV2 ROM space is F000:1C00–1FFF
in the existing protected F1 page: direct/file at 1C00, bind/lifecycle at 1C04,
critical continuation at 1C08, marshalled callout at 1C0C, descriptor at 1C10.
It is disjoint from BIOS vector stubs below 1000, EMS at 1000, mouse at 1020,
DPMI real stubs 1040–11FF, protected stubs 1200–13FF, and DFVM v1 1800–1B53.
f3-56 specifies exact gate bytes, descriptor offsets and reservation tests.
No new ROM page or conventional-memory reservation is needed.

**DFV2 reservations (f3-56 Amendment 1, lead, 2026-10-21).** These replace the
proposal order above; wire layout and diagrams are in
[f3-56-unified-binding.md](../validation/f3-56-unified-binding.md).

| Reserved item | Value | Reference |
| --- | --- | --- |
| Discovery | AX=C148h BX=4644h **CX=2** DX=564Dh → AX=0 CX=2 ES:DI=F000:1C10; v1 keeps CX=1 | `src/kernel/vm/dos_unified.c` `dos_unified_discover` |
| Descriptor / bind record identities, version | `DFV2` / `DFB2`, version 2, descriptor 48 bytes, call record 48 bytes, bind record D0h bytes | `src/kernel/vm/dos_unified.h` |
| Feature bits | 3Fh: direct, local 11xxh, lifecycle, critical continuation, two-phase late bind, single country-1/CP437 set | `dos_unified.h` `DFV2_FEATURES` |
| ROM window | F000:1C00–1C3F: 1C00 direct `CD FE CB D2`, 1C04 local 11xxh `CD FE CB A2`, 1C08 lifecycle/bind `CD FE CB E2`, 1C0C critical continuation `CD FE F4 C2`, 1C10 descriptor; 1C40–1FFF stays free | `dos_unified.c` `dos_unified_rom`, asserts in `dos_file_vm.c` |
| Role bytes | D2 / A2 / E2 / C2 | as above |
| Driver identity | device `CIUKIDF$`, control block `CIUKIDFC` v2 at header+12h, identity `CIUKIDF2 F3-56 1` | `third_party/freedos/unified/ciuki/fork/drivers/ciukidf.asm` |
| Kernel identity | `CIUKIDOS K2043 2` | `third_party/freedos/unified/ciuki/fork/kernel/ciukinit.c` |
| Events | 0 = BIND in both phases, distinguished by the bind record's phase field (phase 0: driver registration at INIT, no kernel pointer; phase 1: the final layout after `init_kernel()`); 1–7 as above; 8 issued only by the monitor | `dos_unified.c`, `ciukidf.asm` |
| Profile | `kernel=unified` (value 2) only, DOS=LOW required: the final bind refuses a kernel text or data range outside conventional memory | `dos_file_vm.c`, `dos_unified.c` |

No other number is reserved by f3-56.

f3-57 additions, pending the lead's review (private wire only; no lifecycle
selector, no public ABI; rationale in `docs/validation/f3-57-unified-file-channel.md`):

| Item | Value | Source |
| --- | --- | --- |
| Call status | 2 = CRITICAL (local role only): AX = INT 24h AH/AL, DI = cause, BP:SI = device; the driver raises it with INT 2Fh/1206h | `dos_unified.h` `DFV2_STATUS_CRITICAL`, `ciukidf.asm` |
| Retry word | call record +2Ah: 0, or 1 = Retry of the previous CRITICAL request (+2Ch–2Fh stay zero) | `dos_unified.c` `parse_call` |
| Fail result | 53h | `DFV2_FAIL_ON_INT24` |
| Control block | `CIUKIDFC` v3, 24 bytes; +16h = offset of the policy byte (1 = `/LOCAL`) | `ciukidf.asm`, `hdr/ciuki.h` |
| Identities | driver `CIUKIDF2 F3-57 1`, kernel `CIUKIDOS K2043 3` (supersede the f3-56 rows above) | `ciukidf.asm`, `ciukinit.c`, `dos_unified.c` |
| CDS publication | owned drives `C000h` (IFS) at event 0 phase 1 commit | `dos_unified_ifs.c` |

Reuse event selector **0** for the late BIND and **1–8** for existing meanings:
1 PSP_CREATE, 2 PSP_SELECT, 3 EXEC_BEGIN, 4 EXEC_COMMIT, 5 EXEC_ROLLBACK,
6 PROCESS_EXIT, 7 TSR, 8 VM_DESTROY. INIT discovery/driver registration does
not issue 0. f3-57 activation is part of 0's transaction, not a new event.
File operations (including checked INT21/10h metadata and 45h/46h reference
updates) use their DOS operation identity on the file/callout path and native
reserve/commit/rollback; never overload lifecycle 3/4/5 with file transactions.
No additional selector is requested now. If implementation proves an operation
cannot be represented, stop that interface change and ask with its missing
transition and evidence, not a speculative number allocation.

For a managed-drive call, each enumerated already-marshalled site invokes the
registered local backend using its own 11xx convention. The GPL guest driver
translates it into the DFV2 checked marshalled-callout ROM role.
Unowned calls continue to the original INT2F chain. The VMM accepts INT FE only at that role's
immutable published address for a live binding. It checks the registered
caller/frame, guest memory, PSP, SFT token and generation. CHAIN applies only
to unowned requests; owned drive failures return a DOS error. DONE patches
the appropriate saved FLAGS; PENDING suspends in the monitor and resumes
once. Publish separate stack diagrams for direct INT21, each 11xx site and
lifecycle calls in f3-56; the far-CALL's four bytes do not replace the wrapper
stack or original application interrupt frame. A handled local call has no
intervening INT2F frame. Unknown multiplex requests chain once untouched.

**Lead-confirmed fork-local IFS seam:** a globally installed network redirector
conflicts with the unchanged battery. The `multiplex`
probe in `tests/host/dos/cases.py:719–729` requires public 1100 to return AL=0;
RBIL requires FF for an installed redirector. Both cannot be true in the
same profile. The confirmed choice is a **fork-local IFS backend at the seven
marshalled callout sites**. Keep public
1100 absent and public 11xx chaining/error behavior unchanged; advertise the
local backend only through its negotiated DFVM capability. Do not intercept
arbitrary application's INT2F as a privileged request. This preserves the
preferred upstream marshalling and the unchanged test with per-site kernel
hooks before each original INT2F instruction. The public 1100h probe continues
returning AL=0; its existing assertion stays unchanged.
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
must support installers. Power requests reuse the existing scoped shell/VM
interface, as the lead decided; no new capability or host-power authority is
requested. Do not transplant bare-metal reset/APM writes into the native desktop.
Preserve any legacy-only
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

The lead has selected both assets and must acquire them under
`build/downloads/openwatcom/`, SHA-256 pinning them in `config/openwatcom.json`
using the schema in f3-55:

- [open-watcom-c-linux-1.9](https://github.com/open-watcom/open-watcom-1.9/releases/download/ow1.9/open-watcom-c-linux-1.9),
  83,959,748 bytes; the release lists MD5 `960fe6b5cf88769a42949f5fedf62827`,
  useful as a cross-check, **not** our SHA-256 pin.
- [open-watcom-1.9.0-src.tar.bz2](https://github.com/open-watcom/open-watcom-1.9/releases/download/ow1.9/open-watcom-1.9.0-src.tar.bz2)
  27,927,922 bytes; preserve source and license provenance. Verify asset
  availability/content before recording pins. Both hashes remain for the lead
  to fill; this documentation does not create the configuration file.

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
fresh source extraction write only `config.mak` as an external build input
and invoke `make all` in that extraction with WATCOM and PATH pointing to
the pinned tools. No production image build in implementer sandboxes.
Uncompressed output is a standard supported configuration and avoids a new
UPX pin; it does not reproduce the compressed release bytes. Pin environment,
locale/timezone, file mtimes, host compiler, tool versions and configuration;
compare two clean build outputs and source manifests under **different actual
compiler wall-clock calendar dates**, not just roots/mtimes. H55-02 records
the clock seen by the compiler for each run and a fixed source epoch; changing
file mtimes or environment variables alone is insufficient. A narrowly scoped
host clock fixture may provide the two dates without changing the system clock;
prove the compiler observes them with an unfrozen date/time control. Audit `__DATE__`,
`__TIME__`, `__TIMESTAMP__`, linker timestamps and source paths. If they alter
the kernel bytes, preserve the failing evidence and seek a documented minimal
build reproducibility patch; never label a patched build “unmodified”.

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
| Ciuki kernel/driver additions | New fork-specific implementation must be GPL-2.0-or-later. Existing GPL-2.0-only files cannot simply be relabeled. The owner has been asked for the grant; f3-56 and f3-58 do not start until the lead records that answer. f3-55's unmodified upstream work may start without it. |
| FreeCOM 0.85a | Pinned package says GPL-2.0; distribute the shell conservatively under GPL v2 and preserve actual file notices. Bundled SUPPL contains LGPL-2.0-or-later notices and separate texts (`F:suppl/src/copying{,.lb}`). Build the supplied mini-SUPPL from source, not an opaque .LIB. Include its source, modifications and relinking materials as required for the chosen distribution route. |
| SHARE 08/2006a | Separate reference-only COM, package LSM says GPL without a precise version grant. Preserve original binary/source/LSM rather than invent a later-version permission. Unified native sharing/locks belong to VMM; do not load a competing SHARE over them. |
| SYS FAT16 boot template | GPL-2.0-or-later; exact template `boot16.bin`, authors and `K:boot/boot.asm` source accompany images using it. Wrapper MBR/build glue is separately attributed. |
| Ciuki VMM/native FS/probes | Existing repository licenses, separately linked native binary/test executables; no FreeDOS implementation transplant. Shared ABI specifications are documented; native and guest code do not import one another's private structs. |
| OW1.9 / NASM / host compiler | Build-time tools retain their own terms and pins; audit linked runtime separately as above. NASM is BSD-2-Clause; existing SDK provenance remains authoritative. |
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
actual source publication URL; f3-54 creates no release offer or upload. The
owner has been asked to approve the GitHub repository plus a per-release
corresponding-source archive and hash. That answer is also a start gate for
f3-56/f3-58; no repository URL or permission is invented here. f3-55 may prepare
the upstream build/source packaging meanwhile; actual distribution still needs
the approved destination and independently fetched archive/hash.

## Transition and gates

Keep three identities separate: current old CiukiDOS guest; immutable pristine
FreeDOS reference with bridge disabled and SHARE enabled; new source-built
unified CiukiDOS guest. The new guest is opt-in until acceptance. Each gets
kernel/base/content-specific COW namespaces. Existing per-game choices and
base SHA/hash-cache behavior remain stable; never reuse an old overlay after
changing its filesystem/kernel identity. The lead reserves the exact existing
BOOT.CFG/DOS Box setting **`kernel=unified`** and **schema value 2** for the
per-game kernel choice. Preserve `ciukidos` and the pristine reference's current
value, default and stored meanings; extend the existing `kernel` setting rather
than inventing a separate guest option. Reject invalid values and missing
unified images locally. f3-59 alone migrates the default after all gates pass.

| Gate | Required closure |
| --- | --- |
| f3-55 | Lead pins both OW1.9 assets/configuration; upstream work may start before owner rights/source-destination answer. Two identical clean unmodified kernel builds across different compiler calendar dates, host boot/layout and `kernel=unified`/schema2 checks; lead boots beside old/pristine guests. |
| f3-56 | Owner rights/source-destination answer recorded and DFV2 ROM reserved; two-phase late binding/lifecycle, all seven callout frames, checked FCB-close metadata/arithmetic and NLS/H50 tests; v1 unchanged. |
| f3-57 | Every coverage/gap row and frozen drive matrix through one native owner; activation after init_kernel/before DoInstall, H49/H50 and FCB metadata matrices; unchanged f3-48 base cases pass at 100% against pristine reference. |
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
