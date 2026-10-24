# f3-57 unified CiukiDOS local IFS channel (host validation)

Recorded 2026-10-21 21:34 CEST (worktree `wt/f3-56`, continuing f3-56).
Host evidence only: libx86emu CPU boots of the real fork on private FAT16
fixture disks and sanitized native code. No QEMU run, no OS image, no
hardware. The lead owns the f3-48 QEMU differential (H57-06) and the T23
DF6 measurements (H57-07).

## Sources read (K2043 = FreeDOS kernel 2043 patch-0001 sources)

| Topic | Source |
| --- | --- |
| Seven redirector sites | `K:kernel/int2f.asm` 353–485 (`REDIRECTOR_INT2F` at getfattr/lseek, lock, general `int2f_call`, printer, getfree, read/write, process-end/qualify) |
| Callers | `K:kernel/dosfns.c` 240 (REM_READ/WRITE, DTA = buffer, CX = count), 344 (1121h only for deny-read/none), 538–564 (open/create/112Eh; FreeDOS increments `sft_count` only when the redirector left it 0), 700 (1106h/1107h: no local decrement), 770/885 (110Ch, also 7303h), 954–973 (1105h, FreeDOS copies FN1 to the CDS), 1022/1082 (111Bh/111Ch on `sda_tmp_dm`), 1143/1164 (110Fh/110Eh), 1204/1218/1258 (1113h, 1111h, 1101h/1103h), 1413 (110Ah param block {ofs,len}); `task.c` 598/604 (1122h, 111Dh, then per-handle close); `blockio.c` 359 (1120h) |
| Drive routing | `newstuff.c` 326–345 (CDSNETWDRV ⇒ IS_NETWORK; 1123h tried for every drive, local mapper on failure); `hdr/cds.h` 69–81 (C000h = installable file system); `fatfs.c` 90 (no DPB for network drives); `ioctl.c` 175–206 (4409h: AX=0300h, DX=1000h; other block IOCTLs 000Fh) |
| SDA | `kernel.asm` 584–760: DTA +0Ch, PSP +10h, FN1 +9Eh, FN2 +11Eh, search block +19Eh, entry +1B3h, SAttr +24Dh, ext open action/attr/mode +2DDh/+2DFh/+2E1h |
| Critical errors | `entry.asm` 546–700 (`_CriticalError`: INT 24h on the user stack with the INT 21h frame, Abort → 4C00h restart), `inthndlr.c` 1839 (2F/1206h), 470 and 1110 (ErrorMode and 4Dh type) |
| RBIL | INT 2F/1106h (redirector decrements the SFT count), 1108h, 1116h, 1121h, 112Eh, 1206h (invoke critical error; stack word = INT 24h AX) |
| Win98 RK | ch. 28 "Installable File Systems" (`build/rskt/wrkc28_5.htm`): the IFS Manager arbitrates every file-system request; redirectors no longer hook INT 21h |

## Decisions

1. **Owned drives become IFS drives.** The VMM converts the owned CDS entries
   to `C000h` (network+physical = IFS, `cds.h`) inside the final bind's commit,
   before `ciukinit.c` publishes the hooks. FreeDOS then routes every file
   operation on those drives through its own redirector sites; the fork's
   SFT/JFT/CDS/SDA bookkeeping stays upstream. C: is the VM's logical FAT16
   view (f3-47 provider, private overlay when open), D: the native system
   drive read-only, E: the conformance fixture or the first native removable
   drive, read-only by policy.
2. **One native description per SFT slot.** The open file is named by ES:DI
   (resolved against the bound SFT slot table); a per-slot nonce in
   `sft_dirsector` (+1Bh) authenticates it. FreeDOS's reference count is
   authoritative for dup/inherit/forced dup; the open leaves the count 0 for
   FreeDOS's own increment (`dosfns.c:560`), 1106h decrements it (RBIL) and
   closes the native description at the last reference, so no reference is
   counted twice. A forged or stale owned SFT is DOS error 6 and never
   reaches native state; a slot the guest reused while still held is
   reclaimed (closed) first.
3. **Positions and sizes.** Reads/writes use `sft_posit` (FreeDOS keeps SET/CUR
   seeks local) and write back position and size; CX=0 truncates or extends
   at the position (the FCB-close size hook `ciuki_fcb_close_size` uses it);
   1121h returns the native size + offset. `SFT_FDATE` times are applied at
   commit/close; O_SYNC commits after writes.
