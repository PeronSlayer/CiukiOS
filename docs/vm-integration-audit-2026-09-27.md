# VM integration artifact audit — 27 September 2026

The three development tracks did not initially qualify the same artifacts.
One fresh combined lifetime run passed with the video-track module, the then
current HDPMI host and the qualified desktop window payload. That run predates
the subsequently identified scheduler reentry and guest-IF corrections. It is
preserved evidence for its exact inputs, not final acceptance of the integrated
VM or of every client interrupt state.

The compact [audit archive](validation/2026-09-27-integration/README.md) contains
the unchanged reports, module manifests, input identity and invocation. Disk
images, executables, WADs, screenshots, audio and memory captures remain in
ignored `build/`; no program or game payload is included in the archive.

## Artifact matrix at the start of the audit

Hashes below are shortened for comparison. Linked JSON files retain complete
SHA-256 values. Later source edits do not retroactively qualify these binaries.

| Evidence | Session module | HDPMI32I | Shell / DOSWIN | Scope |
| --- | --- | --- | --- | --- |
| [Lifetime `r110`](validation/2026-09-27-integration/lifetime-r110.json) | `5de57c…`, 67,584 bytes | `252d931a…` | `dadb2e…` / `b908b3…`, read from its preserved disk | Repeated clients, real protected video/I/O, fault unwind and recorded ownership checks; earlier source snapshot |
| [VGA acceptance `full4`](validation/2026-09-27-vga/vga-session-acceptance.json) | `5e584d…`, 68,608 bytes | Not this test's protected host | Baseline shell; separate presenter | Native-versus-guest VGA semantics, FIRE, Costa, cleanup; Wolf3D records equivalent insufficient-memory failure |
| [Native window `winrun11`](validation/2026-09-27-vga/vga-dos-window.json) | `5e584d…` | Not a DPMI window-game test | `1edc72…` / `9be2d3…` | Fixed-size window, BIOS-key focus, redraw/minimize, FIRE and cleanup |
| [Legacy V86](validation/2026-09-27-vga/legacy-v86-gate.json) / [DPMI](validation/2026-09-27-vga/legacy-dpmi-gate.json) | `5e584d…` | DPMI test: `72d7adce…` | Baseline shell | Earlier bounded probes on the video-track module |
| [Fresh combined lifetime](validation/2026-09-27-integration/lifetime-initial.json) | `5e584d…` | `ad1a8f83…` | `1edc72…` / `9be2d3…` | PASS with the three tracks' selected runtime artifacts, before subsequent corrections |
| Initial guest-peripheral model report | No JLM | No HDPMI adapter | No desktop execution | 912 host-model assertions and measured synthesized PCM; explicitly no guest execution or hardware acceptance |

Both lifetime versions and the four passing VGA reports use Jemm
`b45af26aca69ce54cb858119ad846bb5778c50bf3a6905ee05ce22b4c3aefa74`
and JLOAD
`550d71d3e5acc671270123998bbb3869c1b2f8b772082ee69a5ac836dad0d16e`.
Those binaries and their adaptation hashes matched the selected Jemm build
manifest. The kernel is
`2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af`
(43,217 bytes).

The [r110 module manifest](validation/2026-09-27-integration/module-r110.json)
differs from the [video module manifest](validation/2026-09-27-integration/module-video-session7.json)
in six source inputs: `session_jlm.asm`, `session_video_abi.inc`,
`virtual_vga.h`, `virtual_vga.c`, `vga_presenter.c` and `session_video.c`.
All 18 source hashes in the video manifest matched the shared checkout when
audited. The later scheduler guard edit intentionally makes that older module
stale for final scheduler qualification.

The HDPMI output's copied assembly adapter, patch, scheduler ABI and modification
notice matched their source files. At this audit point its builder did not emit
a complete source-to-binary manifest covering every C/header input. The builder
now emits that manifest, but exercising the new build and qualifying its runtime
remain pending; the earlier QEMU binary hash does not qualify a later rebuild.
The initial peripheral report matched all ten recorded source/provenance hashes.

## Peripheral namespace and combined link correction

