# f3-56 — Unified CiukiDOS binding, ROM gates, lifecycle, FCB and CP437

- **Phase/model:** F3; Codex, gpt-6.1-sol, xhigh with primary-source research.
- **Status/worktree:** complete directive text from f3-54; lead creates wt/f3-56
  after f3-55 and reviewed contract. No implementer commits.
- **Authority:** GPL-compatible fork additions, Ciuki CONFIG.SYS control driver,
  private DFVM binding/ROM adapter in src/kernel/vm/, FCB/NLS adaptation,
  tests/host/dos/, binding/layout documentation and validation.
- **Dependencies:** f3-55 integrated; the lead reserves the DFV2 ROM/discovery/
  capability proposal below at launch. The fork-local IFS seam is confirmed;
  public 1100h stays AL=0 and its battery assertion is unchanged. Guest identity
  is `kernel=unified`, per-game choice 2. The owner's GPL-2.0-or-later grant and
  approval of the GitHub plus per-release source-archive/hash destination have
  been requested: **do not start f3-56 until the lead records both answers**.
  f3-55 may proceed before that answer. No new transaction selectors are picked.
- **Size:** substantial ABI/lifecycle port; source-built bindings tested against
  the real guest CPU and native owner, not a Python DOS replacement.

## Read/research

Read f3-54 contract and inventories, f3-50 including its wire note, native
dos_file_hook and mirror publication code, K hdr/{process,sft,cds}.h,
kernel/{kernel.asm,entry.asm,task.c,inthndlr.c,fcbfns.c,nls.c,int2f.asm} in
bounded ranges. Verify [RBIL SDA](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html),
[SFT](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html) and
[Microsoft DOS reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/).
Record behavior differences and decisions before changing code.

## DFV2 ROM and discovery proposal

The lead selected the negotiated identity **DFV2**; the following exact ROM
reservation and wire assignments are proposals for the lead to reserve when
launching this directive, not permission to publish unreserved ABI. Retain
v1 discovery AX=C148h/BX=4644h/CX=1/DX=564Dh and every v1 descriptor/gate/table
byte. Proposed DFV2 discovery uses the same AX/BX/DX with CX=2, returning
AX=0/CX=2/ES:DI=F000:1C10 only for the identified unified profile. The descriptor
starts with its own `DFV2` signature; no v1 size or offsets are reused implicitly.

Reserve **F000:1C00–1FFF** (linear F1C00–F1FFF) within the existing protected
F1 page. Initialize unused bytes to CC; only the following are executable gates:

| Address | Exact bytes | Role |
| --- | --- | --- |
| F000:1C00 | `CD FE CB D2` | Checked direct/file operation; INT FE then RETF, identity marker D2. |
| F000:1C04 | `CD FE CB E2` | Late BIND/event 0 and lifecycle 1–8; RETF, marker E2. |
| F000:1C08 | `CD FE F4 C2` | Critical continuation; monitor completion, HLT guard, marker C2. |
| F000:1C0C | `CD FE CB A2` | Already-marshalled 11xx callout; RETF, marker A2. |

Proposed descriptor at 1C10 is 32 bytes, little endian: +0 signature
`44 46 56 32`; +4 u16 layout version 2 (`02 00`); +6 u16 size 32 (`20 00`);
+8 u32 negotiated capabilities (numeric bit assignments reserved by the lead);
+12 direct far pointer `00 1C 00 F0`; +16 event `04 1C 00 F0`;
+20 critical `08 1C 00 F0`; +24 marshalled callout `0C 1C 00 F0`;
+28 reserved u32 zero. Unknown required capabilities/version/length fail locally.
The distinct final bind record describes the contract's actual code/data/SDA/
SFT-chain/CDS/PSP/driver ranges with length/version tags; publish its complete
packed offsets and capability meanings for lead review before implementation.
Do not fit it into v1's 68-byte record. CP437 may reuse the identical immutable
v1 tables by explicit checked pointers; do not modify them or allocate game RAM.

Reservation proof covers **complete** occupied ranges: BIOS vector stubs
F000:0000–0FFF, EMS entry at 1000, mouse continuation at 1020, DPMI real gates
1040–11FF and protected stubs 1200–13FF, plus current DFVM 1800–1B53. Check
the actual symbols in dos_file_hook.h, dos_file_vm.c, dosvm_devices.h and
dpmi_internal.h; assert end <=2000 and no overlapping ranges, and compare v1
ROM bytes before/after. INT FE is accepted only at the exact gate's IP+2 with
the registered role/frame and current generation, never at marker/fill bytes.

