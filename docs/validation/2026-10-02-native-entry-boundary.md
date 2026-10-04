# Native CPL3 entry boundary audit

This is a source audit of pinned Jemm `e96bb6bb80cbdc3e6585544db79a6b133a5566dd`
with the current CiukiOS scheduler patch applied. No native process was run,
and no runtime patch is claimed by this audit.

## Primary sources

- [Intel 80386 interrupt procedures, section 9.6](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s09_06.htm): a privilege transition uses the TSS inner stack; interrupt frames and exception error codes must be preserved for IRET.
- [Intel page protection, section 6.4](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s06_04.htm): user access requires the effective permissions of both page-directory and page-table entries; descriptor-table and inner-stack accesses use supervisor protection.
- [Intel I/O protection, section 8.3](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s08_03.htm): IOPL and the TSS I/O-permission bitmap jointly control port access.
- [Intel debug exceptions, section 12.3](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s12_03.htm): TF is sampled at the beginning of an instruction; the handler must check the saved privilege and explicitly maintain single stepping across software interrupts.
- [Intel coprocessor control, sections 11.1.3–11.1.5](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s11_01.htm): CR0.TS causes exception 7 for x87 ESC instructions; WAIT checks TS only when CR0.MP is also set. CLTS and MOV CR0 are privileged.
- [Pinned Jemm monitor](https://raw.githubusercontent.com/Baron-von-Riedesel/Jemm/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/JEMM32.ASM), [initialization](https://raw.githubusercontent.com/Baron-von-Riedesel/Jemm/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/INIT.ASM), and [JLOAD services](https://raw.githubusercontent.com/Baron-von-Riedesel/Jemm/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/VMM.ASM) are the implementation checked below.

## Concrete boundary dependencies

`V86_Monitor` first classifies entry by comparing ESP with two fixed positions
below `?TOS`. Those positions describe V86 frames, which include the four
saved V86 segment registers. A protected CPL3-to-CPL0 transition instead
pushes EIP, CS, EFLAGS, ESP and SS, with an additional error code only for
the relevant exceptions. With the current TSS stack, a CPL3 entry therefore
falls into `@@Reentered`. That path admits ordinary ring-0 IRQs only when
the saved CS is `FLAT_CODE_SEL` and the stack matches `dwStackR0`; a user CS
goes to `handle_exception_r0`. It is not a native process dispatch path.

The monitor's entry stubs also normalize vectors differently: vectors 0–7
duplicate a stack word as a fake error code; other exceptions may have a
CPU-provided error code. A new CPL3 router must know the vector's frame form
before locating CS/EFLAGS. It must not inspect one guessed offset for all
vectors, nor expose generic software INT gates which let a client manufacture
a frame without the error code expected for a hardware exception.

`INIT.ASM` builds generic DPL3 gates. `Set_PM_Int_Vector` changes only their
selector and offset, retaining their access/type bytes. Native entry must
select an IDT whose unapproved software vectors have DPL0 and whose INT 80h
gate has an explicitly verified DPL3 interrupt-gate descriptor. Changing the
legacy shared IDT globally is not a safe compatibility-preserving shortcut:
VCPI/DPMI and JLOAD currently depend on it. An owned native IDT, restored
atomically before resuming the owner, is a viable design; the existing shared
IDT alone does not implement it.

The GDT lacks a dedicated flat native user-code/data pair, but descriptors
are not intrinsically unavailable. JLOAD `_Allocate_GDT_Selector` can allocate
empty slots. There is already a DPL3 descriptor at selector 40h for a small
BIOS data range; it is not a native flat address space. Native descriptors
need explicit ownership and must stay allocated while any saved process CS,
SS, DS, ES, FS or GS still references them. `_Free_GDT_Selector` itself does
not validate selector bounds, so trusted native code must validate them.

The existing TSS uses `SS0=FLAT_DATA_SEL` and `ESP0=?TOS`. Its I/O bitmap is
initialized for DOS/monitor operation and changed by the negotiated virtual
device profile. Merely setting native EFLAGS IOPL=0 does not deny native port
I/O: zero bitmap bits still permit individual ports. A native context needs
an owned all-denied I/O bitmap and kernel stack, with exact restoration before
DOS resumes. Saving only ESP0 is insufficient. A dedicated native TSS can
provide that ownership, but its descriptor busy bit and TR restoration must
also be handled explicitly.

JLOAD's current page services operate on Jemm's owner address space. A native
process CR3 must retain supervisor mappings for the monitor, dispatch code,
stack, IDT, GDT and TSS. Every return, exit and fault path must restore the
owner CR3 before using Jemm allocation services or resuming V86. Allocation
of supervisor blocks by NATPAGE is evidence for page ownership, not proof of
these mappings or transitions.

## Integration decision

A standalone INT 80h handler or user GDT pair is not a coherent native entry
implementation. The smallest runtime integration must bind together an
owned native IDT/TSS/stack, vector-specific normalized CPL3 frames, owned
callback registration, owner-CR3 return, and a complete fault/exit rollback.
It must also service or defer physical IRQs while native code executes;
disabling interrupts for an unbounded user program would reproduce system
lag and break audio.

Implement this as a separately selected Jemm/JLOAD adaptation after the
existing scheduler patch, with a versioned registration ABI. Its first
executable gate should run a bounded CN32 probe, report its saved CPL=3,
reject an unauthorized INT and port I/O, recover from a deliberate page
fault, then resume DOS with the original IDTR, TR, CR3, TSS/bitmap ownership
and interrupt state. Only a second gate with two isolated live processes,
preemption and reclamation can establish the requested native process model.
An unused router or successful assembly alone must not be reported as native
process support.

Native FPU state is not yet owned by a process. The integer-only gate must
therefore save CR0, set TS and MP before entering user code, treat exception
7 as a process fault, and restore the exact owner CR0 before its callbacks
resume. Setting TS alone would leave WAIT outside this guard when the owner
had MP clear. A real native FPU ABI needs separately owned save areas and
validated save/restore on every process transition; it must not reuse DOS's
current coprocessor state.

## Bounded execution implementation

`src/native/native_entry.asm` now supplies a trusted, synchronous CPL3
diagnostic boundary without altering Jemm's legacy IDT. It allocates owned
flat user code/data and TSS selectors through JLOAD, installs a private IDT
with normalized vector stubs and only INT 80h callable from user code, and
uses an owned 4-KiB ring-0 stack. The 104-byte native TSS has an I/O-map
offset beyond its descriptor limit, denying every user port operation.
Pinned JLOAD `_Allocate_GDT_Selector` takes flags at callee ESP+4, the low
descriptor dword at ESP+8 and the high dword at ESP+12. Callers therefore
push high, low, flags. The first guest integration returned a recoverable
GP with zero user steps because the initial caller order produced nonpresent
descriptors; all three allocation calls now follow the actual source ABI.

The caller supplies a separately validated private CR3 and exclusively owned
code/data/stack mappings. The runner saves CR3, CR4, IDTR, TR, the old TSS
access byte, DR6/DR7, caller stack, registers, selectors and flags. It clears
PGE during native execution to flush any global legacy user translations,
and restores the owner state before calling JLOAD to release its selectors.
It returns on native exit, exception or exhausted budget; it does not route
CPL3 frames into the V86 monitor.

For this diagnostic only, user IF is zero, IOPL is zero and TF is one, with
a mandatory budget of at most 256 instructions. Each debug trap and syscall
advances the budget, and every user return re-arms TF while retaining only
arithmetic/direction flags. This covers a user POPFD clearing TF because TF
was set at the beginning of that instruction. Hardware IRQs remain pending
for the bounded quantum; no PIC edge is acknowledged or discarded. This
design is a first execution gate, not asynchronous native scheduling, and
must be replaced by a resumable IRQ-aware process scheduler for general
application execution.

Pinned Jwasm assembled the standalone boundary with zero warnings/errors.
Full module linkage and guest execution are integration gates owned by the
main agent; assembly success alone proves no CPL3 sample has run.

## Resumable quanta

`native_entry_step` is a separate trusted ABI with a 136-byte context;
`native_entry_run` retains its original 60-byte ABI. The extended context
contains a magic value, fresh/suspended/terminal state, saved user registers,
EIP/ESP/EFLAGS, abstract flat segment modes, accumulated steps/quanta and a
binding to the process CR3. The manager retains all process-owned mappings
when the result is `NE_BUDGET`, and releases them only on exit/fault/stop.

Saved segment modes represent null, the owned flat user data selector or
the owned flat user code selector. The gate validates saved CS/SS ownership
and translates these modes into newly allocated selectors on each quantum;
it never restores numeric selectors which were freed at the prior yield.
Native flat programs do not acquire arbitrary legacy GDT/LDT ownership.

At a budget trap, the snapshot contains the CPU's post-instruction EIP and
registers. Syscalls execute before the budget check, so a report completing
the quantum is retained exactly once and resumes after INT 80h. Each
quantum still uses TF with a 256-step bound and returns the exact owner
CR3/IDTR/TR/stack/flags. The scheduler must run at most one native quantum
at a safe, nonnested Jemm-owner poll boundary, then service DOS/audio and
resume its normal V86 scheduling. The gate itself does not invoke DOS or
switch V86 machines.

This supports an incremental persistent process manager while retaining the
validated IRQ routing. It does not enable native IF, expose Jemm's generic
V86 IDT to CPL3, or claim an interrupt-driven native scheduler. Native FPU
ownership also remains incomplete; the TS+MP guard faults its first use.

## Persistent processes without a prior DOS VM fork

The first `scripts/qemu_test_native_resume.py` runs on the full image
(`build/tests/native-resume-20261002`, `native-resume-stage-20261002`)
failed at the first `VM_OP_NATIVE_START` with `VM_ERROR_VMM_INIT` (43h).
`nproc_start` required `vmm_installed`, but only `vmm_arm`, called from the
first `VMM_CREATE`, sets it and registers `jlm_poll` (Jemm profile callback
6), the only caller of `nproc_poll`. A DOS prompt opened with F4 runs in the
system VM without a fork, so a native process could never be started there.
`nproc_poll` also needs `vmm_kbase` (the InDOS/FAT gates) and the canonical
client frame, which only VM manager operations recorded.

`nproc_start` now requires `VMM_INIT` (`vmm_ready`), records the canonical
API frame and calls the idempotent `vmm_arm`. With one VM, `vmm_irq` only
counts ticks (`vmm_count < 2`), the same state as after the last forked VM
has exited. `NATRESUM.COM` sends `VMM_INIT` with the DOSMGR/MCB layout
exactly as `VMFORK.COM` does, and reports its failing stage. On a copy of
the full image with only `CVSESS.DLL` replaced, the test passed: two live
handles, both completed with private cookies and more than 1,200 steps,
STOP and generation guard, then `NATPAGE.COM` and the one-shot `NATIVE.COM`
(`build/tests/nres-fix/run1`).
