# OPENGEM-016 — Design Document: 16-bit Execution Layer for CiukiOS

**Status:** Design (no-code). Implementation frozen until this document is approved.
**Date:** 2026-04-19
**Baseline:** CiukiOS Alpha v0.8.7
**Scope owner:** the 16-bit execution layer required to natively run `gem.exe`, with forward-looking compatibility for Windows DOS-based (3.x → 9x/ME).

---

## 1. Purpose

OPENGEM-007 through OPENGEM-015 delivered full observability of the OpenGEM runtime and staged binary loading up to — and including — the full MZ header of `gem.exe`. The pipeline now stops at the hard architectural boundary:

> `shell_run_staged_image()` rejects 16-bit MZ with `[dosrun] mz dispatch=pending reason=16bit`.

OPENGEM-016 is the design phase for the subsystem that removes this boundary. It is **explicitly not an implementation phase**. Implementation will be broken into OPENGEM-017+ milestones that each satisfy the CiukiOS single-session discipline.

---

## 2. Long-term objective

CiukiOS will support the **Windows DOS-based family** as a first-class long-term target:

- Windows 1.x / 2.x
- Windows 3.0 real mode
- Windows 3.1 Standard mode
- Windows 3.1 / WfW 3.11 Enhanced 386
- Windows 95
- Windows 98
- Windows ME

### Non-goals (permanent)

The following are **out of scope forever** for the execution layer designed here:

- Windows NT 3.x / 4.0
- Windows 2000
- Windows XP / Vista / 7 / 8 / 10 / 11
- ReactOS

Rationale: Windows NT and its descendants do not run on DOS; they are independent OS kernels with their own loader (NTLDR/BOOTMGR), driver model (WDM), HAL, filesystem (NTFS), and kernel PE loader. Supporting them would require a second parallel project, not an extension of CiukiOS's DOS-like runtime.

---

## 3. Strategic decision

### 3.1 Candidate strategies considered

| Strategy | Mechanism | Verdict |
|---|---|---|
| **A. Native v8086 monitor** | CPU executes 16-bit opcodes directly in virtual-8086 mode; stage2 traps `INT`/`IN`/`OUT`/faults | **CHOSEN** |
| B. DPMI server only | Re-host MZ as 32-bit DPMI client | **REJECTED** — useless for pure 16-bit binaries including `gem.exe` and all Win16 code |
| C. Software 16-bit emulator | Interpret/JIT 8086 opcodes inside stage2 | **REJECTED** — cannot scale to Win 3.1 Enhanced or Win9x, which require real protection traps and real hardware timing |

### 3.2 Why A (v8086) is mandatory for the chosen scope

Every Windows DOS-based release — from 3.0 through ME — either runs its own v8086 monitor on top of DOS, or is launched from a DOS that must itself be able to host v8086 tasks during installation (Win9x setup enters protected mode but initially executes real-mode DOS code via v8086 during its early bootstrap). A CiukiOS path that cannot host v8086 cannot host any of these targets. Strategy A is therefore the only strategy consistent with the declared long-term objective.

### 3.3 Long mode constraint

Stage2 runs in **x86_64 long mode**, where virtual-8086 mode **does not exist**. The design must include a mode-switch subsystem:

```
Long mode (stage2 host)
    │  ↕ mode_switch
32-bit protected mode (v8086 host task)
    │  ↕ VM entry
Virtual-8086 (16-bit guest, DOS/Windows/GEM)
```

This three-level architecture is the defining cost of OPENGEM-016. It is also the minimum-viable foundation for every tier below.

### 3.4 Where DPMI fits

DPMI is **not** the execution layer. It is a **service layer** provided on top of the v8086 monitor, starting at tier T3. It is implemented as a host-side INT 31h handler and a host-managed descriptor pool. No Windows target below T3 needs it; every target at or above T3 requires it.

---

## 4. Compatibility tier map

Each tier is a distinct CiukiOS milestone. Each must land as an incremental one-session branch (OpenGEM discipline). Tiers are a specification contract, not a schedule.