The peripheral API originally reused the presenter's `cvp_` / `CVP_` namespace.
The [actual pre-fix compile](validation/2026-09-27-integration/device-link-before.json)
fails when both headers are included: `CVP_OK`, `cvp_state` and `cvp_init` collide.
Peripheral types, constants, functions, scheduler declarations and DBOPL adapter
names now use **`cvgp_` / `CVGP_`**; the presenter retains `cvp_` / `CVP_`.

The [combined link report](validation/2026-09-27-integration/device-link-after.json)
passes with the real video model, presenter and renamed peripheral sources:
one host executable runs under ASan/UBSan, and the same core objects link as a
freestanding OpenWatcom target. The separate
[renamed model run](validation/2026-09-27-integration/peripherals-renamed.json)
passes all 912 assertions and preserves the measured SB/OPL PCM hashes. These
reports bind their source hashes and supersede the old-prefix report for this
revision. They establish coexistence at compile/link level and isolated model
behavior. They execute no guest through Jemm/HDPMI and provide no native input,
hardware audio, desktop concurrency or frame-rate acceptance.

## Exact workload and combined result

The lifetime workload is the packaged **doom-vanille DOS engine**
`APPS/DOOMVAN/PCDMCORE.EXE`, 601,960 bytes, SHA-256
`efbe64359fb1dfe569cde2428f41ef15a40b8975b8eb36a1cfc5a2731b894980`.
The harness stages it as `DPMIDOOM.EXE` without changing its bytes. It is not
the proprietary original Doom executable and is not recompiled into the
cooperative CiukiOS window port. The JSON field name `original_client` is kept
unchanged as historical test output; it means the unmodified supplied DOS
executable, not a claim about which Doom engine it is.

The new run uses QEMU/KVM with a Pentium III model and an effective 128 MiB RAM
setting. The full invocation is preserved in
[invocation-initial.json](validation/2026-09-27-integration/invocation-initial.json).
Its [prepared input](validation/2026-09-27-integration/input-initial.json) is a
private copy of baseline `08bc6df6…`, with the qualified window shell/runtime
and 43,217-byte kernel installed. Both the baseline and that private input
remained byte-identical after the test. The generated test disk is separate.

The recorded run observes distinct Jemm/HDPMI CR3s, real protected VGA accesses,
630,885 emulated faulting instructions from the unchanged DOS engine, 475 port
reads and 4,551 port writes. It finishes with five matching callback
install/remove and client entry/exit pairs, one deliberate fault exit, zero
retained handle, zero PTE repairs, exact Jemm PTE restoration and exact physical
VGA-aperture restoration. The test's deeply nested DOS parent remains halted
after the final observation; this is not evidence for returning that parent to
the desktop. Window behavior is qualified separately by `winrun11`.

Host ticks advance during the particular CLI and BIOS-wait probe intervals.
That observation does not prove that virtual IF stays clear, that guest IRQ
delivery is suppressed throughout CLI, or that all V86 IOPL/POPF paths preserve
the required physical-versus-virtual interrupt distinction. Those are explicit
remaining integration gates.

## Scheduler reentry regression

The earlier `vm_scheduler_irq` overwrote its global saved ESP and switched to
the shared private stack before acquiring the busy guard. A nested call could
therefore damage the outer callback before being rejected.

`scripts/test_session_scheduler.py` extracts the actual production IRQ routine,
validator and storage declarations, assembles them with pinned-source JWasm,
and executes them in Unicorn. It models flat segments, descriptor/PTE memory
and one nested CALL at the validator entry. It does not simulate a complete
interrupt controller or claim hardware/QEMU IRQ reentry.

The [pre-fix run](validation/2026-09-27-integration/scheduler-before.json) fails
with `nested callback overwrote saved outer ESP`. The
[corrected run](validation/2026-09-27-integration/scheduler-corrected.json),
source SHA-256
`c75cc8e0f6431a77688287d0ae3777c86591106f0937478427eedaa8008d471c`,
passes preservation of the live outer frame, saved ESP, GPRs, segments and flags;
one recorded reentry with one outer service/PTE check; normal repeated service;
and release of the busy guard only after restoring the caller stack.

