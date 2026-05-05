# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise. Every completed task must update the `Unreleased` section unless the task is a release cut that creates a new version section.

## Unreleased (2026-05-05)
1. Added FAT16 per-drive CWD mirror state for C: and D: when INT 21h changes the default drive, kept chdir updates synchronized with the active drive slot, extended DOS21 smoke coverage for explicit C:/D: getcwd paths, added a full-CD shell drive QEMU lane for D: prompt/CWD validation, and expanded shell stability stress loops for repeated invalid commands plus COM/MZ execution, then added DOS-like drive-qualified chdir handling for C: and D: absolute paths, C:REL/D:REL relative paths, parent traversal, and invalid drive rejection without changing the default drive, and split INT 21h AH=38h country-info handling from AH=67h set-handle-count compatibility so DOOM-era callers no longer hit an unsupported or empty country-info path, and confirmed the only observed AH=44h IOCTL failure class is AL=06h input-status by adding DOS21 smoke coverage for that subfunction without broadening generic IOCTL behavior, and initialized COM/MZ PSP handle tables with DOS-standard inherited handles 0-4 plus closed entries so programs that inspect or close inherited handles no longer see an all-zero PSP handle map.
2. Slimmed the public README quick links and removed the legacy in-repo `OLD/` archive from the current project tree, keeping the repository focused on the active legacy BIOS x86 codebase and documentation.
3. Published the GitHub Sponsors support link in `DONATIONS.md`, surfaced it from the README support section, and added `.github/FUNDING.yml` so GitHub can expose the Sponsor button for the repository.
4. Added DOOM taxonomy `visual_gameplay` screenshot validation for fresh QEMU P6 PPM captures, separating missing/stale screenshots from low-diversity visual failures.
5. Removed the obsolete `build-profiles/` stub documentation directory now that active profile guidance lives in the Makefile, README, roadmap, and agent directives.
6. Removed obsolete historical smoke notes, unused screenshot artifacts, the stale ASCII splash source, and the unreferenced standalone CIUKEDIT smoke script while preserving active full/full-CD build, setup, driver, and runtime paths.
7. Added an opt-in DRVLOAD evidence mode that can attempt DEVLOAD.COM and log DEVLOAD/MSCDEX child termination results while keeping the default driver smoke path fail-open and unchanged, and made shell `run` forward command tails through the INT 21h AH=4Bh exec parameter block so evidence-mode arguments such as `/DEVLOAD` reach child PSP tails.
8. Added observed INT 21h compatibility for AH=20h legacy termination, AH=37h switch-character queries, and AH=55h child PSP creation, fixed COM termination trampoline selection for programs that change the current PSP before exit, corrected DOS allocator handling for freed/non-overlapping MCB table entries and overlapping allocations, and moved DEVLOAD evidence forward to the real SYS device-driver EXEC boundary (`INT 21h AH=4Bh` returning 000Bh) using a smaller real CD driver payload.
9. Added an isolated full-profile `CIUKWIN.COM` Windows 3.11-inspired GUI preview branch plan and packaged demo app without changing the runtime shell or Stage1 path.
10. Rebaselined active-profile validation around the full and full-CD lanes by making `make qemu-test-all` run only current active profile smoke tests, documenting the DEVLOAD `.SYS` execution boundary, and recording a full/full-CD stability matrix with build, shell, driver, setup, and DOOM taxonomy validation.

