# FreeDOS 1.3 / kernel 2043 inventory

f3-54, 2026-10-21. `K:` and `F:` citations and exact source archive hashes
are defined in the [contract](unified-dos-contract.md#source-register-and-reproducible-coordinates).
The package is the [official kernel.zip](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/kernel.zip),
not `third_party/freedos/kernel-master`. This inventory used 181 kernel,
800 FreeCOM and 2 SHARE source-archive files. The [file ledger](unified-dos-licenses.tsv)
also records files not selected for runtime import. No build or boot is implied.

## Kernel entrypoints and reuse

| Family | Actual 2043 source | Reuse and delta |
| --- | --- | --- |
| INT21 fast/reentrant entries | K `kernel/inthndlr.c:77–196`, `kernel/entry.asm` | 25/33/35/50/51/62 can bypass the main switch. Hooks must include fast PSP selection and preserve vector services. |
| Console 01–0C | K `kernel/inthndlr.c:479–579`, `kernel/chario.c` | Full upstream base; retain virtual console and yield. Device behavior is separate from VFS file descriptions. |
| Drive/default/DTA | K `kernel/inthndlr.c:582–678,745–753`, `kernel/dosfns.c:916–955,1183` | Shared CDS/default drive plus per-process DTA hooks. |
| FCB family | K `kernel/inthndlr.c:591–625,682–723`, `kernel/fcbfns.c:91,223,278,333,354,390` | Upstream adapters call SFT operations; port f3-50 checks/observable semantics without a second FAT path. |
| Memory, PSP, EXEC/exit | K `kernel/memmgr.c`, `kernel/task.c:215–265,556–600,815–844`, `inthndlr.c:1042–1124` | Reuse MCB allocator and COM/MZ/overlay loader, add full transactional DFVM lifecycle. |
| Handle/path file API | K `kernel/inthndlr.c:911–1040,1126–1201,1251–1269,1364–1369,1490–1536` | Upstream entrypoints plus native IFS backend and explicit non-callout hooks below. |
| NLS | K `kernel/inthndlr.c:882–909,1431–1488`, `kernel/{nls.c,nlssupt.asm,nls_hc.asm,country.asm}` | Real upstream NLS, but unified baseline stays country1/CP437; unsupported switching must fail. Share tables with parser/alias service. |
| Internals | K `kernel/inthndlr.c:1742–2100`, `kernel/kernel.asm:374–400,584–600` | Preserve genuine FreeDOS structures, not the old compact mirror. |
| SHARE/locks | K `kernel/dosfns.c:582–590,1264–1288`, `kernel/int2f.asm:143` and package SHARE | Local upstream FAT uses separate SHARE; redirected files call backend lock service. Unified native VMM supplies SHARE semantics; pristine reference keeps its original SHARE.COM. |
| LFN/extended API | K `kernel/inthndlr.c:987–1009,1542–1584`, `kernel/lfnapi.c` | Do not equate 71xx dispatch with a complete Win9x LFN implementation. Inventory exact subfunctions before exposure; preserve existing 8.3 and alias contract. |
| Boot/build | K `boot/boot.asm`, `sys/sys.c`, `makefile:74–122`, `config.m`, `mkfiles/*` | Original FAT16 boot template plus Linux OW path; no Ciuki legacy ABI header. |

Undefined/gap functions in `inthndlr.c:636–645` return upstream's dummy
behavior; this does not qualify a newly exposed service. Cases 69/73/74 and
43FF extended-name subfunctions require individual tests if used. Upstream
availability is not a promise that all DOS 7 or Windows interfaces exist.

## Exact redirector callout coverage

Definitions are K `hdr/network.h:32–60`; dispatch wrappers are
`kernel/network.c:54–62` and `kernel/int2f.asm:347–475`.
The original kernel's INT2F handler does **not implement a filesystem
redirector**: `kernel/int2f.asm:91–105` returns absent for 1100 and error1
for unhandled 11xx. It calls an installed redirector for network/IFS CDS
and shared SFT entries. That distinction is essential.

| INT21 input/path | FreeDOS function and exact K site | Redirector request | Boundary caveat |
| --- | --- | --- | --- |
| 39 mkdir | `DosMkRmdir`, `kernel/dosfns.c:1251–1258` | 1103 | Only `truename` result IS_NETWORK. |
| 3A rmdir | same | 1101 | Missing/parent/current-directory errors retained. |
| 3B chdir | `DosChangeDir`, `kernel/dosfns.c:943–955` | 1105 | Kernel CDS publication also needs synchronization. |
| 3C create/truncate | `DosOpenSft`, `kernel/dosfns.c:500–574` | 1117 | ES:DI new SFT; stack attribute; native admission before mutation. |
| 3D open | same `:546–560` | 1116 | Stack mode; backend initializes file fields, DOS supplies initial refcount. |
| 5B create-new | `inthndlr.c:1256–1259`, `dosfns.c:543–546` | 1117 with attribute bit8 | Must be atomic/exclusive, not 3C truncation. |
| 6C00 | `inthndlr.c:1526–1536`, `dosfns.c:547–554` | 112E | SDA ext mode/attr/action, CX result copied into high word of DosOpen result. |
| 5A temp | `inthndlr.c:1251`, `DosMkTmp` helper | Repeated create via `DosOpen` | Not a dedicated temp callout; kernel uniqueness loop requires bounded/admission tests. |
| 3E close | `DosCloseSft`, `dosfns.c:687–701` | 1106 | Redirector performs refcount decrement; don't decrement twice. |
| 68 and 6A commit | same with commitonly | 1107 | Must forward native commit errors and honor autocommit. |
| 3F read | `DosRWSft`, `dosfns.c:231–245` | 1108 | ES:DI SFT, CX bytes, temporary SDA DTA points to data buffer; output CX bytes. |
| 40 write, including count0 | same | 1109 | CX=0 retains truncate/extend semantics; short write count is exact. |
| FCB sequential/random/block | `fcbfns.c:223–272` -> `SftSeek` and `DosRWSft` | 1108/1109 plus seek hook | No new hidden native pointer in FCB reserved fields. |
| EXEC COM/MZ/overlay reads | `task.c:815–844`, loader SFT reads/close sites | Open/read/close callouts | Activating C: before shell load is necessary, not just intercepting user INT21. |
| 42 SEEK_END, selected share modes | `SftSeek`, `dosfns.c:310–350` | 1121 | Only SFT_FSHARED and O_DENYREAD or O_DENYNONE; SET/CUR/other END are local. |
| 41 delete | `dosfns.c:1194–1204` | 1113 | Wildcards/path data live in kernel SDA. |
| 4300 get attr | `DosGetFattr`, `dosfns.c:1120–1143` | 110F | Root path can return directory attr locally; no callout in that branch. |
| 4301 set attr | `DosSetFattr`, `dosfns.c:1153–1165` | 110E | Forward exact access/errors. |
| 56 rename | `DosRename`, `dosfns.c:1211–1219` | 1111 | Both path buffers; cross-volume/native-share preflight remains required. |
| 4E FindFirst | `DosFindFirst`, `dosfns.c:1022` | 111B | ES:DI CDS; SDA search state; marshal to exact output DTA. |
| 4F FindNext | `DosFindNext`, `dosfns.c:1078–1084` | 111C | Temporary search DTA with bit7 in drive marks remote cursor. Copied DTA must remain valid. |
| 1B/1C/36 allocation | `dosfns.c:770–772,885–887` | 110C | Different INT21 return forms require kernel adapter, not one copied result frame. |
| 5C lock/unlock | `DosLockUnlock`, `dosfns.c:1276–1277,1399–1413` | 110A for both | `int2f.asm:359–368` selects lock vs unlock via BX/parameter; REM_UNLOCK definition is not a second observed C call site. |
| 60/qualification internals | `remote` qualify wrapper `int2f.asm:462–466` | 1123 where path code invokes it | Canonical paths/CDS/aliases still owned by common service. |
| 4C/INT20/31/INT27 return | `return_user`, `task.c:572–585` | 1122, then 111D only non-TSR child | Teardown plus clone/PSP/rollback hooks required; duplicate mass-close and per-handle close must not double-release. |
| 5D07–09 printer | `inthndlr.c:1288–1291` | 1125 | Not part of local file backend; fail/chain honestly. |
| 5E/5F network administration | `inthndlr.c:1316–1358` | 111F/111E | No new network administration advertised. |

The header also defines 1118/1119 (CDS-less create/find), 1120 (flush-all)
and other values. A definition is not proof that an INT21 path calls it.
f3-57 must account for every wrapper actually reached, and reject unsupported
owned operations without fabricating success. 0D's local flush path does
not become a global native commit merely because REM_FLUSHALL is defined.

## Functions which bypass the redirector

| Selectors / internal operation | Actual source | Necessary port |
| --- | --- | --- |
| 42 set/current/conditional EOF | `kernel/dosfns.c:310–350` | Checked shared position update and errors at SftSeek boundary. |
| 45/46 and CloneHandle | `kernel/dosfns.c:632–685` | SFT/JFT/native reference transaction, inheritance and failure rollback. |
| 5700/5701 | `kernel/dosfns.c:1087–1117` | Native metadata query/update, same open description and reopen outcome. |
| 0D flush | `kernel/inthndlr.c:582–585` | Explicit bound-volume flush in addition to local bootstrap buffers. |
| 0E/19 default drive | `inthndlr.c:587–589,648–650`, `dosfns.c:1183` | Shared drive-view validation/publication. |
| 1A/2F DTA | `inthndlr.c:653–656,750–753` | Change correct per-PSP native association and real SDA pointer. |
| 47 cwd | `kernel/dosfns.c:916–941` | Real shared CDS path; no independent per-PSP cwd snapshots. |
| 59 errors | `inthndlr.c:1241–1249` | Same persistent native error published in real SDA, INT24 ordering. |
| 67 expanded JFT | `inthndlr.c:1490–1492` | Native admission and guest MCB/JFT pointer/count publication. |
| 44 IOCTL | `kernel/ioctl.c:81–216` | SFT type and mapped-drive queries; character-device driver calls remain upstream. |
| PSP/EXEC/lifetime | `inthndlr.c:186–195,705–707,1169–1175`, `task.c:215–265,342–350,556–600` | All original lifecycle event meanings, including 26/55, selecting another PSP, rollback, TSR and destroy. |
| Internals 34/52/5D06, 2F12xx | `inthndlr.c:847–851,1147–1150,1280–1286,1798–1804,1914–1933,1974–1992,2018–2023,2091` | Real layout fields and checked private backend, internal open through same SFT owner. |
| NLS 38/65/66 | `inthndlr.c:882–909,1431–1488` | Table pointers, buffer limits, country/page failures, unified CP437. |
| 29 FCB name parse and 24 set random | `fcbfns.c:91–207,311–318` | Common NLS/record conventions; no file data request expected. |

## Layout evidence and activation risks

| Structure | Source-backed contents | Port rule |
| --- | --- | --- |
| PSP/JFT | K `hdr/process.h`, `task.c:249–264` initializes 20 entries with a far pointer and clones eligible parents | Use real guest allocation and dynamic pointer/count, reject forged indices, honor first-20/no-inherit. |
| SFT | K `hdr/sft.h:42–118`; refcount00, mode02, attr04, flags05, guest device/DPB07, dates0D/0F, size11, position15, name20, PSP31; 3Bh bytes | FreeDOS's FAT32 build uses some old disk fields differently but common DOS offsets remain. Preserve device entries, chain and real allocation. |
| SysVars | K `kernel/kernel.asm:379–389`, first SFT at `:508` | Register actual SFT head and CDS pointer; don't replace chain with old personality's 20-slot table. |
| SDA | K `kernel/kernel.asm:584–600` | Only explicit fields synchronized. Native old `dos_file.c:165` writes owning PSP at +1C, which is not an available FreeDOS extension slot. |
| CDS | K `hdr/cds.h:32–94`, path67, flags, DPB and redirector/IFS data | Kernel bookkeeping and native drive view agree. Bind after bootstrap, never leave local and native FAT writers active. |
| Frame | K `kernel/int2f.asm:369–421,452–475` | Internal C and INT2F marshalling differ from original INT21; distinct checked ROM role and return diagram. |

The old v1 validator requires several records in one code segment and
rewrites complete compact mirrors. FreeDOS relocation/data/driver segments
and genuine SDA cannot meet that layout safely. A separately negotiated
layout is required; weakening v1 checks globally is not a port.

## FreeCOM, toolchain and license inputs

| Input | Evidence / finding |
| --- | --- |
| FreeCOM version/identity | F `shell/ver.c:44–65` identifies 0.85a and FreeCom; uses `__DATE__` and `__TIME__`. Rebrand user identity with retained credits and make dates deterministic in f3-58. |
| Batch/installer engine | F `README.md:65–74`, `shell/batch.c`, `cmd/{call,for,goto,if}.c`, `shell/cmdtable.c:50` onward. Preferred over writing that engine into the smaller Ciuki interpreter. |
| Build | F `docs/compile.txt:8–20`, `build.sh`: Linux cross build with wc or gcc; mini-SUPPL sources included. |
| OW Linux kernel | K `makefile:74–122`, `mkfiles/owlinux.mak`; source provides `kernel/kernel.asm:887–897` math helpers and top-level empty libm. Audit link map for no incompatible runtime injection. |
| OW1.9 SYS dependencies | K `sys/makefile:9,54–55` uses portable DOS C headers and `wcl -mt`; `config/openwatcom.json` pins the extracted `h/*.h`, `h/sys/*.h`, `lib286/dos/{clibs.lib,doslfns.lib,cstart_t.obj}` and Linux wcl. Generated SYS is a build utility which may link this runtime; it is not installed as the rebuilt guest kernel or substituted for the pinned reference boot template. Kernel link-map restrictions remain mandatory. |
| Vendor comparison (f3-55 Amendment 3) | UPX 5.2.1 rejects the flattened SYS wrapper. The unused pin/tooling is removed; retain the original packed member/hash. Acceptance is two-host-date reproducibility plus the lead's unchanged f3-48 differential battery at 100% identical results. Record raw rebuild size and Wlink section layout beside vendor provenance. |
| gcc-ia16 kernel | K `mkfiles/gcc.mak:10–60`: genuine 16-bit target, ELF objects, packed structs and far function option. Distinct from DJGPP. |
| DOS compilers | K `config.b:35–47`: Turbo C 2.01/3, Turbo C++1.01, Borland3.1/5, Microsoft and Watcom variants; not a Linux-native bootstrap choice. |
| Kernel licensing | Per-file DOS-C GPL2-or-later headers, `COPYING`, `sys/fdkrncfg.c:8–11` Public Domain origin; retain mixed attributions. |
| FreeCOM licensing | Package GPL2, F `license`; bundled SUPPL library GPL and Library-GPL files, separate notices. Package shorthand is not every file's individual license. |
| SHARE licensing | Separate pinned package LSM GPL, no precise version granted by that field alone; source `share.c` and makefile. Reference only in this transition. |

For detailed scope, source-offer plan, runtime-link audit and reserved ABI
requests see the contract and f3-54 report. The proposed fork-local callout
backend preserves public 1100 absence; a globally installed redirector would
fail the unchanged `multiplex` conformance program. That choice needs explicit
lead confirmation before f3-57, not a change disguised as a new test profile.