Use existing selectors only: **0 BIND** at the late callback; **1 PSP_CREATE,
2 PSP_SELECT, 3 EXEC_BEGIN, 4 EXEC_COMMIT, 5 EXEC_ROLLBACK, 6 PROCESS_EXIT,
7 TSR, 8 VM_DESTROY**. INIT uses discovery/resident-driver registration only,
not BIND. These are process events, not generic file-transaction selectors.
FCB-close metadata and duplication use checked file requests carrying the
original DOS operation identity (10h, 45h/46h) and internal native transactions.
Ask the lead only if a demonstrated missing transition cannot use these paths;
provide the exact gap before requesting any additional selector.

## Required implementation

1. Retain v1 for the old guest byte-for-byte. Implement the reserved DFV2
   layout with checked code/data/driver segments, explicit SDA field addresses,
   actual SFT chain, CDS base/count/stride, PSP/JFT allocations, device headers,
   result/stack and build identity. Never store owning PSP at FreeDOS SDA+1C
   or overwrite a guest SFT chain with FF. No generalized weakening of v1 checks.
2. Provide two-phase initialization. CONFIG.SYS driver INIT validates discovery/
   ROM and registers only its resident allocation and deferred callback with
   the fork (no native bind/event). It does not bind kernel/SFT/CDS pointers,
   mount views or enable file dispatch.
   Finalize and bind **after init_kernel() returns and before DoInstall()**
   (`K:kernel/main.c:121–128`). Resolve final code/entry/data and structures
   after PostConfig SFT allocation/CDS replacement (`config.c:459–465`), final
   FsConfig (`main.c:348`) and DOS=LOW relocation (`config.c:506`). Validate the
   relocated callback afresh. f3-57 filesystem activation joins this late BIND
   transaction before any INSTALL, shell or AUTOEXEC program. Bare/pristine
   stays isolated; malformed required unified binding fails before applications.
3. Implement distinct direct/file, local 11xx marshalled-request and lifecycle
   ROM entry validation. Document packed fields and separate complete stack
   diagrams. Original INT21 and redirector return frames are not interchangeable.
   Gate spoofing from ordinary guest code, overlap and stale generation fail
   without native side effects. PENDING is monitor-only and resumes once;
   CHAIN restores the complete unowned frame; no nested INT21 redispatch.
   Implement interception immediately before **each** marshalled INT2F site,
   not just at call_int2f. Owned calls use the new callout gate without an extra
   interrupt frame; unowned calls execute the original site once. Preserve
   each wrapper's conversion and DS/SS expectations from the contract's table.
4. Port process boundary notifications to FreeDOS's actual fast paths, new_psp,
   child_psp, EXEC begin/commit/rollback, return_user, TSR/INT27 and VM destruction.
   AH50 selection cannot alter immutable ancestry. Preserve real MCB/JFT ownership,
   first20 inheritance/noinherit, expanded JFT and parent DTA/exit status. Fault
   every reversible publication and unwind native resources exactly once.
5. Retain upstream FCB engine and route its SFT operations through the same
   bound description interface. Port all f3-50 checked arithmetic, status and
   memory-safety behavior, including standard/extended FCB, seq/random/block,
   127/128 rollover, 24/32-bit record counts, record-size0/overflow, partial/EOF
   padding, DTA segment wrap, wildcard mutation and 59h errors. No alternate FAT
   parser or hidden native FCB pointer. f3-57 supplies the full live backend;
   until then host adapters are explicit fixtures and cannot claim runtime closure.
   Explicitly harden `K:kernel/fcbfns.c:252–271`: a negative DosRWSft return
   exits before unsigned transfer/count arithmetic; zero record size is rejected
   before the modulus at :268. Exercise negative, zero, partial and oversized
   transfer results and guarded FCB/DTA ranges.
   Add the checked INT21/10h close-metadata hook before :609–613, where upstream
   copies caller FCB size and skips merging for SFT_FSHARED. Submit caller size/
   packed date/time explicitly with validated PSP/SFT/generation; never trust
   SFT tampering or treat it as handle close. Match the contract's pristine-
   SHARE metadata/merge semantics. Freeze appended reference cases for changed
   size/date, concurrent change in both close orders, attribute-read-only files,
   read-only mounts, failed publication and reopen results. Reserve publication
   before mutation; failure leaves open state/metadata/refs intact, success closes
   once (AL=00, FCB slot FF). Error AL=FF/59h, repeated closed FCB no-op. f3-57
   supplies the live metadata backend and provider fault/stop/cancel evidence.