## pre-Alpha v0.6.3 (2026-05-05)
1. Added a `runtime_stable` DOOM taxonomy stage after `video_init` to classify post-video observation stability separately from startup progress; a 120s visual headless run now fails explicitly on QEMU SIGSEGV instead of being masked by earlier `video_init=PASS`.
2. Added CHS fallback boot paths for the full-CD MBR and full stage0 when booting via direct El Torito hard-disk emulation, enabling the faster non-ISOLINUX real-hardware ISO to reach Stage1 in QEMU.
3. Fixed full-CD hardware validation by accepting valid short reads of `SYSTEM/STAGE2.BIN` during Stage2 autorun and making the CD hardware profile leave PS/2 mouse controller initialization disabled by default to preserve legacy keyboard input.
4. Added a disposable HDD install validation lane that writes the full-profile MBR, FAT16 partition, and boot chain to `build/full/setup-hdd/target-hdd.img`, verifies the partition geometry and FAT directories, and boots the image standalone in QEMU.
5. Integrated the direct El Torito hard-disk CD image into `scripts/build_full_cd.sh` and added a disposable CD-to-blank-HDD probe lane that boots the direct CD with a separate blank target disk attached while verifying the target remains unchanged.
6. Added a serial-only read-only BIOS HDD probe in `SETUP.COM` and extended the CD-to-blank-HDD QEMU lane to launch setup, verify the probe masks, and stop before any destructive install step.
7. Added a build-gated `SETUP.COM` raw runtime HDD install path for the disposable QEMU topology: `qemu-test-setup-runtime-hdd-install` enables `CIUKIOS_SETUP_RAW_HDD_INSTALL=1`, clones the direct-CD hard-disk image from BIOS `80h` to blank BIOS `81h`, verifies `[SETUP-HDD-INSTALL] START/DONE`, checks MBR/FAT16/mtools readability, and boots the installed HDD standalone. Normal full-CD builds leave the raw install path disabled unless explicitly enabled.
8. Promoted the full-CD build into the main Live/install media path: CD/QEMU boots default to D:, installed HDD boots default to C:, SETUP enables destructive raw HDD installation with a typed DESTROY confirmation, SETUP patches the installed Stage1 default back to C: after cloning, and the shell now includes a drives command for quick unit visibility.
9. Improved Live/install usability by showing a SETUP connected-disk map for BIOS 80h/81h roles, making FAT path lookup preserve typed case, updating QEMU harnesses to send uppercase FAT paths explicitly, fixing the Live CD shell prompt to display the DOS default drive D:, and realigning the installed Stage1 D-to-C patch offset after the prompt code change. Added an explicit `qemu-run-full-cd` visual runner and `qemu-test-full-cd` smoke target so QEMU Live CD boots use the direct ISO path and validate the D: prompt instead of the installed-HDD C: profile.
10. Fixed FAT16 shell footer RAM telemetry to show available DOS heap memory instead of subtracting it into a misleading `RAM:2K` used-memory value, and restored DOS-program FAT compatibility by uppercasing external INT 21h path lookups while keeping shell path commands case-sensitive.
11. Added convenience wrappers `scripts/build_run_full.sh` and `scripts/build_run_full_cd.sh` to run build+run in one step for the full and full-CD profiles.
12. After any DOS program exits, the shell now automatically restores the pre-execution working directory and clears the screen, returning the user to a clean shell at the correct path.
13. Replaced tick-counter CPU% estimator in `shell_footer_compute_cpu_pct` with a per-tick idle-loop-count high-watermark algorithm (`shell_footer_loop_count` / `shell_footer_max_loop`); removed unused `shell_footer_cpu_idle_ticks` / `shell_footer_cpu_busy_ticks` variables; renamed RAM footer label from `RAM:` to `FREE:` and adjusted column offsets (75→74, +4→+5) for the longer prefix.
14. Added `woof` as a CiukiOS-specific alias for the `cd` shell command, changing the current working directory without altering existing `cd` behavior.
15. Added a full-profile shell stability QEMU lane that validates `woof` as `cd`, CWD preservation after DOS program exit, prompt recovery, and mixed-case path rejection.
16. Extended the full-profile shell stability harness with deterministic empty-input, invalid-input, edit-key, tab-key, and repeated COM/EXE execution recovery coverage.
17. Expanded FAT16 INT 21h drive semantics so C: and D: are accepted for default-drive and free-space calls, aligned the full and full-CD profiles with the active C:/D: runtime model, and extended the DOS21 smoke path to validate the new drive/free-space behavior.

## pre-Alpha v0.6.1 (2026-05-04)
1. Added a full-profile DOOM taxonomy harness and Makefile target to classify launch progress stages deterministically.
2. Added local-only DOOM payload packaging in the full image build lane and guarded proprietary assets from publication.
3. Fixed INT 21h MZ loading to use header-declared module size, removing the previous 4B:08 launch failure and advancing DOOM to extender startup diagnostics.
4. Closed the Phase 4 installer execution lane with deterministic scenario coverage (success, media swap, timeout, missing media, and insufficient space).
5. Hardened installer manifest-source diagnostics, including explicit `MANIFEST_MEDIA_HEX` reporting for normal and fallback parse paths.
6. Synchronized project documentation to reflect installer-lane closure while keeping the runtime/DOOM lane active.
7. Improved README changelog visibility and updated local agent directives to require a `CHANGELOG.md` update for every completed task.
8. Advanced the DOOM taxonomy harness to boot the full profile interactively, invoke `DRVLOAD.COM`, and launch `DOOM.EXE`, adding a deterministic `doom_exec_attempted` stage before extender/video/menu gates.
9. Advanced DOOM runtime coverage by adding an MZ transfer stage, FAT16 32-bit seek/read file positions, real handle duplication for DOS extender loaders, and DOOM-specific environment executable path handling; stricter taxonomy in item 13 now separates DOS/16M banner detection from the remaining `tstack` blocker.
10. Improved DOS/16M conventional-memory bring-up for DOOM by repairing the INT 21h MCB arena and setting PSP:0002 to `DOS_HEAP_LIMIT_SEG` on resize success; later stricter taxonomy in item 13 reclassifies the remaining `tstack` allocation error as still open.
11. Added a full-profile DOOM loader fallback for `DOOM.ETX` self-reopens, preserved PSP free-tail allocation state while keeping DOS/16M-compatible PSP limits, and implemented XMS `move_emb` via BIOS INT 15h AH=87h; intermediate stricter DOOM validation blocked at DOS/16M `tstack` after MZ transfer and before video init.
12. Rebuilt the full-profile INT 21h MCB arena from an ordered chain source after allocation, free, and resize operations: PSP MCB sizing remains separate from compatibility `PSP:0002`, heap resize limits preserve adjacent MCB headers, invisible bump allocations are disabled, PSP resize no longer relocates caller-owned blocks, and post-block2 free gaps are allocatable by slot3.
13. Tightened the DOOM taxonomy harness so DOS/16M `tstack` allocation errors fail `extender_init` instead of producing a false extender pass, and added persistent INT 21h AH=58h memory-strategy get/set state; intermediate DOOM validation was honestly classified as blocked at DOS/16M `tstack` before `video_init`.
14. Aligned full-profile MZ process memory setup closer to DOS semantics by sizing the initial PSP block from MZ minalloc/maxalloc, keeping PSP:0002 as the compatible top-of-memory value while tracking the real MCB end internally, and reserving the below-heap MCB gap; intermediate diagnostics showed DOS/16M AH=4Ah/48h/4Ah calls succeeding while the runtime still reported the tstack blocker before video_init.
15. Introduced a compact ordered DOS memory block table to drive MCB chain rebuild, next-allocation scanning, and largest-gap reporting, and persisted the low free gap exposed by AH=49 block promotion so AH=48 can consume it; full and stage1 smoke lanes remained green during this intermediate step while DOOM taxonomy still failed honestly at DOS/16M tstack.