| Tier | Target | New compatibility surface vs previous tier |
|---|---|---|
| **T0** | `gem.exe` (MZ 16-bit) | v8086 host, INT 21h minimal (read, write, exec, exit), INT 10h text + mode 13h, INT 16h keyboard, INT 33h mouse, BIOS data area |
| **T1** | Windows 1.x / 2.x | No additions required beyond T0; used as canary for Windows-shape workloads |
| **T2** | Windows 3.0 real mode | INT 21h completion (LFN-less), INT 2Fh multiplex minimal |
| **T3** | Windows 3.1 Standard | DPMI 0.9 host (INT 31h), 286 protected-mode transitions (emulated via 32-bit PE trap-and-emulate), LDT management |
| **T4** | WfW 3.11 Enhanced 386 | Stable PIC 8259 + PIT 8253 timing, VxD negotiation handshake (no VxD execution — negotiated fallback), VGA register fidelity |
| **T5** | Windows 95 / 98 | FAT32 + LFN, INT 13h extensions (AH=41h/42h), VESA VBE 2.0, PS/2 8042 full, IDE/ATA real, real→protected bootstrap preserved |
| **T6** | Windows ME | Minimal ACPI surface, FAT32 boot without real-mode driver dependency |

### 4.1 Parallel non-Windows targets

These are unaffected by OPENGEM-016 but share the v8086 foundation:

- `docs/roadmap-ciukios-doom.md` — DOOM DOS binary path.
- Future DOS software catalog.

DOOM targets and Windows targets are allowed to advance independently once the T0 foundation is in place.

---

## 5. Architectural contract

### 5.1 Three-level execution model

```
┌─────────────────────────────────────────────────────────────┐
│  Stage2 long-mode host                                      │
│  - services ABI (FAT, serial, video, input)                 │
│  - v8086 monitor orchestrator                               │
│  - INT dispatcher (host-side handler table)                 │
│  - descriptor/memory allocator                              │
└──────────────┬──────────────────────────────────────────────┘
               │ mode_switch (IDT shim + compatibility CS)
               ▼
┌─────────────────────────────────────────────────────────────┐
│  32-bit protected-mode compatibility host                   │
│  - GDT with v8086 TSS                                       │
│  - trap handlers (#GP, #PF, #UD, INT)                       │
│  - bridge back to long-mode host for services               │
└──────────────┬──────────────────────────────────────────────┘
               │ IRET into VM task (EFLAGS.VM=1)
               ▼
┌─────────────────────────────────────────────────────────────┐
│  Virtual-8086 guest                                         │
│  - real-mode addressing (seg:off, 1 MiB + HMA)              │
│  - 16-bit DOS binary / Windows kernel                       │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 INT dispatch contract

All guest software interactions with the OS go through interrupts. The host-side dispatcher is the single authority. Handlers are registered per-vector, and each tier may add handlers but **must not remove or silently rewrite** existing ones.

```
guest INT N
    → #GP (because EFLAGS.VM=1 and IOPL<3 for most vectors)
    → PE trap handler decodes the INT N
    → host INT dispatcher looks up handler[N]
    → handler reads/writes guest registers via the trap frame
    → IRET back into the guest
```

Handler signature (C pseudo-prototype, for OPENGEM-017+):

```c
typedef struct vm_trap_frame {
    u32 eax, ebx, ecx, edx, esi, edi, ebp;
    u16 cs, ds, es, fs, gs, ss;
    u32 eip, esp, eflags;
} vm_trap_frame;

