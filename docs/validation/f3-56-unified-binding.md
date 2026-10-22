# f3-56 DFV2 binding: wire layout, stack diagrams and decisions

Recorded 2026-10-21 20:14 CEST, directive f3-56. This is a private Ciuki
interface between three separately built binaries: the CiukiDOS fork of the
FreeDOS 2043 kernel, its GPL-2.0-or-later control driver `CIUKIDF.SYS`, and
the CiukiOS VMM. It is not a public DOS, Microsoft multiplex or NTVDM API.
The ROM slot, version 2, the feature bits, the role bytes and the build
identities below were **reserved by the lead in f3-56 Amendment 1**
(2026-10-21 20:42; table in the contract). Version 1
(`dos_file_hook`, F000:1800–1B53) is unchanged and remains the only binding
of the old CiukiDOS guest.

## Sources consulted before implementation

- [Microsoft MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/):
  device driver INIT request (end address, status), PSP, FCB and country information.
- RBIL 61: [SDA 215D06](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html),
  [SFT/List-of-Lists 2152](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html),
  [21/27 random block read](https://fd.lod.bz/rbil/interrup/dos_kernel/2127.html),
  [21/28 random block write](https://fd.lod.bz/rbil/interrup/dos_kernel/2128.html),
  [21/29 parse filename](https://fd.lod.bz/rbil/interrup/dos_kernel/2129.html),
  [21/38](https://fd.lod.bz/rbil/interrup/dos_kernel/2138.html),
  [21/65](https://fd.lod.bz/rbil/interrup/dos_kernel/2165.html),
  [2F/1100](https://fd.lod.bz/rbil/interrup/network/2f1100.html) and the 11xxh callouts.
- [Intel SDM Vol. 2, PUSHF](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  a 386 in real mode pushes IOPL/NT, which K2043's `query_cpu` relies on.
- The pinned K2043 archive (inner `SOURCES.ZIP` SHA-256 `a977497b…a9d5`);
  coordinates `K:file:line` refer to it plus patch 0001, not to upstream master.

## Two-phase binding

FreeDOS replaces the structures a CONFIG.SYS driver could see: the SFT
chain is extended and the CDS replaced in `config.c:459–506`, the second
`FsConfig()` (`main.c:348`, body `:355–410`) rebuilds the CDS and reopens the
root's AUX/CON/PRN handles, and `configDone()` (`main.c:350`) relocates for
DOS=LOW. So:

1. **Phase 0, driver INIT.** `CIUKIDF.SYS` issues discovery
   (AX=C148h BX=4644h **CX=2** DX=564Dh, INT 2Fh; v1 answers CX=1 only),
   requires ES:DI=F000:1C10, compares every descriptor and gate byte with
   its compiled-in copy, then registers only its own resident memory
   (event gate, AX=0, phase 0). Any failure prints one line, returns
   status 810Ch with the break address at its header: FreeDOS does not link
   it (`main.c:579–587`) and the kernel stays unbound.
2. **Phase 1, `CiukiLateBind()`.** Patch 0002 calls it in `main.c` right
   after `init_kernel()` (`:121`) and before `DoInstall()` (`:128`), so INSTALL= and the shell
   already run bound. It finds `CIUKIDF$` in the device chain, checks the
   control block at header+12h and its registered state, fills the final
   record from the live kernel (below) and calls the driver's late-bind
   entry, which adds its registration and enters the event gate. Only after
   success are the resident hooks (`CiukiHooks`) published. Without the
   driver the function returns silently and the kernel is the upstream one.

The VMM allocates its DFV2 state (one 4 KiB page, charged to the VM's
commitment) at the first event-gate call, writes the DFV2 ROM only for the
unified profile (kernel=unified, value 2) and never answers DFV2 discovery
for the old guest or the pristine reference.

## ROM slot proposal (protected page F1000h)

| F000: | Bytes | Meaning |
| --- | --- | --- |
| 1C00 | `CD FE CB D2` | direct (claimed INT 21h) gate |
| 1C04 | `CD FE CB A2` | local 11xxh redirector gate |
| 1C08 | `CD FE CB E2` | lifecycle/bind gate |
| 1C0C | `CD FE F4 C2` | critical continuation (monitor-only PENDING) |
| 1C10–1C3F | descriptor, 48 bytes | see below |

1B54–1BFF and 1C40–1FFF stay free. The range is disjoint from v1
(1800–1B53), the DPMI real-mode stubs (1040–DPMI_RM_END ≤ 1800), the DPMI
protected stubs (F1200–F13FF), the mouse return (1020–1023) and the BIOS
page F0. `dos_file_vm.c` asserts all of these at compile time.

Descriptor (little endian): `+00` "DFV2"; `+04` u16 2; `+06` u16 48;
`+08` u32 features 3Fh (bit0 direct, bit1 local 11xxh, bit2 lifecycle,
bit3 critical continuation, bit4 two-phase late bind, bit5 single
country-1/CP437 table set); `+0C/+10/+14/+18` far pointers to the four gates;
`+1C` u32 VM generation; `+20` u16 call-record bytes 48; `+22` u16 bind-record
bytes D0h; `+24` u16 ROM range start 1C00h; `+26` u16 end 1C40h; `+28` 8 zero
bytes. The driver requires every byte except the generation.

## Call record (48 bytes, inside the driver; registered in phase 0)

`+00` u16 version 2; `+02` u16 48; `+04` u32 generation; `+08` u32 serial
(previous+1, never reused); `+0C` u16 role 1/2/3; `+0E` u16 status: the driver
presets FFFFh, the VMM writes 0 CHAIN or 1 DONE; `+10` FLAGS; `+12…+22` AX BX
CX DX SI DI BP DS ES; `+24/+26` caller frame SS:SP; `+28` 11xxh stack word;
`+2A–2F` zero. The VMM reads only this record (and, for direct/local roles,
the frame it names) and writes only `+0E…+23` (DONE) or `+0E` (CHAIN). A
refused call writes nothing: the driver sees FFFFh.

## Bind record (D0h bytes)

`+00` "DFB2"; `+04` u16 2; `+06` u16 D0h; `+08` u16 phase; `+0A` u16 0;
`+0C` u32 generation; `+10` driver segment, `+12` resident bytes, `+14` call
record, `+16/+18` stack offset/bytes, `+1A/+1C/+1E` return sites of the
direct/local/event calls; `+20` driver identity `CIUKIDF2 F3-56 1`.
Phase 1 only: `+30` kernel identity `CIUKIDOS K2043 2`; `+40/+42` DGROUP
segment/bytes (to `DynLast()`); `+44/+46` resident text segment
(`CurrentKernelSegment`)/bytes; `+48` List-of-Lists (DGROUP:0026); `+4C/+50`
SDA and 78Ch bytes (`kernel.asm` controlled variables); `+52` SFT entry
size 59; `+54` ten far pointers: ErrorMode, InDOS, locus, extended error,
action, class, DTA, PSP, return code, default drive (SDA +00,+01,+03,+04,
+06,+07,+0C,+10,+14,+16); `+7C` SFT head; `+80` SFT entries; `+82` CDS
count; `+84` CDS; `+88` stride 88; `+8A` root PSP; `+8C` NUL header
(LoL+22h); `+90…+A4` country record, upcase, filename upcase, terminator,
collating and DBCS tables; `+A8–AF` zero; `+B0–CF` VMM output: the INT 21h
AH bitmap the backend claims for the direct role (none in f3-56).

The VMM checks, before any publication: identities and generation; the
registration unchanged; conventional-memory extents (DOS=HIGH is refused);
disjoint driver, data and text; LoL and SDA inside DGROUP; every SDA field
at its exact offset; the List-of-Lists agreeing with the record; the
**actual** SFT chain walked to its FFFFh link (≤16 blocks, ≤255 entries,
disjoint from every control area and each other, total equal); CDS
26×88; the device chain from NUL containing CON, AUX, PRN, CLOCK$ and this
driver exactly once; the root as the selected PSP with its 20-entry JFT
in the PSP; the NLS tables (country 1, code page 437, size 38, both upcase
tables identical and idempotent, empty DBCS, the RBIL terminator table).
The tables are then snapshotted as the one definition the native side may
use (f3-57). Nothing is written to SDA, SFT, CDS, PSP or JFT; the owning
PSP is never stored at FreeDOS SDA+1Ch, which K2043 uses itself.

## Stack diagrams

Every gate is entered on the driver's private 512-byte stack by one far
CALL per role; the VMM requires SS = driver segment, SP = stack top − 4,
`[SS:SP]` = that role's registered return site and `[SS:SP+2]` = driver.

```text
Direct role (claimed INT 21h), hook at the top of int21_service():
  caller stack  SS_u:SP_u+00  iregs AX BX CX DX SI DI BP DS ES (FreeDOS pcb.h)
                         +12h  IP  CS  FLAGS  (the original INT 21h frame)
  kernel stack  ... int21_service(r) -> ciuki_direct(r) -> CiukiHooks.direct
                (cdecl far call: [bp+6] = r)
  driver stack  top-4: return site (direct), driver CS
  record        frame SS:SP = r; AX..ES and FLAGS must equal iregs
  DONE: driver copies AX..ES into iregs and the arithmetic FLAGS
        (OF SF ZF AF PF CF) into iregs.FLAGS; the kernel skips dispatch.
  CHAIN: nothing changes; FreeDOS dispatches the unchanged frame once.

Local role (11xxh callout), REDIRECTOR_INT2F at int2f.asm:355,364,407,
432,443,454,471 replaces "int 2fh" by "pushf; call far [CiukiHooks.local]":
  kernel stack  SS=DGROUP:SP+0 IP  +2 CS (kernel text)  +4 FLAGS  +6 11xxh word
                (arguments in AX..ES exactly as for an installed redirector)
  driver stack  top-4: return site (local), driver CS
  record        frame SS:SP = that kernel frame (inside DGROUP), FLAGS and
                stack word copied from it, return CS:IP inside the text
  DONE: registers from the record, arithmetic FLAGS live, RETF 2.
  CHAIN/refused: registers restored, jump to the *current* INT 2Fh vector
        with the same FLAGS/CS/IP frame (later redirectors still chain).

Lifecycle and bind role:
  kernel/user stack  far cdecl call: event, psp, parent, kind
  driver stack       top-4: return site (event), driver CS
  record             AX=event BX=psp CX=parent DX=kind; frame fields zero;
                     bind: AX=0, DS:SI = bind record
  DONE: CF/AX = DOS error, 0 on success.

Critical continuation (monitor-only PENDING):
  INT FE at a direct/local gate returns PENDING to the monitor; the guest
  sees nothing. The monitor saves the gate frame and pushes
  [FLAGS, F000, 1C0C] below the gate frame on the driver stack, then runs
  the guest INT 24h vector with the backend's AX/DI/BP:SI. Its IRET hits
  the C2 gate with SS:SP = stack top − 4; the VMM completes the record once,
  the monitor restores the saved frame and resumes after the original INT FE.
```

Spoofing: ordinary code that calls a gate from its own stack, with another
role's return site, a replayed or skipped serial, a stale generation, a
mismatched iregs/11xxh frame or an unwritable record is refused with no
native change (stats only). The guest could imitate the driver completely,
as it can imitate DOS itself; it never reaches a native address, and every
guest pointer is range-checked.

## Lifecycle (existing event meanings 1–7; 8 is the monitor's VM stop)

| FreeDOS site | Event | Failure handling |
| --- | --- | --- |
| `inthndlr.c` 26h after `new_psp()` | 1, kind 26h | ignored by DOS: no handle is cloned |
| `inthndlr.c` 55h after `child_psp()` | 1, kind 55h (selects) | CF/AX; `ciuki_release_jft()` drops the clones once |
| `int21_syscall` 50h (caller's stack) | 2 | not reportable; the VMM re-reads SDA+10h at every event |
| `DosComLoader`/`DosExeLoader` before `child_psp()` | 3 | blocks freed, EXEC returns the error |
| `load_transfer()` before `cu_psp=child` | 4 | `ciuki_exec_unwind()`: clones, child blocks, event 5 |
| COM `asize<11h` (upstream leak) | 5 | unwound when bound |
| `return_user()` after closes | 6, or 7 for 31h/INT 27h | termination is never refused |

The native process records keep the immutable parent; AH=50h only changes
the selection. EXEC_BEGIN requires the block's arena owner to be the
parent; COMMIT requires it to be the child and the child's 20-entry JFT to
equal `CloneHandle()` applied to the parent (open, not O_NOINHERIT).

## FCB and NLS decisions

The upstream engine stays (`fcbfns.c`). Hardening, all in patch 0002: record
size 0 and current record ≥128 → AL=1, 59h=87; position or end of transfer
beyond 4 GiB → AL=1, 59h=39 (upstream allocated clusters to 528 MB and
filled the disk); a negative transfer result no longer enters the record
arithmetic (`fcbfns.c:252–271`) and the modulus uses the record size read
on entry (the DTA may overlap the FCB, `:268`); record counts use 32-bit
arithmetic; random records are 32-bit below 64-byte records and 24-bit
otherwise, preserving the high byte on write-back; the SFT index in the
reserved byte must name an open, FCB-opened entry with the FCB's name
(AL=FFh, 59h=6 otherwise; upstream let an FCB read or write a handle-opened
SFT such as CON); invalid drives report 59h=15; EOF results report 38;
29h keeps an absent name when bit 2 is set (RBIL; upstream blanked it);
FCB find refuses a DTA that would wrap. FCB close of a local file keeps the
reference semantics (its size field becomes the entry size); for a
redirected (FSHARED) entry the size is published as an explicit standard
1109h zero-length write at that position (`ciuki_fcb_close_size`), never by
storing into the SFT; a refusal keeps the FCB open. FreeDOS keeps the SFT
index in reserved byte 18h, so a forged byte cannot be neutralised the way
the v1 native FCB map did; it can only name the caller's own FCB entries.

NLS keeps FreeDOS's hardcoded country 1/CP437 package as the single
definition (`nls_hc.asm`; 65h, 66h, 38h, 29h and the filename upcase use it,
and the bind snapshots it). The f3-50 results are implemented on it: 65h/01
returns the DOS 4+ 41-byte record (size 26h; ten reserved bytes appended),
CX ≥ 5 returns that bounded prefix and CX = length; 65h/02–07 return 5 bytes
and CX=5; 65h/03 points to a lowercase table derived from table 2 only;
CX < 5 or a destination wrapping its segment → 87; 65h/21/A1 beyond the
segment and 65h/22/A2 without a terminator before it → 87 (upstream
faulted on the wrapping copy under a 386 model and wrapped silently
elsewhere); 66h/02 selecting the active page succeeds; other pages and
countries still fail. 38h keeps FreeDOS's 24-byte copy. The case tables are
FreeDOS's (e.g. 82h→'E'), not the Unicode-derived v1 tables.

## Decisions confirmed by the lead (f3-56 Amendment 1)

- Decision: an FCB whose reserved SFT byte does not name an open, FCB-opened
  entry with the FCB's own (uppercased) name fails with AL=FFh and 59h=6 on
  14h/15h/21h/22h/27h/28h and 10h.
- Decision: after an FCB end of file (AL=1/3) 59h reports 38; after a short
  FCB write (AL=1) it reports 39.
- Decision: 65h/03 returns a lowercase table derived only from table 2.
- Decision: FreeDOS's hardcoded country-1/CP437 tables are the single
  definition for 29h, 38h, 65h, 66h, the filename upcase and the native
  snapshot. **Accepted change relative to v1:** CiukiDOS's uppercasing of
  80h–FFh now follows FreeDOS/MS-DOS country 1 (for example 82h é→'E',
  84h ä→8Eh, A4h ñ→A5h), no longer the Unicode-derived v1 tables, which
  left accented letters without a CP437 capital unchanged.
- Decision: 2F/1226h behaves as in K2043 (`inthndlr.c:2018`, reaching
  `DosOpen`); the v1 "DOS internal stack only" precondition is not ported.
- Decision: `kernel=unified` requires DOS=LOW; the final bind refuses a
  kernel text or data range outside conventional memory and the VMM logs
  that refusal once per VM with its reason.

## Evidence and limits

`tests/host/dos/run_unified_binding.sh` builds the fork twice (identical
bytes across roots and input mtimes) and the pristine patch-0001 kernel,
runs the native matrix (242 checks) and 24 libx86emu boots: bound with and
without claims, unbound fork, pristine with and without the driver, no
host, seven descriptor/ROM-role faults, failure injection at both binds
and at EXEC begin/commit and 55h, the FCB and NLS probes on both kernels,
and the 27 basic f3-48 probes with identical results on the pristine
kernel and the bound fork. Every native write is classified (call record
or claim output only). The backend is a fixture: no file operation reaches
a native VFS. QEMU, the canonical image, DPMI 0300h/reflection origins,
INT 24h Abort through a PENDING request and hardware remain the lead's.
