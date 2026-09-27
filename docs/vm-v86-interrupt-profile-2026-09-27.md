# Experimental V86 interrupt profile

This is an extension to the pinned Jemm monitor for one foreground CiukiOS
session. It keeps the physical timer service running during guest `CLI`
without prematurely enabling the guest's interrupts. It does not create
another VM. Since 27 September 2026 (evening) the profile is **negotiated at
runtime** by its owner instead of being selected by a build hash, and it is
the single virtual PIC through which the session's peripheral model raises
guest IRQs (see [input and sound devices](vm-input-audio-devices-2026-09-27.md)).

## Why the physical callback was insufficient

Upstream Jemm uses `V86IOPL equ 3`. The previous scheduler patch called its
callback when a physical IRQ arrived, but ordinary V86 `CLI` still disabled
physical interrupts. A busy loop therefore starved the host callback too.
The old comment claiming that placing the hook before reflection fixed this
was incorrect and has been removed.

The actual counterexample is retained in
`build/tests/v86-cli-counterexample-2026-09-27-r3/report.json`. Under
QEMU/KVM, Pentium 3, 128 MiB and `NOVME`, host/BIOS timer deltas were:

| Phase | Host callback ticks | Guest BIOS ticks |
| --- | ---: | ---: |
| Before CLI | 13 | 13 |
| CLI busy loop | 0 | 0 |
| After STI | 25 | 25 |

The measured loop contains only `RDTSC`, arithmetic and branches. No HLT,
DOS, BIOS, I/O, host memory injection or CPU pause makes it yield.
`--expect-starved` means that reproducing this failure is the expected result;
it is explicitly not a passing scheduler implementation.

## Selection and implementation

```sh
scripts/build_jemm_monitor.sh --output build/external/jemm-monitor \
  --ciukios-device-query --ciukios-vm-scheduler
```

There is one Jemm build. `--ciukios-vm-scheduler` now always compiles the
profile (`CIUKIOS_V86_INTERRUPTS`); the old separate `--ciukios-v86-interrupts`
switch is accepted but implied. Compiling it in changes nothing by itself:
the profile is **active** only while the exact `Install_Host_Scheduler`
callback owner is installed **and** that owner has requested it through the
runtime service below. Releasing the request or removing the callback
restores the owned IOPB bits and physical PIC masks. The current build is
JEMM386 SHA-256
`486d1453850f75f17124707ee25e1e7230e23ce81ef42bfc1d8ccefc2b99890a`, JLOAD
`b76e057a545d6560e1d56fd1d5343bbe79d9a382bafab4fb10eb8e3b44125512`, patch
`de747a75…`. Earlier records cite the former separate builds
(`b45af26a…` without and `caf3c73f…` with the profile); they are historical.

While active, monitor frames contain logical guest FLAGS. Immediately before
returning to V86, the monitor materializes physical IF=1 and IOPL=0. The
software decoder handles CLI/STI, PUSHF/POPF/IRET in both operand sizes and
INT imm8. Stack operations retain 16-bit SP wrap between words and reject a
word crossing the segment limit. The one-instruction STI shadow handles both
an ordinary instruction and one which itself traps for monitor emulation.
The artificial TF used for that shadow is not exposed by emulated PUSHF.

Physical interrupts enter a pending set and receive a host PIC acknowledgement;
guest reflection waits for logical IF, its interrupt mask, fixed PIC priority,
and the end of the STI shadow. Virtual ISR state is cleared by the guest's
virtual EOI. A V86 EOI goes to the physical PIC instead when that PIC still has
a level in service, or when no virtual level is open: profile claims are
acknowledged at once, so such a level belongs to an IRQ that a VCPI client
(HDPMI) accepted and reflected to real mode. Without this rule a doom-vanille
tick whose handler EOIed in protected mode (DOS/4GW pass-up) left a virtual
level open, the next reflected BIOS tick's EOI was consumed virtually, and
physical IRQ0 stayed in service for good (`failure-profile-eoi-ownership`). The monitor's HLT/Yield IRQ path follows the same rule. The slave
PIC is arbitrated at master priority 2; spurious IRQ7/IRQ15 have separate
acknowledgement handling. The focused test exercises IRQ0, not all slave IRQs.

VCPI return retains the specified logical IF=0 while the active Jemm profile
uses physical IF=1/IOPL=0. That flag decision is taken only after the switch
back to Jemm's CR3: before it, the profile variables are not mapped in the
VCPI client's page tables and HDPMI's startup faulted in a ring-0 #PF loop
(`failure-profile-vcpi-cr3`). Emulated POPF/IRET keep NT as upstream IOPL=3
Jemm does; a VM=1 IRET never takes the task-return path and ring 0 never
loads client flags. Rejecting NT had stopped HDPMI's CPU detection
(`failure-profile-popf-nt`). A protected-mode HDPMI client is a separate
CR3/interrupt owner with its own adapter (see the integration audit).

## Runtime negotiation

