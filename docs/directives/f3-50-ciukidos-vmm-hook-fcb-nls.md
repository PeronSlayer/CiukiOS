# f3-50 — CiukiDOS handoff, coherent DOS internals, FCB services and CP437 NLS

- **Phase:** F3, DF1–DF5. **Model:** gpt-6.1-sol / xhigh with internet research. **Worktree:** `wt/f3-50` from main after reviewed/integrated f3-49.
- **Authority:** `src/runtime/ciukidos.asm`, only relevant bounded ranges in `src/boot/floppy_stage1.asm`, internal DOS/VMM binding glue in `src/kernel/vm/`, canonical DOS fixture/probe installation glue, `tests/host/dos/`, personality host harness, design/validation notes. `SHELL.COM` behavior and game binaries are unchanged.
- **Size:** approximately 1,500–2,500 assembly/C lines plus 1,000–1,500 host-test lines and removal of proved-dead sector paths. Six acceptance groups, completed together. This includes FCB edge semantics, not just dispatch labels.
- **Dependencies:** reviewed f3-49 state/description APIs, f3-47 provider and cross-review of the hook protocol below. Claude owns merge and evidence. No automatic FreeDOS migration.
- **Read first:** f3-48 register inventory and source register; current INT21 prologue, PSP/JFT initialization, EXEC unwind, List-of-Lists/SDA, path/FCB parser and canonical runtime include. Re-read RBIL PSP/SFT/SDA/FCB/NLS/internal 2F entries and FreeDOS `FcbReadWrite`, `FcbRandomIO`, `FcbRandomBlockIO`, `FcbOpen`, `FcbParseFname` as behavioral references only.

## Required

1. **Versioned private handoff.** Implement the following internal protocol and document its final packed offsets. It is a Ciuki protocol, not an existing Microsoft multiplex API. Discovery is INT2F AX=C148h, BX=4644h (`DF`), CX=1, DX=564Dh (`VM`). Exact signature match under the owning CiukiDOS VM returns AX=0, CX=1, ES:DI descriptor; a different signature chains untouched. The VMM supplies two immutable synthetic-ROM far gates, one file dispatch and one lifecycle event; reserve their ranges against DPMI/BIOS/mouse stubs by symbol and test, rather than hardcoding an occupied offset. The descriptor contains `DFVM` magic (4), version (u16), byte size (u16), feature bits (u32), dispatch far pointer (4), event far pointer (4): minimum 20 bytes. Discovery never hands out a native address.
2. **Bind before interception.** The event gate consumes AX event, BX selected PSP, CX owning PSP, DS:SI a checked binding structure for BIND or checked event arguments thereafter. BIND has version/size (u16 each), old INT21 far pointer, SDA far pointer/byte extent, List-of-Lists far pointer, SFT first block far pointer/entry size/count, selected-PSP and DTA field far pointers, and one-byte dispatch-result far pointer. Fixed widths and explicit bounds are part of the final documented protocol. VMM checks all extents and identities; binding publishes atomically or fails with no ownership change. Event codes: 0 BIND, 1 PSP_CREATE, 2 PSP_SELECT, 3 EXEC_BEGIN, 4 EXEC_COMMIT, 5 EXEC_ROLLBACK, 6 PROCESS_EXIT, 7 TSR, 8 VM_DESTROY. Event return is CF/AX (0 success, DOS error otherwise). File gate receives original DOS registers; result byte 0=CHAIN, 1=DONE, 2=PENDING. PENDING remains suspended in the monitor; guest code resumes only as DONE/CHAIN, never polls shared state. No replay of a partly completed request.
3. **One frame and recursion guard.** Place the handoff before the legacy INT21 dispatcher saves/clobbers registers. A far CALL adds only its four-byte return address to the original interrupt frame. The gate's INT FE/private escape is accepted only at the published ROM address for the live binding, and consumes no extra INT21 frame. DONE writes only declared result registers and the original saved FLAGS, then CiukiDOS returns by IRET. CHAIN restores live flags/registers and jumps once to the saved legacy handler. Never issue nested INT21 to redispatch the same file call. Explicit per-VM dispatch depth distinguishes FCB-to-handle adapter calls and permitted INT24 console/59h calls from accidental recursion. Protect vector changes and hooks without breaking DOS/4GW's own translation chain.
4. **Lifecycle and state coherence.** Notify the f3-49 SFT owner before/after every listed lifecycle boundary. Preserve immutable EXEC ancestry even when AH50 selects another PSP. Error/abort/TSR and failed EXEC paths cannot skip notifications. Initialize the JFT from actual inherited SFT indices; update expanded JFT memory/count/pointer coherently. 26h Create PSP and 55h child PSP have distinct documented behavior. InDOS/critical/DTA/PSP/default drive and SFT mirror stay coherent for programs reading SDA/SysVars directly. Keep DOS's shared cwd/default drive across ordinary EXEC (DF2 correction); DTA restoration remains per process.
5. **FCB adapter.** Implement 0F–17,21–24,27/28 on the f3-49 descriptions, not sector routines. 26h remains PSP creation. Standard FCB is 37 bytes; extended FCB starts FFh and has seven-byte prefix/attribute. Use DS:DX FCB and the current DTA, default record size 128, checked record-size/block/current-record/random-record arithmetic and exact AL statuses (0 success, 1 EOF/no record, 2 segment wrap, 3 partial record as defined by each call). 27/28 return actual CX records and update random position; sequential/current-block and random fields change only as documented. Pad short final reads with zero where required. Handle block/random rollover, zero record count, random-file-size rounding, DTA segment boundary, wildcards in find/delete/rename and extended attributes. 29h parser preserves its caller's pointer/update flags and CP437 rules. Use a bounded FCB-to-description mapping keyed by VM/PSP/FCB identity with stale-file detection, not hidden native pointers in FCB reserved bytes. FCB errors update 59h context even though AL reports failure.
6. **NLS and internal services.** Implement 65h AL=01–07 and 20–23 for country 1/CP437: correct buffer lengths, stable guest pointers to upper/collating/filename/DBCS tables, character and counted/ASCIIZ uppercasing, yes/no classification. CP437 has no DBCS lead bytes. Existing 38h/66h tables agree; unsupported pages/countries fail explicitly rather than pretending a codepage switch. Implement internal 2F/1203,1216,1220,1226,122D with the documented preconditions and mirror/59h results. 1226 is an internal DOS-stack-only open; it marshals into the same file service and cannot expose a second disk owner. Do not advertise 11xx redirector or ANSI 1Axx services. Unknown multiplex functions chain.
7. **Remove proved-dead paths only.** With the binding active, all file/directory functions and EXEC's file loads must go through the VMM/VFS service; otherwise EXEC retains private FAT data paths and observes incoherent state. EXEC remains a CiukiDOS process operation but uses an internal handle adapter to read COM/MZ/overlay contents. Audit SHELL built-ins and direct kernel reads/writes, not just external INT21 callers. Retain early boot and disabled-binding fallback sector code where reachable (including the original DOS personality's future purpose); delete only paths with a call-site/reachability proof and equivalent new coverage. No restoration/build of the floppy profile and no mass rewrite of the 25K-line source.