6. Use one CP437/country1 definition for 29/38/65/66, upper/collation/filename/
   DBCS tables and aliases. Support f3-50 65 01–07/20–23 with exact lengths and
   stable checked guest pointers; unsupported switching fails. Keep the real
   FreeDOS 2F1203/1216/1220/1226/122D view coherent and internal-open preconditions.
7. Preserve source licenses/authors and build determinism. No old loader header
   injected into FreeDOS; no legacy memory manager over native XMS/EMS/DPMI.

## Acceptance/host evidence

| ID | Required tests |
| --- | --- |
| H56-01 negotiation | Old/absent/unsupported/truncated/overlap/wrong-role/spoof/stale binding; same-segment and split-segment valid fixtures; allocation failure at every bind publication; old v1 remains unchanged. |
| H56-02 layout | Execute INIT then final SFT allocation (FILES), CDS replacement (LASTDRIVE), final FsConfig and DOS=LOW relocation; poison old addresses and reject early BIND/stale callbacks. Only final PSP/JFT/SFT-chain/SDA/CDS/code ranges are admitted after init_kernel/before DoInstall. Fault every late registration; guarded direct reads after open/dup/inherit/close/select, no padding scribble or severed chain. |
| H56-03 frame | Production guest CPU through every gate and all seven K int2f.asm sites: :355 getfattr and lseek (no argument WORD), :364 lock/unlock (no WORD, DS:DX/BL/CX block), :407 general (extra argument WORD), :432 printer (WORD plus changed DS), :443 getfree (WORD near output pointer), :454 read/write (no WORD, SDA DTA/CX, signed DX:AX result), :471 process-end and qualify (no WORD, changed DS). Publish exact stack diagrams; success/error/chain/pending where applicable, preserved BP/SI/DI/SS/DS/FLAGS and sentinels, unowned printer chain once, 0300/reflection and INT24 resume once. |
| H56-04 lifetime | Nested EXEC success/error/rollback, 26 vs55,50/51/62, child abort, TSR/INT27, VM stop and late completion; native/device references and MCB/JFT ledger return to baseline. |
| H56-05 FCB | H50-03 plus explicit negative-transfer/zero-modulus hardening at fcbfns.c:252–271 and authenticated close-metadata hook :609–613. Changed FCB size/date, concurrent writes/close order, read-only attribute/mount, publication failure/retry/reopen, repeated close and FcbCloseAll. Match frozen pristine-SHARE AL/59h/size/date/bytes/other-description results; reject forged SFT-only changes; no mutation or ref loss on precommit failure. |
| H56-06 NLS/internals | Every H50-02/04 layout/string/high-byte/length/precondition case; 38/66/29 and aliases agree; no unsupported-codepage success. |

Extend production host harnesses and their CPU adapter; add a focused
`tests/host/dos/run_unified_binding.sh` compiling the actual new guest and
native adapter, documented in the DOS README and included in normal host
kernel checks. New script retains ASan/UBSan and leak detection unless a
specific existing harness's documented setting applies.

Run sequentially at nice19: the new binding script, DOS unittest discovery,
run_file_api.sh, run_hook_cpu.sh and host_kernel_tests.sh, using their unmodified
sanitizer policy. These commands are `nice -n 19 bash <script>` and
`nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v`.
Public ABI edits require API reference regeneration and `nice -n 19 make CAP= apps`.

## Evidence/constraints

Write build/codex/f3-56-report.md and the final wire/layout note: reservations
actually supplied, every H56 status, source/map hashes, FCB/NLS results, stack
diagrams, failure injection and leak ledgers. Distinguish stubbed backend from
live VFS tests. Lead owns cross-review, canonical image and allowed runtime
evidence. Codex runs no QEMU/systemd-run/image build, no git mutations or diary
edits; no new panic, unmeasured memory increase, game/old-guest behavior change,
duplicated filesystem owner or private corpus reference. English throughout.