JLM code calls the new VMM service `Host_Scheduler_Profile` (declared in
`JLM.INC`, dispatched through a Jemm table slot that is 0 when the profile is
not compiled in, in which case the service returns EAX=0). EAX selects the
function; the result is `EAX = 01000000h | flags` (version 1.0) with CF=0, or
CF=1 for an invalid request.

| EAX | Function |
| --- | --- |
| 0 | Query only |
| 1 | Request the profile for the installed callback owner |
| 2 | Release: clears the request, the IRQ filter and the poll procedure |
| 3 | Raise virtual IRQ ECX (0–15 except 2); only while active |
| 4 | Set the IRQ1/IRQ12 filter procedure (ECX, 0 = none) |
| 5 | State: EDX = virtual in-service mask, ECX = pending mask |
| 6 | Set the return-to-V86 poll procedure (ECX, 0 = none) |

Flags: bit 0 available, bit 1 requested, bit 2 active, bit 3 logical guest IF,
bit 4 filter installed, bit 5 poll installed.

CVSESSION operation `VM_OP_IF_PROFILE` (0Ah) exposes this to its owner:
BX=0 queries, BX=1 allows (the default) and BX=2 declines. The scheduler
requests the profile when it is armed unless the owner declined, and releases
it when it is disarmed. `VM_OP_QUERY` reports `VM_CAP_V86_VIRTUAL_IF` (80h)
only after the service answers, never from the callback pointer or
`CVSCHED_FLAG_V86_IRQ`, which still means only that the physical callback is
installed.

When the profile deactivates while an unmasked virtual IRQ is still pending
(for example a keyboard byte raised just before `DEV_END`), Jemm keeps it
active for up to 10000h further monitor exits until the guest has taken the
IRQ. Dropping it had left the physical 8042 output buffer full and the desktop
keyboard and mouse dead.

## Device-model ownership

There is exactly one virtual PIC: the profile's. The peripheral model
(`guest_peripherals.c`) keeps its own 8259 only as an aggregator in auto-EOI
mode; each IRQ it produces is forwarded with function 3. Its PIT channel 0 is
stopped and IRQ0/IRQ2 are never forwarded, so the guest timer remains the
physical IRQ0 path. Function 4 lets the session see physical IRQ1/IRQ12
before they would be reflected, so the model captures 8042 bytes instead of
the guest reading the controller directly. Function 6 runs the session's
poll (audio buffers, 8042 drain) just before returning to V86, rate-limited by
the session to 0.5 ms.

## Focused qualification and limits

`scripts/qemu_test_v86_cli.py` builds `src/probes/vm/v86_cli.asm` and runs it
on a private disk image. Its ordinary mode requires:

- Physical host ticks continue while guest timer delivery is disabled.
- Guest timer delivery resumes after STI and POPF/POPFD.
- Actual INT21, IRET/IRETD, and a separately allocated SS with wrapping
  interrupt frames work.
- The ordinary guest IRQ0 handler observes the instruction after STI as
  completed, and sees coherent guest IOPL bits in its saved FLAGS.
- Masking virtual IRQ0 leaves the physical timer service live; withholding
  EOI blocks subsequent guest deliveries until explicit EOI.
- CLI plus HLT does not leak guest IRQs.
- A blocking BIOS INT16 call receives a real QEMU keyboard event.
- Physical PIC masks are restored, JLM/Jemm unload, COMDEMO executes again,
  and the source disk image is unchanged.

`V86CLI.COM` now also checks the capability and a full allow → decline →
release → allow cycle through operation 0Ah, and prints the service flags
(`0100000F`: available, requested, active, guest IF). Final record:
`build/tests/vm-final-2026-09-27/v86cli/report.json`
([archived](validation/2026-09-27-devices/v86cli.json)).

The earlier focused record is
`build/tests/v86-cli-profile-2026-09-27-final-r2/report.json`. It uses Jemm
SHA-256 `8ed34f7fe5e9cc85403cbee92ad08eea7c9386ff41bc9b378be8b5718feb62ed`
from `build/external/jemm-v86-interrupt-profile/work-mlko8ckw/output/`, and
shows 13/0 host/guest ticks during CLI and 13/13 after STI. The real guest
fixture is SHA-256
`40cad2948c3d507f244004cf808ea0d1485754f147e00b6acc0901d4a6542a92`.
Later combined DPMI/game runs must
identify their own exact artifacts and results; these focused records do not
prove them.

The profile deliberately rejects PIT0 divisor reprogramming and unsupported PIC
commands such as priority rotation/special-mask operation through Jemm's
visible exception path. It does not yet provide a
recoverable per-task error for these cases. It is not a complete 8259/PIT or
debugger emulation and has not been qualified on the T23. Pending keyboard
delivery and teardown are defined by the device session (linger above,
`DEV_END` order and 8042 drain); desktop focus routing is described in the
device document.

Keep the profile experimental until its actual DOS/DPMI/file/video lifecycle,
device ownership conflicts and target hardware have been qualified together.
