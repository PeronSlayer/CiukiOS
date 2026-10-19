# f3-48 DOS API inventory and conformance plan

Date: 2026-10-19. Contract/test infrastructure only; no new guest, QEMU or
hardware result is claimed. The source state is the `wt/f3-48` checkout of
main after e5b85ef7. References below were researched before test code changed.

## Evidence levels and corrections

`S` means a source-level access pattern, `B` an adjacent immediate-register
binary candidate, `K` a CiukiDOS/loader requirement. None means a runtime call
was observed. The exact runtime-used set, subfunctions and flags are **not yet
measured**. Existing `vm_perf_v` records aggregate vectors and retain only the
AX of the slowest dispatch (`dosvm_perf.inc:72`, `:137`), not paired results or
BX/CX/DX. f3-48 expressly forbids production changes and running QEMU. The
required production trace therefore belongs to f3-49 and collection to f3-51.
It would be false to fill this gap with invented traces or label every extender
translation-table entry as a game call.

The directive's introductory support ranges are not an implementation audit:
`floppy_stage1.asm:930–1127` has no dispatch for 0Fh/10h or 26h, nor 03h–05h.
29h parses FCB names and EXEC initializes FCBs, but this does not implement
FCB open/close. 26h creates a PSP; it is not FCB I/O. Quake 1.06 uses the
DJGPP/go32 stub, not DOS/4GW. Wolf3D is real mode and LZEXE-compressed.
No DOS/32A signature/member was found in these pinned public distributions.
That is a corpus statement, not proof about private game libraries.

## Primary source register