16. Converted INT 21h AH=48h/49h/4Ah memory mutation to the ordered DOS memory block table as the allocator source of truth, including reusable FREE entries, legacy mirror synchronization, and allocator register preservation; full and stage1 lanes passed, while fresh DOOM taxonomy reached MZ transfer without the prior tstack marker but still lacked an extender marker at that point.
17. Fixed INT 21h AH=33h Ctrl-Break get/set compatibility so DOS/4G can preserve AH across its get-then-set sequence instead of accidentally invoking AH=00 terminate, removed the stale `DOOM.ETX` to `DOOM.EXE` path rewrite, and tightened taxonomy to fail DOS/16M executable-validation errors; DOOM now advances past the immediate return-to-prompt path and blocks honestly on `not a DOS/16M executable`.
18. Fixed FAT16 INT 21h read/seek return-value preservation across handle-slot swaps so DOS/4GW sees correct `AX` byte counts and 32-bit seek positions, expanded the ordered MCB table to 16 entries for DOOM's later runtime allocations, and updated the DOOM taxonomy launch to run from `\APPS\DOOM` with DOS4GW/video markers; full-profile DOOM taxonomy now reaches `video_init=PASS`.
19. Added a full-profile DOOM visual taxonomy lane using QEMU `-display none` and optional monitor `screendump` capture, removed the false `M_Init` menu marker, and captured post-status-bar gameplay evidence at `build/full/doom_post_status_after_marker_fix.png`; serial taxonomy remains honest at `video_init=PASS` with `menu_reached=DEFERRED`.
20. Closed the Phase 4 DOOM gameplay playable milestone and bumped the public project version to `CiukiOS pre-Alpha v0.6.1`; manual owner validation confirms DOOM is playable on the generated full-profile image.

## pre-Alpha v0.5.4 (2026-05-01)
1. Improved shell input stability for hold-key repeat, line wrap, and backspace behavior.
2. Stabilized FAT16 shell footer telemetry (`CPU/DSK/RAM`) with corrected non-stuck stat refresh behavior.
3. Revalidated cross-profile build and regression lanes on floppy (FAT12) and full (FAT16) profiles.
## pre-Alpha v0.5.3 (2026-04-30)
1. Stabilized shell `move/mv` path-command handling across floppy (FAT12) and full (FAT16) runtime profiles.
2. Fixed `INT 21h AH=56h` rename/move behavior to preserve deterministic DOS file-move semantics.
3. Revalidated shell command regression lanes after the fixes to confirm runtime stability.

## pre-Alpha v0.5.2 (2026-04-29)
1. Closed Stage1 DOS command regressions across floppy (FAT12) and full (FAT16) profiles.
2. Stabilized INT 21h runtime return behavior in critical read/write/seek paths for deterministic file I/O outcomes.
3. Fixed floppy image build write path to preserve root/runtime payload integrity during regression runs.

## pre-Alpha v0.5.0 (2026-04-28)
1. Stabilized Stage1 startup and shell-entry runtime behavior for more deterministic boot bring-up.
2. Hardened shell prompt/input paths, including drive and current-directory state handling.
3. Improved QEMU stderr observability to speed up runtime diagnostics and regression triage.

## pre-Alpha v0.5.0 (2026-04-22)
1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Reset the previous codebase state to start from a clean legacy BIOS x86 architecture baseline.
3. Introduced two official build profiles: `floppy` (1.44MB bring-up) and `full` (extended runtime), both shell-first.
4. Added build and QEMU smoke-test scripts for both image profiles.
5. Established branch discipline and documentation standards for reproducible delivery.
6. Set real floppy bootability and shell reliability as the baseline acceptance criteria.