4. **Critical errors are raised by the kernel itself.** The live backend never
   uses the monitor-only PENDING path: a DF5 critical error (provider I/O,
   critical share, stale generation, read-only mount, lock on read/write)
   completes the local call with status **2 CRITICAL** and AX = INT 24h
   AH/AL (flags 18h|write, drive), DI = cause, BP:SI = CIUKIDF$ header.
   CIUKIDF.SYS, on the kernel stack and with `busy` clear, calls INT 2Fh/1206h
   with that AX as the stack word, so FreeDOS's `CriticalError` builds the
   genuine INT 24h frame on the user stack and handles Abort by its own 4C00h
   restart (the driver frame is simply abandoned). Retry re-enters the gate
   with the original registers (kept on the caller's stack) and call word
   **+2Ah = 1**; the VMM accepts it only for the identical request, within the
   original 2000 ms total deadline (INT 24h time included), else Fail. Fail
   returns 53h; Ignore is not offered. O_NOCRIT (open mode / 6Ch bit 13)
   returns the error directly.
5. **Upstream defect fixed in the fork.** K2043's Watcom 386 build keeps the
   2F/12xx register frame pointer in GS:SI (`wdis inthndlr.obj`: `mov gs:[si+10h],al`
   after `call _CriticalError`), but `CriticalError` loads the user's FS/GS
   for INT 24h and never restores them: 2F/1206h returned AL unchanged (06h
   observed) and wrote the reply through the user's GS. `entry.asm` now saves
   and restores the caller's FS/GS around the call (patch 0002, WATCOM/386
   only). Without it no redirector Retry can work on this kernel.
   Accepted by the lead (f3-57 Amendment 1); to be reported upstream after
   the 1.0 Alpha with this minimal reproduction: on a K2043 built with Open
   Watcom for 386 (`Kernel compatibility 7.10 - WATCOMC - 80386`), inside any
   INT 21h file call (kernel stack, SS = DGROUP) execute
   `push 1802h / mov ax,1206h / int 2Fh / add sp,2` with an INT 24h handler
   that returns AL=01h and a user GS other than DGROUP. Expected AL=01h;
   observed AL=06h (AX unchanged), and the reply byte goes to
   user-GS:[SI+10h] (SI = kernel-stack offset of the 2F/12xx frame).
   `wdis kernel/inthndlr.obj` shows `call _CriticalError` followed by
   `mov byte ptr gs:0x10[si],al`; `entry.asm` `_CriticalError` runs
   `Restore386Registers` (pops the user's GS/FS) before `int 24h` and never
   restores the caller's. Host reproduction: `H57.COM F` step
   `critical-retry` on the fork without the `entry.asm` hunk.
6. **4Dh after Abort.** K2043 clears ErrorMode at the 4Ch dispatch
   (`inthndlr.c:470`) before case 4Ch reads it (`:1110`), so 4Dh returns type
   0 after an INT 24h Abort (MS-DOS: 2). Kept: the pristine kernel is the
   oracle.
7. **Activation barrier** (event 0 phase 1, after `init_kernel()`, before
   `DoInstall()`): `ciukinit.c` writes all dirty buffers (INT 21h/0Dh), then
   the bind's prepare mounts the views and checks SDA extent, CDS
   writability, C: local, every view within LASTDRIVE and no bootstrap regular
   file open on an owned drive; any refusal unmounts and leaves nothing. The
   commit only writes the CDS entries; the VMM then routes BIOS INT 13h for
   C: through `dos_file_volume_raw` (same provider/generation, raw-write
   invalidation). A refused activation stops before INSTALL/shell unless the
   driver was loaded with `/LOCAL`, the explicit isolated-fixture policy that
   keeps the local FAT (`unified_binding.py` fixtures use it). f3-57
   Amendment 1: the monitor stops the VM at that same lifecycle gate call
   (`DOSVM_FAULT`), before control returns to the guest, with one record
   `[dosvm] unified_activation_refused reason=<token>`, so the DOS Box shows
   the refusal and no guest is left halted; `ciukinit.c`'s `cli; hlt` is now
   only a guest-side fallback for a monitor that did not stop the VM.
8. **Private wire additions** (no new lifecycle selector, no public ABI):
   status 2 CRITICAL; call word +2Ah Retry (0/1; +2Ch–2Fh stay zero); control
   block v3, 24 bytes, +16h offset of the policy byte; identities
   `CIUKIDF2 F3-57 1` and `CIUKIDOS K2043 3` (a VMM sending status 2 must
   never meet an f3-56 driver). `abi.h` changed only the comment of
   `CIUKI_DOSVM_UNIFIED`; `docs/developer-api.md` regenerated.
9. **kernel=unified installation.** `scripts/unified_dos.py` installs the fork
   `KERNEL.SYS`, `CIUKIDF.SYS` and `DEVICE=C:\CIUKIDF.SYS` (before `SHELL=`,
   DOS=LOW unchanged) only from `build/unified-dos/fork-qualified.json`, which
   `run_unified_file.sh` writes after two identical fork builds and passing
   boots; the record, the hashes and the installed bytes are read back.
   Without that record the f3-55 path is unchanged. No default switch.

## Host evidence

`nice -n 19 bash tests/host/dos/run_unified_file.sh` (H57 PASS, 7 boots):

- fork kernel `c0b48a49…a28a45` (71,236 bytes) and CIUKIDF.SYS
  `0854f318…a22d956` (2,672 bytes), identical across two roots/mtimes;
  pristine patch-0001 kernel `d24464ec…92bb22e1`.
- `H57.COM F`: 18/18 tests PASS (classification, files, dup/forced dup,
  copied-DTA search, attributes, rename/delete, directories, 36h, locks,
  6C00h, times, FCB-close size metadata, read-only D:, INT 24h Retry, Fail,
  O_NOCRIT, Abort, sanity after Abort); 105 owned requests, 53 chained,
  3 critical results, 1 Retry, 24 opens = 24 closes, guest INT 13h after
  activation 0 (636 before), native state empty after VM stop.
- `H56.COM N` (f3-56 suite incl. EXEC/TSR/PSP/internals) over the live
  backend: all PASS, guest INT 13h after activation 0.
- Activation refusals: mount failure stops before the shell; `/LOCAL` keeps
  the local FAT; a view beyond LASTDRIVE is refused after the mount and
  unwound (1 mount, 1 unmount).
- Unchanged f3-48 basic battery (27 probes): identical results on the
  pristine kernel (local FAT, 12.9 M instructions) and the fork with the live
  IFS (2.6 M instructions); D48020 fails on both in this BIOS-only harness.

## Lead decisions (f3-57 Amendment 1, 2026-10-21 21:52)

- Decision 5 (GS fix) accepted; upstream report after the 1.0 Alpha (above).
- Status 2 and the +2Ah Retry flag reserved; the f3-56 monitor-only PENDING
  path stays for cases without an INT 24h frame (contract ABI table).
- Identities `CIUKIDF2 F3-57 1`, `CIUKIDOS K2043 3` and control block v3 with
  `/LOCAL` reserved.
- Refused activation stops the VM through the lifecycle gate with the
  `unified_activation_refused` record (implemented 21:54; host cases
  `strict-mount` and `lastdrive` in `unified_file.py`; `local-mount` shows
  that `/LOCAL` does not stop the VM).
- Kept FreeDOS behaviours accepted: 4Dh type 0 after Abort, sharing
  violation 05h, E: read-only.

## Open (not closed here)

- H57-06/07: lead's QEMU pristine/unified differential and T23 DF6 timing.
- H57-08: only 4409h/4408h/32h/1100h cells are executed; 7302h–7305h,
  INT 25h/26h, 4400h/0Ah/0Eh on IFS handles, 1Bh/1Ch and the full C:/D:/E:
  present/absent matrix are not yet run through the fork.
- 15×15 sharing, lock lifetime across processes and DF5 cancel/stop on the
  fork path rely on the f3-49/f3-51 VFS qualification; no new fork-path
  matrix. Entry/return trace records (v1 trace ring) are not emitted by the
  IFS backend; the counters in `struct dfv2_ifs_stats` are its only ledger.
- E: removable media is read-only; a writable policy is not wired.

## 1112h free space: AX is sectors per cluster only (2026-10-24)

FreeCOM `DIR` calls 7303h. K2043 `DosGetExtFree()` (kernel/dosfns.c:887-892)
has no 11A3h extension, so for a redirected drive it copies the raw 1112h
AX into `xfs_clussize`; `do_space` put the media id in AH, making every
cluster 0xF8xx times larger ("801,637 Mega bytes free"). `do_space` now
returns AX = sectors per cluster (the 36h path masks AH itself). AH=36h
values stay clamped to 2 GB - 32 KB (16-bit fields, spc up to 64), as on
Win95 OSR2/98 FAT32 (RBIL Int 21/AH=36h and AX=7303h,
https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-2751.htm).
H57 `space` now also checks 7303h. Limit: K2043 offers no 32-bit 7303h
counts for redirected drives; sizes above 2 GB are clamped.