Final runtime acceptance must use a new source-bound build after the guard and
guest-IF fixes, and explicitly verify host progress together with unchanged
guest interrupt masking. The initial combined report cannot satisfy that gate.
Integration of the renamed peripheral model with real V86/HDPMI traps, focused
input and host audio remains separate work.

## Corrected session module runtime checks

`UNBIND_FB` now rejects an active session with `VM_ERROR_ACTIVE`. Releasing its
physical mapping before retiring the presenter/callbacks could leave a live
writer pointing into freed address space. `VMNEG.COM` exercises that refusal;
`VGAHOST.COM` additionally attempts it against the real, bound framebuffer
before presenting the subsequent guest frames and explicitly ending/unbinding.

The [cleanup gate](validation/2026-09-27-integration/cleanup-gate.json) passes
with module `afb651…`: ordinary DOS COM/MZ, file access, negative lifetime
operations, bounded framebuffer copy, exact PTE restoration, unload and desktop
return. The [VGA integration run](validation/2026-09-27-integration/vga-integration.json)
passes with module `9c816e…`: five native-versus-guest VGA pixel checkpoints,
FIRE palette/frame observations, Costa pixels (documented clock exclusion),
PTE/BDA/vector/physical-aperture restoration and desktop return. Wolf3D remains
an equivalent native/session insufficient-memory result. FIRE records 18.1
present operations per second; it is not a 30 fps or hardware performance result.

These two module binaries have identical executable bytes apart from one byte
in the PE/COFF timestamp at offset 128. Both identities remain recorded as
actually executed; neither report is relabelled. The
[later build manifest](validation/2026-09-27-integration/module-integration.json)
also records compiler/include inputs and rejects any change to those inputs
during compilation. Both gates use the ordinary `b45af26a…` Jemm build, not
the experimental V86 virtual-IF profile. They do not qualify the pending new
HDPMI interrupt adapter or peripheral wiring.

## Completion runs: rebuilt HDPMI, guest IF and combined acceptance

The audit left three items open: the new HDPMI build had never been run, the
combined lifetime gate had not been repeated with the corrected scheduler, and
guest-IF behaviour had not been qualified together with HDPMI. All three were
closed on QEMU/KVM Pentium III, 128 MiB. Each failure found on the way is
archived next to the passing report that superseded it.

### Defects found and fixed

| Evidence | Symptom | Cause and fix |
| --- | --- | --- |
| `failure-hdpmi-image-over-64k.json` | HDPMI never became resident; Jemm reported #GP at `22A6:00010000` in V86 | The guest-IF adapter moved `_TEXT32R3` to RVA 10000h, and HDPMI addresses it with 16-bit offsets. The three C objects inside HDPMI are now compiled with `-os`, and `build_hdpmi_host.sh` refuses any section ending above RVA 10000h |
| `failure-if-adapter-iret-abort.json` | The deliberate-fault client was killed at an `IRETD` of the DOS/4GW exception path | Same-privilege IRET/IRETD is now emulated during a stepped region. It validates the CS descriptor and limit, refuses NT/VM images and applies the image's logical IF |
| `failure-if-adapter-stale-tf.json` | doom-vanille received exception 01h in DOS/4GW code | A service resume set by `INT 21h`/`INT 31h` was consumed by an unrelated later host return, which armed the adapter's TF there. The resume is now bound to the exact return CS:EIP and cancelled when the region ends |
| `failure-if-adapter-step-budget.json` | A client was killed after 1,048,576 single steps inside a legal 3-tick CLI wait | The limit is removed. Physical IF stays 1, so host ticks keep running; the count is diagnostic only |
| `failure-profile-popf-nt.json` | With the V86 IF profile, HDPMI's CPU detection (POPF with bits 12–15) was fatal | The profile kept NT out of emulated POPF/IRET. It now keeps NT as upstream IOPL=3 Jemm does |
| `failure-profile-vcpi-cr3.json` | Ring-0 #PF loop in Jemm's VCPI PM→V86 path | The profile read `CvActive` before switching back to Jemm's CR3; the check now follows the switch |
| `failure-profile-eoi-ownership.json` (+ `-ring.txt`) | doom-vanille stalled: physical IRQ0 permanently in service | EOIs from V86 were consumed by a stale virtual level. A V86 EOI now goes to the physical PIC when that PIC has a level in service or no virtual level is open (see the profile record) |
| `failure-costa-focus-nondeterminism.json` | Intermittent Costa mismatch of one cell | Program-state variance of the keyboard-focus highlight on both native VGA and the session; the test now reports that cell separately and captures only settled frames |