typedef void (*int_handler)(vm_trap_frame *frame);
```

### 5.3 Memory model

- Guest sees the first 1 MiB + HMA (up to 1 MiB + 64 KiB − 16 B) as physically addressable.
- Host reserves a 1 MiB physical region mapped into the v8086 task's linear address space at virtual 0x00000000–0x000FFFFF.
- Above 1 MiB the guest is blind; DPMI (T3+) bridges this.
- Stage2 long-mode memory is untouched by guest writes (paging isolates them).
- The existing `SHELL_RUNTIME_COM_ADDR = 0x600000` and staged buffers are **host-side** regions; they are copied into the v8086 guest's conventional memory at dispatch time, not mapped.

### 5.4 Services bridge

INT 21h, INT 10h, INT 13h, INT 16h, INT 33h, INT 2Fh, INT 31h all live on the host side and call into existing CiukiOS services (`fat_*`, `serial_write`, `gfx_*`, `mouse_*`). The v8086 never touches CiukiOS kernel memory directly.

This is the design point that makes Windows 9x feasible: Windows 9x's VMM assumes DOS is "real" enough to answer its INT calls deterministically. Because CiukiOS backs those calls with native long-mode code, the answers are already deterministic — we are not an emulator; we are a host.

### 5.5 Observability (mandatory, from day 1)

Every guest entry and exit emits a marker disjoint from the OpenGEM observability set:

```
vm86: enter task=<handle> cs=<h16>:<h16>ip=<h16> ss=<h16>:<h16>sp=<h16>
vm86: int vec=<h8> ah=<h8> al=<h8>
vm86: svc vec=<h8> name=<token> status=<ok|eflag-cf|unhandled>
vm86: exit task=<handle> reason=<int20|int21-4c|fault|host-abort> errorlevel=<h8>
vm86: fault kind=<gp|pf|ud|ts|ts-sel> eip=<h32>
```

No tier may skip these. They are the debugging backbone for Windows-era work.

---

## 6. Deliverable shape per milestone

OPENGEM-017 through at least OPENGEM-030 are the v8086 road. Each one must satisfy:

1. **One dedicated branch.**
2. **No merge without explicit approval.**
3. **Alpha v0.8.7 baseline** unless the user requests a bump.
4. **New static gate** with measurable counts (`N OK / 0 FAIL`).
5. **Existing regression green** — no existing test may be removed or silenced.
6. **Handoff + diary entry.**
7. **Incremental observability markers.** No invisible changes to the VM state machine.

### 6.1 Proposed phase sequence (subject to revision)

| Phase | Scope |
|---|---|
| OPENGEM-017 | Long-mode ↔ 32-bit PE mode-switch scaffold (no v8086 yet) — observability only |
| OPENGEM-018 | GDT + 32-bit PE TSS + IDT shim for trap frame capture |
| OPENGEM-019 | V8086 entry: empty guest task that immediately halts; verify re-entry to long mode |
| OPENGEM-020 | INT dispatcher skeleton + unhandled-vector logging |
| OPENGEM-021 | INT 21h AH=4Ch exit path (hello-world exit) |
| OPENGEM-022 | INT 21h read/write console + INT 20h |
| OPENGEM-023 | INT 10h text output (AH=0Eh first) |
| OPENGEM-024 | `gem.exe` boots to first INT — **T0 reached** |
| OPENGEM-025+ | T1 → T6, one milestone per tier or per sub-surface (DPMI, VGA, PIT, etc.) |

The sequence is not a schedule. Each phase lands when it is green.

---

## 7. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Long-mode ↔ PE transition bugs (paging mismatch, TSS corruption) | Dedicated phase OPENGEM-017 with pure observability — no guest code runs yet |
| Win9x requires high-fidelity PIC/PIT timing | Phase T4 explicitly validates timing as a gate before touching Win9x code |
| DPMI host complexity underestimated | Phase T3 is scoped to DPMI 0.9 only; 1.0 deferred |
| Scope creep into Windows NT territory | Non-goal declared here; any Windows NT request is explicitly rejected |
| V8086 not available in long mode | Architecturally addressed: mode-switch to 32-bit PE is the first deliverable |
| Regression drift across OpenGEM phases | Mandatory retention of every existing OpenGEM gate; OPENGEM-017+ must all keep the 17-gate stack green |

---

## 8. Approvals required before OPENGEM-017 begins

1. **This document reviewed and approved by the user.**
2. **Strategy A (v8086 via 32-bit PE mode-switch) confirmed.**
3. **Non-goal (Windows NT+ family) confirmed permanently out of scope.**
4. **Tier map T0–T6 confirmed as the baseline compatibility ladder.**

Until all four approvals are recorded, no code from OPENGEM-017+ will be written.

---

## 9. References

- `docs/opengem-preload-probe.md`
- `docs/opengem-native-dispatch.md`
- `docs/opengem-mz-probe.md`
- `docs/roadmap-ciukios-doom.md`
- `docs/roadmap-windows-dosbased.md` (new — companion to this document)
- `CLAUDE.md` — Project North Star update (new — see §2 above)
