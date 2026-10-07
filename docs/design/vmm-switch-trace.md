# Optional CVSESSION VM-switch trace

Status: opt-in diagnostic implemented; production fault remains under investigation.
Ordinary CVSESSION builds and runtime behavior remain unchanged.

## Why this trace

The latest recorder run froze on V86 #UD at `CS:IP=3146:1186`,
`SS:SP=1180:0200`. Its final recorded `Simulate_Far_Ret` returned to
`1191:0290` from a `VMFORK` `VM_EXIT` path; no recorded Jemm far call or return
targeted `3146:1186`. A separate snapshot shows VM0's saved frame at
`CS:IP=1180:B67F`, `SS:SP=1180:F544`, `EFlags=33087`; the nearby shell code
does `MOV SS`, `MOV SP`, `STI`, then `RETF`. The diagnostic must therefore
include each VM's virtual interrupt state, especially guest IF, alongside the
three client-frame snapshots. The fault appears after the VM-exit switch
path. Recording the live frame immediately before save, the selected VM's
saved frame, and the resulting live frame immediately after load can show
whether the destination frame was already bad or changed during save/load.

The hook belongs at the real `switch_to` label, after urgent-switch metadata
is finalized and before the running frame is saved, and immediately after
the destination frame's `rep movsd`. Do not record scheduling candidates that
are gated or never switch. Freeze after a saved destination frame has
`CS=3146` or after the loaded live frame has `CS=3146`; this segment value is
only a diagnostic filter, never production policy.

## Binary layout

The optional in-memory block begins with `CVSWTR01` and a 32-byte header
followed by 32 records. All fields are little-endian 32-bit values. The trace
block is 3,104 bytes; one adjacent 4-byte scratch word stores the live guest-IF
query result between the existing XMS gate and the actual-switch record.

| Header offset | Field |
| ---: | --- |
| 0 | Eight-byte magic `CVSWTR01` |
| 8 | Next record index, modulo 32 |
| 12 | Valid record count, capped at 32 |
| 16 | Monotonic record sequence |
| 20 | Freeze reason bitmask: bit 0 saved destination CS matched; bit 1 loaded CS matched |
| 24 | In-progress record index for the pre/post pair |
| 28 | In-progress marker (0 or 1) |

Each 96-byte record contains 24 dwords:

| Record offset | Field |
| ---: | --- |
| 0 | Sequence |
| 4, 8 | Old VM id, next VM id |
| 12, 16 | Old VM state, next VM state |
| 20 | EBP pointer to the active Jemm client frame |
| 24–40 | Old live frame: CS, IP, SS, SP, EFlags |
| 44–60 | Next VM's saved frame: CS, IP, SS, SP, EFlags |
| 64 | Old live guest-IF state from Host_Scheduler_Profile query bit 3 |
| 68 | Next VM's saved `VMREC.cv` first dword (`CvGuestIF`) |
| 72 | Next VM's `cv_valid` marker; zero means saved CV state is not authoritative |
| 76–92 | Live frame after loading the next VM: CS, IP, SS, SP, EFlags |

The header is exactly 32 bytes and records start at offset 32. The live
guest-IF query is read-only; `CvGuestIF` is the first dword in the saved CV
state, and `cv_valid` determines whether the saved state will be restored or
inherited. A pre-switch
snapshot creates the record and captures the first two frames. The post-load
snapshot completes that same record and captures the live frame. If the
pre-snapshot matches the diagnostic CS, freeze reason bit 0 is set while the
post-load capture is still allowed to complete; a loaded-frame match sets bit
1. Once frozen, later switch attempts do not overwrite the ring.

## Source contract and decision

At the pinned Jemm revision, `Client_Reg_Struc` is 76 bytes; EIP, CS, EFlags,
ESP, and SS are at offsets 40, 44, 48, 52, and 56. CVSESSION declares
`VMM_FRAME=76`, saves the active frame with `rep movsd`, and restores the
selected VM frame the same way. Jemm then returns through `IRETD`. The
[Intel Software Developer's Manual](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366821.pdf)
describes protected-mode interrupt handlers returning to virtual-8086 mode
with IRET; the relevant frame is therefore the actual processor frame that
CVSESSION copies, not just its separately saved virtual PIC state. The pinned
[Jemm JEMM32 source](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/JEMM32.ASM)
defines the client frame and its return path. CVSESSION's `switch_to` save and
load sequence is in [session_vmm.inc](../../src/vm/session_vmm.inc).

Compile only with `scripts/build_vm_session.sh --switch-trace`. The build
manifest records this diagnostic option and the include hash. The include
wraps its two captures with `PUSHFD/PUSHAD` and restores both, performs only
bounded writes to the trace block, and does not alter the selected VM, frame,
CR3, or control flow. No logging or device I/O is added. The regular build
does not include the code or block.

## Stack extension, version 2

The `music-switch-runtime2` failure restored VM0 to `1180:B68A`,
`5A52:F258` with identical saved and loaded flags. `B68A` is the shell's
`RETF`, so checking the frame alone does not check the actual return address.
Intel's RET specification above requires reading that address from the guest
stack. Before the next probe, extend the diagnostic to `CVSWTR02`, retaining
the 32-byte header and first 96 bytes of each record. Each record becomes
128 bytes: offsets 96–111 capture the outgoing stack's first 16 bytes before
its pages are unmapped; offsets 112–127 capture the incoming stack after its
pages are mapped. Compute each guest byte address with 16-bit SP wrap and
`SS * 16`. These are bounded reads of mapped guest memory, never writes to it.
This distinguishes a valid frame with damaged stack contents from a frame
copy defect. Ordinary builds still exclude the diagnostic entirely.