- R1: Microsoft [MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/), chapters 1 and 4: console/process/FCB/file interfaces, PSP and critical-error context. This edition stops before 6Ch; do not cite it for DOS4 extended open or a universal SFT layout.
- R2: Ralf Brown [INT21 index](https://fd.lod.bz/rbil/zint/index_21.html), release 61; [3Dh modes/matrices](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html), [5Ch](https://fd.lod.bz/rbil/interrup/dos_kernel/215c.html), [6C00h](https://fd.lod.bz/rbil/interrup/dos_kernel/216c00.html), [59h error domains](https://fd.lod.bz/rbil/interrup/dos_kernel/2159_bx0000.html). Decision: encode compatibility separately, retain full extended error and distinguish create/open/replace outcomes.
- R3: RBIL [52h SysVars/SFT](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html), [5D06h SDA](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html), [26h PSP](https://fd.lod.bz/rbil/interrup/dos_kernel/2126.html), [55h child PSP](https://fd.lod.bz/rbil/interrup/dos_kernel/2155.html). Decision: preserve the guest's versioned structures; never publish native pointers.
- R4: RBIL [FindFirst/DTA](https://fd.lod.bz/rbil/interrup/dos_kernel/214e.html) and [INT24](https://fd.lod.bz/rbil/interrup/dos_kernel/24.html). Decision: guarded 43-byte search buffers, copied DTA tokens and callback continuations outside storage locks.
- R5: RBIL [INT2F redirector installation](https://fd.lod.bz/rbil/interrup/network/2f1100.html), [DOS internal data segment](https://fd.lod.bz/rbil/interrup/dos_kernel/2f1203.html) and [INT2F index](https://fd.lod.bz/rbil/zint/index_2f.html) (11h, 12h, 1Ah). Decision: a local VMM file bridge does not advertise an installed network redirector. 12h is DOS-internal, not a public VFS syscall; 1A00h tests ANSI.SYS, unrelated to INT21/1Ah DTA.
- R6: Microsoft Windows 95 DDK [IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM), f0-42/P10, read in full. It documents separate global/context handles, file services and lock context. Decision: DOS handles are translated through owned descriptions, not confused with ring-0 global handles; the DOS DTA is not the LFN API's WIN32_FIND_DATA.
- R7: Microsoft [Q183173](https://www.betaarchive.com/wiki/Microsoft_KB_Archive/183173), original KB text mirrored: reopening files across volume locks can lose access flags; hook-chain forwarding must preserve reopen context. Microsoft [Q223282](https://helparchive.huntertur.net/document/108125) documents the Windows98 map-cache interaction with IFS API hooks. The accessible P10 topic does **not** specify full real-mode reflection or SFT synchronization. Relevant complete DDK hook/reflection topics were not retrievable here; their absence remains a source-access review item. No exact reconstruction of Microsoft's private dispatch internals is claimed.
- R8: [FreeDOS dosfns.c](https://github.com/FDOS/kernel/blob/master/kernel/dosfns.c): `DosOpenSft`, `DosOpen`, `DosCloseSft`, `DosRWSft`, `DosSeek`, `DosLockUnlock`; [fcbfns.c](https://github.com/FDOS/kernel/blob/master/kernel/fcbfns.c): `FcbReadWrite`, `FcbRandomIO`, `FcbRandomBlockIO`, `FcbGetFileSize`, `FcbOpen`; [inthndlr.c](https://github.com/FDOS/kernel/blob/master/kernel/inthndlr.c): `int21_service` dispatcher. Read as a behavioral cross-check only, no copied implementation. In the vendored source, `DosOpenSft` is at 517, `DosOpen` at 634, and `FcbReadWrite` at 230. Reference execution must pin the **1.3 release kernel**, not assume this master snapshot is that release.
- R8a: The official [FreeDOS 1.3 distribution report](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/report.html) identifies the release package set (kernel, FreeCOM and SHARE). f3-51 must pin the actual installed binaries from that release, not master.
- R9: Open Watcom [DOS/4GW INT21 translation chapter](https://open-watcom.github.io/open-watcom-1.9/pguide.html#DOS_4GW__Interrupt_21H_Functions). It is a supported translation list, not an executed-call list: DS:EDX/ES:EDI become real-mode pointers; large reads/writes are split below 64 KiB, errors are widened. Retain the original extender.
- R10: id Software [DOOM w_wad.c](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.c), `W_AddFile`/`W_ReadLump`, uses open/read/lseek for lump data; this Linux release is not proof of a DOS binary's exact INT21 trace. [Quake sys_dos.c](https://github.com/id-Software/Quake/blob/master/WinQuake/sys_dos.c), `Sys_FileOpenRead`, `Sys_FileRead`, `Sys_FileSeek`, `Sys_mkdir`, provides the DOS-side patterns. [Wolf3D ID_CA.C](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_CA.C), `CA_ReadFile`, `CA_WriteFile`, `CAL_SetupMapFile`, uses DOS handles and seeks; [WOLF3D.MAP](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WOLF3D.MAP) lists `_findfirst`/`_findnext` in the linked runtime. That proves linked support, not execution by this pinned binary. Source and shipped binary evidence remain separate.

## Reproducible binary audit

Run `nice -n 19 python3 -B tests/host/dos/inventory.py`. It uses the canonical
package reader's archive/member/license hashes, unpacks in memory, and emits
only hashes, signatures, exact offsets and byte patterns. No image, payload
copy or commercial material is generated. The checked result is
[f3-48-static-candidates.json](f3-48-static-candidates.json). It covers every
installed EXE/COM, including setup/network tools, rather than just the entry
executables. Adjacent MOV AH/AX + INT is deliberately a conservative **candidate
finder**, not a disassembler: it misses indirect/distant loads and cannot
establish instruction boundaries or unpack compressed bodies. EDh/FFh-looking
sites are unresolved candidates, not new file-API requirements.

| Program | Bytes | SHA-256 | Signature | Candidate sites |
| --- | --- | --- | --- | --- |
| `/WOLF3D/W3DHELP.EXE` | 33161 | `ea226934cbfa1d11060992c2201aabe6c5e2310a117225dd0388a068d5b23684` | LZ91 | 2 |
| `/WOLF3D/WOLF3D.EXE` | 109959 | `75bd63f1db75be77a9dfd317144fec2a86d2a409dd3b6847a251a2565b646280` | LZ91 | 7 |
| `/DOOM/DM.EXE` | 33640 | `2725df25c7536aa98a4e6282e61e17186777f54074e6fa56d9a38825dd1c7e09` | LZ91 | 2 |
| `/DOOM/DOOM.EXE` | 709905 | `b8020523561a5ad9706e009a52d61c578f37faafd85ac471962308406292ce27` | DOS/4GW | 166 |
| `/DOOM/DWANGO.EXE` | 65021 | `724486508cb23da93986c34336299972fbfaa2509a9ed18b0ec2832e07d600ca` | LZ91 | 5 |
| `/DOOM/IPXSETUP.EXE` | 18051 | `1e5eeb2e3b6f526d603f19ed08c00668e04bd2b7e6439c109b84b1c61d283d51` | LZ91 | 3 |
| `/DOOM/SERSETUP.EXE` | 20257 | `94cf23eac5fc43da942f9529e9ff4dd6dc5aab82fe348bd72eb7de971d9170f5` | LZ91 | 2 |
| `/DOOM/SETUP.EXE` | 45884 | `c9d328bff268854414feae5459870aac6f779e55a9cbf4568a83fef9b7be6e15` | LZ91 | 3 |
| `/QUAKE/CWSDPMI.EXE` | 25920 | `0a890815df6cbdc052915a856c16944cedec825267e7b6e453c351a0cdc695f2` | CWSDPMI | 32 | K DPB consumer | DL drive → DS:BX DPB / AL status | present 5361; private-image view | CiukiDOS mirror + VMM drive view | H50-02 |
| 36 |
| `/QUAKE/PDIPX.COM` | 26511 | `868c16e2a561ebc955ab873013a8b2a5e6d2cdd95d185915da51a9294dea15e9` | none | 19 |
| `/QUAKE/QLAUNCH.EXE` | 314880 | `83ce5209f15623fd0a00726e1e6cfee900829971e3294270b8efbca28ea1aac3` | none | 2 |
| `/QUAKE/QUAKE.EXE` | 404480 | `7cdb244122cc607cc6f162524bfcac60f6d11d8ca2ff07473d6cd962311161fb` | go32stub, CWSDPMI | 16 |

The main executables have 7 Wolf3D, 166 DOOM and 16 Quake candidate sites.
DOOM contains a bound DOS/4GW banner; Quake contains `go32stub`/`CWSDPMI`.
Original loaders and installed setup utilities were included. DOS/32A is absent
from this corpus; adding another owner-approved library requires another pin.

## Function inventory and register conventions

`W`, `D`, `Q` mean Wolf3D, DOOM, Quake. `B(D)` includes the bound loader;
`B(Q)` includes only QUAKE.EXE unless a helper is named. `K` is a required
kernel/contract service even without binary evidence. `present` means a path
exists, **not** conformance. `partial` identifies a demonstrated semantic gap.
Line numbers refer to this unchanged checkout's `floppy_stage1.asm` unless a
file is named. `CHAIN` is a genuine real-mode handoff; file operations are VMM
except FCB marshaling in f3-50. Test ids are `cases.py` ids, plus acceptance
ids H49/H50/H51 for the richer host-only and runtime cases in the directives.

All handle/path calls return CF clear on success and CF/AX on error unless
noted. Inputs below are 16-bit real-mode conventions; extender pointer
translation precedes this boundary. Preserved registers and output ranges are
part of DF1, not implementation-dependent scratch space.

| Function/subfunction | Used by / evidence | Input → output | Current status / line | Target | Conformance id |
| --- | --- | --- | --- | --- | --- |
| 00/20/31 | K process termination/TSR; B(CWSDPMI:31) | current PSP / DX resident paragraphs → no normal return | present dispatcher/process unwind | CiukiDOS + VMM cleanup | H50-01; lifecycle regression |
| 01/08 | B(D); K console | AH, queued input → AL character | present 982/986 | CHAIN | console_input |
| 02/09/0B | B(Q/D); K | DL character / DS:DX $ string / status → AL | present 974/988/992 | CHAIN | console |
| 0D | B(D), K | flush drives | partial 1257 (no durable native commit) | VMM | environment, H49-08 |
| 0E/19 | B(D), K | DL zero-based drive → AL count / AL current | present 5081/4943; private C only | VMM | drives |
| 0F/10 | K FCB | DS:DX FCB → AL 00/FF | absent 930–1127 | FCB adapter | fcb_io |
| 11/12/13/17 | K FCB | DS:DX FCB; DTA search → AL 00/FF | absent 930–1127 | FCB adapter | fcb_names |
| 14/15/16 | K FCB | DS:DX FCB, DTA records → AL status | absent 930–1127 | FCB adapter | fcb_io |
| 1A/2F | B(D/W), K | DS:DX DTA / get → ES:BX | present 4931/6315 | VMM state | find_dta |
| 1B/1C | K loader | DL one-based drive → AL,CX,DX,DS:BX | present 5333 | VMM view | environment |
| 21/22/23/24/27/28 | K FCB | DS:DX FCB, CX records for 27/28 → AL,CX/FCB | absent 930–1127 | FCB adapter | fcb_io; H50-03 |
| 25/35 | B(D/Q loaders), K | AL vector, DS:DX set → ES:BX get | present 5877/5905 | CHAIN + hook tracking | psp_vectors |
| 26 | K PSP | DX destination PSP segment | absent 930–1127 (55h is separate) | CiukiDOS | create_psp; H50-01 |
| 29 | B(D), K EXEC | AL parse flags, DS:SI,ES:DI FCB → AL,SI | present 7047 | CiukiDOS | fcb_io |
| 2A/2C | B(W:2C), K | date/time → CX,DX,AL | present 5174/5197 | CHAIN | environment |
| 30/33/34 | B(W/D/Q:30,D:33), K | AL version/control query → AL/BX/CX or ES:BX | present 5117/5286/5386 | CHAIN + SDA view | environment |
| 32 | K DPB consumer | DL drive → DS:BX DPB / AL status | present 5361; private-image view | CiukiDOS mirror + VMM drive view | H50-02 |
| 36 | B(W), B(CWSDPMI), K | DL drive → AX cluster sectors,BX free,CX bytes,DX total | present 5306; only private FAT geometry | VMM | drives |
| 38 | B(D), K | AL country, DS:DX buffer → BX country | present 5130 | CHAIN | environment |
| 39/3A/3B/47 | B(D:39/47), S(W/D/Q), K | DS:DX path / DL drive,DS:SI cwd buffer | present 10336/10542/6604/7001 | VMM/VFS | directories |
| 3C | B(D), S(W/D/Q) | CX attrs,DS:DX path → AX handle | present 8036; share semantics incomplete | VMM/VFS | handles |
| 3D | B(W/D), S(W/D/Q) | AL access bits0..2,share4..6,noinherit7,DS:DX → AX | partial 8823: masks flags, no SHARE claims | VMM/VFS | handles,deny_*,inherit_locks |
| 3E | B(D), S(W/D/Q) | BX handle → close | present 8910 | VMM/VFS or device | handles,invalid |
| 3F | B(W/D/Q), S(W/D/Q) | BX,CX bytes,DS:DX → AX bytes (0 EOF) | present 9114; sector path | VMM/VFS or device | handles,seek_gap |
| 40 | B(D), S(W/D/Q) | BX,CX bytes,DS:DX → AX; CX=0 resizes | present 9471; sector path | VMM/VFS or device | handles,seek_gap |
| 41 | B(D), S(W/D/Q) | DS:DX path → delete | present 10207 | VMM/VFS | handles |
| 42:00/01/02 | B(D), S(W/D/Q) | BX,AL origin,CX:DX signed delta → DX:AX position | present 10993 | VMM/VFS | seek_gap,seek_signed,duplicate |
| 43:00/01 | B(D), K | DS:DX,AL get/set,CX attrs → CX | present 7276 | VMM/VFS | attributes,directories |
| 44:00/06/07/0A | B(D:AH44), K device/file | BX handle,AL operation → DX info/AL ready | partial 6323: unknown subfunctions succeed | VMM regular / CHAIN device | ioctl; H49-07 |
| 44:08/09/0B/0D/0E | K mapped drives/SHARE | BL drive/CX-DX retry or control packet | partial 6323; only selected branches | VMM view / CHAIN device | H49-07 |
| 45/46 | K dup/redirection | BX source,CX force destination → AX dup handle | partial 1406–1454,11230: special 5→6 copy | VMM/VFS | duplicate,invalid |
| 4E/4F | S(W linked runtime), K DIR/search | DS:DX pattern,CX attrs, current DTA → 43-byte result / AX error | partial 7391/7441/7736: reserved bytes zero, global cursor, zero dates | VMM/VFS | find_dta,find_paths; H49-05 |
| 48/49/4A | B(W/D/Q loaders), K | BX paragraphs,ES block → AX segment/error,BX largest | present 13193/13300/13395 | CiukiDOS | memory; existing DPMI clients |
| 4B:00/03 | B(D/Q), K loader | DS:DX executable,ES:BX parameter block | present 2249; overlay 4432 | CiukiDOS + lifetime notifications | inherit_locks; H50-05 |
| 4C/4D | B(D/Q/helpers), K | AL exit → AL child code,AH type | present 1610/1664 | CiukiDOS + lifetime notifications | inherit_locks,critical_abort |
| 50/51/52/55/62 | B(D:50/52,Q:62 loader), K | BX select PSP / get BX; 52 ES:BX; 55 DX/SI | partial mirrors 1681/5394/5654 | CiukiDOS + VMM mirror | psp_vectors; H50-01 |
| 54/58:00..03 | B(Q/CWSDPMI:58), K | verify/memory strategy/link → AL/AX/BX | present 1719/5834 | CiukiDOS | environment; DPMI regression |
| 56 | K/S rename | DS:DX old,ES:DI new → success; cross-drive 17 | present 10744 | VMM/VFS | directories; H49-06 |
| 57:00/01 | B(D), K | BX handle,CX time,DX date → CX/DX | partial 1768–1856: clock read, ignored set | VMM/VFS | attributes |
| 59:BX=0 | K error/INT24 | BX=0 → AX,BH class,BL action,CH locus | partial 1862: class/action/locus zero | VMM SDA | extended_error,critical_* |
| 5A/5B | K contract | DS:DX path+13 writable bytes,CX attr → AX | absent 930–1127 | VMM/VFS | new_temp |
| 5C:00/01 | K SHARE | BX,CX:DX offset,SI:DI length → CF/AX | absent 930–1127 | VMM/common node locks | locks,inherit_locks; H49-04 |
| 5D:06 | K SDA | get SDA → DS:SI,CX always+DX partial sizes | partial 1739: snapshot from globals | CiukiDOS/VMM coherent SDA | psp_vectors |
| 60 | K canonical path | DS:SI input,ES:DI output → absolute DOS path | present 1868 | VMM drive view | truename |
| 65:01..07/20..23 | K NLS/FCB | AL operation,BX page,DX country,ES:DI/CX or DL/string | absent 930–1127 | CiukiDOS CP437 | nls; H50-04 |
| 66:01/02 | K codepage | BX/DX codepages → BX active,DX system | present 5062 | CiukiDOS CP437 | nls |
| 67 | K JFT | BX handle count → PSP JFT pointer/count | partial 1916: metadata cap without full table allocation | VMM+CiukiDOS allocator | exhaust; H49-02 |
| 68 | K durability | BX handle → commit/errors | partial 1944: valid-handle-only success | VMM/VFS | attributes,invalid |
| 6C:00 | K extended open | BX modes/flags,CX attrs,DX actions,DS:SI → AX,CX status | absent 930–1127 | VMM/VFS | extended_create/open/replace |
| 2F:1000 | K SHARE conventions | AX=1000 → AL FF installed | absent 23288 dispatcher | VMM advertise own SHARE service | multiplex |
| 2F:1100 / 11xx | K redirector review; no proven title calls | AX subfunction; 1100 → AL installed flag | not installed; chain 23398 | CHAIN; no new redirector | multiplex; H51-02 |
| 2F:1203 / 1216 / 1220 / 1226 / 122D | K DOS internal consumers; no proven title calls | DOS DS / BX SFT / BX JFT / open / error context | absent 23288 dispatcher | CiukiDOS checked mirror helpers | sft_jft; H50-02 |
| 2F:1A00 | K ANSI check; no proven title call | AX=1A00 → AL FF if ANSI installed | not installed, chains 23398 | CHAIN; never masquerade as DTA call | multiplex |
| 2F:1600 / 1680 / 1687 | B(D:1600/1687,Q:1680/1687), K | Windows version / idle / DPMI ES:DI entry | present 23414/23410; dpmi.c:740 | existing VMM/DPMI, CHAIN as applicable | multiplex; DPMI clients |
| 2F:1681 / 1682 / 1686 | B(D), B(PDIPX); unproved execution | Windows critical-section/context services | chains 23398; not implemented by CiukiDOS | retain chain until reachable trace; no file implementation | H51-02 |
| 2F:4300 / 4310 | B(W/D/CWSDPMI), K | XMS install AL=80 / entry ES:BX | present conditional kernel, VMM XMS entry | existing VMM XMS | multiplex; XMS regression |
| 2F:7A00 | B(IPXSETUP/PDIPX), optional network tools | IPX installation ES:DI, AL flag | not installed; F6 network scope | CHAIN absent, never false success | H51-02 |

## Counts and runtime closure

The table groups related functions, so do not count its rows as executed calls.
The JSON contains 12 executable members and 263 adjacent-byte candidate sites
(counted before interpretation). There are 189 sites in the three main title
executables. The following counts are reproducible, at AH granularity for
INT21 and AX for INT2F, for the **three main binaries only**. They do not count
source-inferred or K-only functions. `present/partial/absent` is dispatcher
coverage, not a FreeDOS pass count.

- INT21 static-candidate AH values: 40; dispatcher present 32, partial 6, absent 2. Absent candidates: EDh, FFh.
- Runtime-used count: **unknown**; runtime present/absent counts: **unknown**. No claim of zero absent calls.
- Unique candidate INT2F AX values in the main binaries: 1600h, 1680h, 1681h, 1682h, 1686h, 1687h, 4300h, 4310h.

EDh/FFh in DOOM may be non-code, private extender services or another encoding;
AH=FF is not an absent file call by definition. Before claiming an exact set,
f3-49 traces all INT21/2F entry/return boundaries and f3-51 runs boot/loader,
menu, demo/play, save/config write, directory search, and normal exit for each
pinned title. Associate every trace with executable/extender/image hashes.
Record AL flags and BX/CX/DX inputs **and results**, PSP, origin, sequence,
monitor/wall time, dropped count. A no-drop trace closes observed-path coverage;
it never proves unexecuted branches are absent. Reachability review of the
unpacked LZEXE body and extender tables remains necessary for static closure.

## Repository comparison and implementation boundaries

`dosvm.c:996` only recognizes the launch EXEC; it does not serve files.
The current reservation predicate must be found by symbol, not the directive's
stale line 1374 (which is port registration here). `dpmi.c:171` issues 62h and
its DOS-memory services issue 48h–4Ah; `dpmi.c:754` handles terminations and
critical vectors. These must keep chaining. `dosvm_disk.c:78` opens the private
image through VFS; `:112` retries a one-second gate wait indefinitely; `:130`
seeks then transfers. A bounded API must propagate deadline/cancellation into
that path rather than merely log elapsed time after it returns.

`vfs.h` has 64 handles/table, 128 descriptions, 64 nodes and 16 tables; it has
no byte locks or compatibility owner metadata. `vfs.c:350` applies symmetric
explicit share checks, `:430` transfers, `:451` seeks, `:580` searches and `:701`
renames. `:52` FIFO reservations wait without a deadline. `fdtable.c:55` creates
native descriptions and `:64` drops them under VFS identity locks; the bridge
must join those claims, not allocate another external-volume node registry.
The private FAT mount adapter and coherent raw/file serialization are new work.
Existing f3-47 caching must be integrated first; no second read-window/cache
implementation is authorized by this contract.

## Battery and outstanding evidence

See [tests/host/dos/README.md](../../tests/host/dos/README.md) for build, strict
comparison, fixture preparation, host model and the lead-only QEMU command.
The delivered COM probes cover the main handle/path/FCB/critical scenarios.
Host-only H49/H50 tests complete malformed guest memory, sharing matrices,
rollback, cancellation and native/DOS races where a COM cannot observe kernel
state. H51 closes actual game trace coverage and the timing targets.

No FreeDOS or CiukiDOS result table has been fabricated. Assembly/parser tests
are runnable now; production API replay awaits f3-49, personality tests await
f3-50 and boot/controller integration awaits f3-51. The standalone suite uses
schema 48 so the existing schema-1 runner cannot silently accept an empty case
list. It has its own executable runner; the lead may integrate it into the
standard runner in f3-51 without changing the result protocol.
