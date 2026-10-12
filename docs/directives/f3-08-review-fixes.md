# f3-08 — Fixes from the kernel review of 169dde64..af6369cd (before the 0.9.0 hardware image)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-08` (branch `wt/f3-08` from `wt/integ2` 755a4954).
- **Scope:** host-testable kernel fixes for the findings below. The lead runs QEMU (`f3-workloads` doom/doom-sound/wolf3d, `f3-vm` dpmi and vm-video, `f0-smoke`) and the T23.
- **Input:** the review in `build/codex/kernel-review-input.md` (read-only review of the night's kernel diff by Codex; sections 1–6).

## Method

For each finding: first reproduce it with a host test (or show by code reading, with the exact lines, why it cannot occur); fix the confirmed ones; refute the others in the report with that evidence. Findings the review lists only as "risk" without a demonstrated defect (fs_lock, the static workload table, the work report limit) get tests where cheap, no behaviour change.

## Findings to handle

1. **DPMI virtual IF, TF cleared by the stepped instruction** (`src/kernel/vm/dpmi_dispatch.c` ~343, 475). An armed POPF followed by an instruction that itself loads TF=0 (second POPF/IRET) raises #DB with TF clear in the saved flags, and `flags_restored()` refuses it: virtual IF stays disabled and the default exception path can terminate the VM. Use DR6.BS as the discriminator (`dosvm.c:240` reports DR6 only when DR7 is set: report it always), and disarm `if_armed` as soon as the stepped instruction has been handled so that later unrelated TF exceptions are never consumed. Keep the MOV/POP SS delay in mind (the #DB lands after the following instruction).
2. **DPMI backward signature match** (`dpmi_dispatch.c` ~318, 333). The PUSHF/POP AX detection scans backwards from the CLI and can match immediate bytes (`MOV BX,589Ch; CLI` = `BB 9C 58 FA`), corrupting AX or the stack. Never modify AX or the stack on a backward byte match alone: decode forward from a known instruction boundary (the previous trap's fault address or the client entry) and require the decoded instruction preceding the CLI to be PUSHF/POP.
3. **VGA burst, partial REP element** (`src/kernel/vm/dosvm_vga.c` ~355, 381–384). A REP string store that leaves the aperture (or any burst element refused mid-way) is abandoned after partial model side effects, so rotate/XOR write modes see the first element twice when the native CPU replays it. Either preflight the whole element (addresses and forms) before committing anything, or commit the register state of the completed REP elements (ECX/ESI/EDI advanced, flags) before stopping, so native replay never repeats a committed element. Cover both directions (DF=0/1) and the 9FFFFh/B0000h boundaries.
4. **VGA burst continuation of faulting forms** (`dosvm_vga.c` ~381; `vga_x86.c` ~596). LOCK and other prefix/form combinations that the native CPU would fault on (#UD) are executed by the burst instead of faulting. Reject them before execution (the native path then raises the fault); decoder test.
5. **Power, retry after a failed sync** (`src/kernel/core/power.c` ~235). After `storage_sync()` fails (EBUSY during a boot-log drain, EIO) the second `ciuki_power` request must not skip synchronisation: keep and return the terminal error, retry the sync, proceed only when it succeeds. No reboot/poweroff with dirty data or open files.
6. **Power `_S5` parsing** (`power.c` ~56–63, 87–100). Decode PkgLength and the package extent; accept only a root-scope `_S5` Name whose Package has the expected elements; reject matches inside Buffers or nested scopes; `power_test.c:93` currently expects success for a zero-length package, which must fail. Reference: ACPI 6.6 §20 (AML grammar).
7. **Power SCI_EN** (`power.c` ~209–216). After the ACPI enable handshake require SCI_EN set (PM1a and, when present, PM1b) before any PM1 control write; on timeout log it and fall back to the reboot path. QEMU ignores SCI_EN, so the host test must cover it.
8. **Audio tail errors** (`src/kernel/audio/audio_dev.c` ~128–137, `drivers/ac97.c` ~145–165). All progress errors during the tail are treated as recoverable underruns; an AC97 EIO on an invalid CIV/PIV regresses the fetched position and a reset can discard queued non-silent VM quanta. Assess with the existing audio host harness: if dropped non-silent quanta can be demonstrated, recover only from a proven silent drain and reset the device otherwise, keeping the `f4-audio` vm-audio pin ranges plausible (rms, pacing); else document why in the report.

## Constraints

clang i686 `-Werror`; no change to the record grammar or to existing record names; existing host suites stay green (`scripts/test/host_kernel_tests.sh`, `python3 -m unittest discover -s tests/host`); no QEMU, no `systemd-run`, no image builds, no commits, no diary entry, no files in the repository root, no files outside the worktree (scratch under `build/`).

## Deliverable

`build/codex/f3-08-report.md`: a table finding → confirmed/refuted → host test name → fix summary (file:line), then the verbatim summary lines of the host suites. List every file changed.