## Acceptance and host tests

| ID | Required evidence |
| --- | --- |
| H50-01 binding/lifetime | Absent/old/malformed descriptor chains identically; valid descriptor binds once; gate spoofing and overlapping ROM reservations fail locally. Success/error EXEC, 26/55,50/51/62, nested children, TSR and INT24 Abort maintain JFT/SFT/SDA and restore parent DTA. Fault every allocation/notification transition and prove rollback/leak freedom. |
| H50-02 internals | Compare guarded guest-memory PSP/JFT/SFT/SDA/List-of-Lists layouts with reference, including direct reads after transfer/dup/inherit/close. Exercise 2F/1203,1216,1220,1226,122D valid/invalid calls; native state cannot be forged by guest mirror writes. Trace shows exactly one dispatch per original file call and coherent return CF/flags. |
| H50-03 FCB | All standard/extended FCB functions, random/sequential/block I/O, 127→128 record rollover, 24/32-bit random count, short partial record/zero padding, EOF, invalid drive, zero/count overflow, segment-wrap DTA, two FCBs and wildcard search/rename/delete. AL/CX/FCB bytes and guarded DTA match FreeDOS, and all errors reach 59h. No sector-API call occurs for file I/O after mapping. |
| H50-04 NLS | Every 65h subfunction and CP437 high byte, exact returned lengths/table pointers, insufficient/malformed buffers, unsupported page/country, counted strings with NULs, zero count and boundary overflow; 38/66/29 and alias generation agree. |
| H50-05 regression | Execute unchanged SHELL DIR/CD/MD/RD/COPY/DEL/REN/TYPE/redirection/batch/command lookup, COM/MZ/overlay EXEC, malformed relocation/EXEC rollback, existing 16/32-bit DPMI and DOS/4GW probes through the production host CPU model. Verify inherited handles share positions, private handles disappear and no guest memory allocator behavior regresses. |
| H50-06 dead-path proof | Before/after call-site inventory for removed sector routines; fixture with binding disabled retains early boot and fallback functions. Bound source reads and cite exact removed labels; no duplicated active FAT owner/cache or changes to SHELL's interface. |

Run `nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v`
and the focused H49/H50 production host suites, sequentially, then
`nice -n 19 env ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`.
Host fixtures may assemble the canonical DOS personality but never the floppy
profile. The lead owns canonical images and QEMU. Match the COM binary hashes
between host and later reference runs; successful assembly alone is no guest
conformance result.

## Evidence

Write `build/codex/f3-50-report.md` with sources, final versioned hook fields and
stack diagram, precise routed function/subfunction table, all H50 outcomes,
FCB status register/memory results, syscall counts, removed/reachable labels,
SHELL/DPMI regressions and remaining f3-51 runtime qualification. Update the
contract/validation document for any reviewed interface refinement. A public
header/ABI change requires `python3 scripts/gen_api_reference.py` and
`nice -n 19 make CAP= apps` immediately after it.

## Constraints

Use `rg`/`sed`, never Semble or whole-file reads of `floppy_stage1.asm`.
No QEMU, systemd scopes, image builds, commits or diary edits by Codex; host
tests sequential at `nice -n 19`. No FreeDOS kernel replacement/copy, game or
SHELL behavior change, new data cache, DPMI/ISR architecture rewrite or memory
cap increase. Preserve the exact approved Ciuki identity and English UI.
