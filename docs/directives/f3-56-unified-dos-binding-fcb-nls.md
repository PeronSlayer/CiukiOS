# f3-56 — Unified CiukiDOS binding, ROM gates, lifecycle, FCB and CP437

- **Phase/model:** F3; Codex, gpt-6.1-sol, xhigh with primary-source research.
- **Status/worktree:** complete directive text from f3-54; lead creates wt/f3-56
  after f3-55 and reviewed contract. No implementer commits.
- **Authority:** GPL-compatible fork additions, Ciuki CONFIG.SYS control driver,
  private DFVM binding/ROM adapter in src/kernel/vm/, FCB/NLS adaptation,
  tests/host/dos/, binding/layout documentation and validation.
- **Dependencies:** lead reserves the new DFVM version, layout capability bits,
  gate role and disjoint ROM range, any required lifecycle extension selectors
  and personality identity. Reuse existing event meanings 0–8 where sufficient;
  choose no new numbers. Lead resolves rights for any copied GPL-only Ciuki
  source; new fork implementation is GPL-2.0-or-later. Confirm the local-IFS
  callout seam so no new public redirector advertisement breaks the base battery.
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

## Required implementation

1. Retain v1 for the old guest byte-for-byte. Implement the reserved additional
   layout with checked code/data/driver segments, explicit SDA field addresses,
   actual SFT chain, CDS base/count/stride, PSP/JFT allocations, device headers,
   result/stack and build identity. Never store owning PSP at FreeDOS SDA+1C
   or overwrite a guest SFT chain with FF. No generalized weakening of v1 checks.
2. Provide the GPL guest control driver loaded through CONFIG.SYS. It validates
   exact discovery/version/length/features, immutable ROM role bytes/ranges,
   registers owned memory and publishes binding transactionally. Bare/pristine
   FreeDOS remains isolated without it. Unknown versions/signatures chain;
   invalid required unified binding returns a contained initialization failure.
3. Implement distinct direct/file, local 11xx marshalled-request and lifecycle
   ROM entry validation. Document packed fields and separate complete stack
   diagrams. Original INT21 and redirector return frames are not interchangeable.
   Gate spoofing from ordinary guest code, overlap and stale generation fail
   without native side effects. PENDING is monitor-only and resumes once;
   CHAIN restores the complete unowned frame; no nested INT21 redispatch.
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
| H56-02 layout | Guarded genuine FreeDOS PSP/JFT/SFT chain/SDA/CDS direct reads after open/dup/inherit/close/select; no padding scribble, no forged authority or severed chain. |
| H56-03 frame | Real guest CPU through all gate roles, complete register/FLAGS/stack sentinels, hook-on-hook chain once, 0300/reflection origins and pending INT24 completion once. |
| H56-04 lifetime | Nested EXEC success/error/rollback, 26 vs55,50/51/62, child abort, TSR/INT27, VM stop and late completion; native/device references and MCB/JFT ledger return to baseline. |
| H56-05 FCB | Every H50-03 case on upstream engine through the checked adapter, including boundary and injected failures; guarded AL/CX/FCB/DTA exact results. |
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