The ordinary Jemm build (`--ciukios-device-query --ciukios-vm-scheduler`) is
unchanged by these profile fixes: it still reproduces JEMM386
`b45af26a…` and JLOAD `550d71d3…` byte for byte.

### Final artifacts

| Artifact | SHA-256 |
| --- | --- |
| HDPMI32I (patched 3.24), as tested in the gates below | `3af7ae37fb5fadb27f8923c931ce441d9edb377177488b7d0e1f65a3cfcd4630` |
| HDPMI32I, rebuilt with the final notice ([manifest](validation/2026-09-27-integration/hdpmi-build-final.json)); differs only in the 2 PE TimeDateStamp bytes at file offset 1EF9h, and passes the lifetime gate under both Jemm builds | `6a3f53a48f1336b1b8893f36bafb310697de7a3c9fd09658084e990b34ca11b0` |
| CVSESSION.DLL ([manifest](validation/2026-09-27-integration/module-final.json)) | `2bdaf7e2e8537de2d84f20c28beeb7116503dc371143b8f3cb7d1b5acb1e59b6` |
| JEMM386, ordinary | `b45af26aca69ce54cb858119ad846bb5778c50bf3a6905ee05ce22b4c3aefa74` |
| JEMM386, V86 IF profile ([manifest](validation/2026-09-27-integration/jemm-profile-build-final.json)) | `caf3c73f6f7e287a8316e264ec344c7ee1da08ae53d04a219d7b8d15e70bc8a5` |
| SHELL.COM / DOSWIN.DRV (current sources, 60,912 / 34,480 bytes) | `8093b03b…` / `793fa6a3…` |

### Final results

| Gate | Ordinary Jemm | V86 IF profile |
| --- | --- | --- |
| HDPMI lifetime: repeated clients, virtual-IF contract (CLI/STI/PUSHFD-POPFD/0900h–0902h, IRQ suppression during CLI while host ticks advance), deliberate #UD unwind, unchanged doom-vanille timedemo, exact PTE/aperture restoration | PASS | PASS in 3 of 3 runs |
| V86 CLI/IRQ contract (`V86CLI.COM`) | — | PASS |
| VGA acceptance: 5 pixel checkpoints, FIRE, Costa, Wolf3D equivalence, exact cleanup | PASS | PASS |
| Native DOS window: checkpoints, focus, cover/minimize, FIRE in window, vector restore | PASS | PASS |
| Legacy V86 session gate | PASS | PASS |
| Legacy DPMI video gate | PASS | — |
| DOS-window refused-cleanup lifecycle (Unicorn, 13 cases) | PASS | — |

FIRE in the window runs at 18.2 paints/s with a 30.3 ms mean paint under both
Jemm builds. The full-screen presenter runs at 18.3 presents/s with a 6.7 ms
mean present. These are QEMU/KVM figures on a 4.2 GHz host; they are not
Pentium III, T23/E500, 30 fps or hardware-acceleration results.

### Still open at completion (resolved the same evening, except the last two)

- ~~The V86 IF profile is selected by build hash; no runtime negotiation.~~
  Negotiated through `Host_Scheduler_Profile`, with one Jemm build.
- ~~The peripheral model is not connected to guest traps or host audio.~~
  Connected for V86 programs. The profile's PIC is the single owner and the
  model's 8259 only aggregates.
- ~~The DOS window is fixed-size, keyboard is INT 16h only, and there is no
  guest mouse.~~ Resizable in VGA session mode, raw IRQ1 keys, virtual INT 33h.
- Protected-mode (HDPMI) clients do not use the device model.
- There is no physical-hardware qualification.

The follow-up evidence, with the final hashes, is in
[validation/2026-09-27-devices](validation/2026-09-27-devices/README.md).
